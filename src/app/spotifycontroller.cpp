#include "spotifycontroller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>

#include "palette.h"
#include "player.h"
#include "playlist.h"

namespace {

QString fileUrl(const QString &path) { return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString(); }

QString minutes(qint64 ms) {
    const qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString kindLabel(const QString &kind) {
    if (kind == "track")
        return QCoreApplication::translate("Spotify", "Track");
    if (kind == "album")
        return QCoreApplication::translate("Spotify", "Album");
    if (kind == "artist")
        return QCoreApplication::translate("Spotify", "Artist");
    return QCoreApplication::translate("Spotify", "Playlist");
}

// The second line of a row: who made it and where it is from.
QString detailOf(const Spotify::Item &i) {
    if (i.kind == "track")
        return i.album.isEmpty() ? i.artistText() : i.artistText() + " — " + i.album;
    if (i.kind == "album")
        return i.year.isEmpty() ? i.artistText() : i.artistText() + " · " + i.year;
    if (i.kind == "playlist")
        return i.owner.isEmpty() ? QString() : QCoreApplication::translate("Spotify", "by %1").arg(i.owner);
    return {};
}

bool defaultLaunch(const QStringList &command) {
    // Omarchy opens terminals with xdg-terminal-exec; $TERMINAL -e
    // otherwise.
    const QString xte = QStandardPaths::findExecutable("xdg-terminal-exec");
    if (!xte.isEmpty())
        return QProcess::startDetached(xte, command);
    const QString terminal = qEnvironmentVariable("TERMINAL");
    if (!terminal.isEmpty())
        return QProcess::startDetached(terminal, QStringList{"-e"} + command);
    return false;
}

} // namespace

// --- SpotifyArt -------------------------------------------------------------------

SpotifyArt::SpotifyArt(const QString &cacheDir, QObject *parent)
    : QObject(parent),
      m_dir(cacheDir.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omamp/spotify"
                : cacheDir) {}

QString SpotifyArt::oembedBase() {
    const QString env = qEnvironmentVariable("OMAMP_SPOTIFY_OEMBED");
    return env.isEmpty() ? QString("https://open.spotify.com/oembed") : env;
}

QString SpotifyArt::keyFor(const Spotify::Item &item) {
    if ((item.kind == "track" || item.kind == "album") && !item.albumId.isEmpty())
        return "album-" + item.albumId;
    return item.kind + '-' + item.id;
}

QString SpotifyArt::cachedFile(const Spotify::Item &item) const {
    if (!item.isValid())
        return {};
    const QString file = m_dir + '/' + keyFor(item) + ".jpg";
    return QFileInfo::exists(file) ? file : QString();
}

QString SpotifyArt::fileForPath(const QString &path) const {
    QString kind, id;
    if (!Spotify::parseReference(path, &kind, &id))
        return {};
    if (kind != "track") {
        // A whole playlist, album or artist: its own picture.
        const QString file = m_dir + '/' + kind + '-' + id + ".jpg";
        return QFileInfo::exists(file) ? file : QString();
    }
    QString album;
    {
        QMutexLocker lock(&m_mutex);
        album = m_trackAlbum.value(id);
    }
    const QString own = m_dir + "/track-" + id + ".jpg";
    if (!album.isEmpty() && QFileInfo::exists(m_dir + "/album-" + album + ".jpg")) {
        // A link by the track's id, so the queue finds the cover after a
        // restart too, before the album is known again.
        if (!QFileInfo::exists(own))
            QFile::link("album-" + album + ".jpg", own);
        return m_dir + "/album-" + album + ".jpg";
    }
    return QFileInfo::exists(own) ? QFileInfo(own).canonicalFilePath() : QString();
}

void SpotifyArt::remember(const Spotify::Item &item) {
    if (item.kind != "track" || item.albumId.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    m_trackAlbum.insert(item.id, item.albumId);
}

void SpotifyArt::want(const Spotify::Item &item) {
    if (!item.isValid())
        return;
    remember(item);
    const QString key = keyFor(item);
    if (m_pending.contains(key) || QFileInfo::exists(m_dir + '/' + key + ".jpg"))
        return;
    Want w;
    w.key = key;
    if (!item.imageUrl.isEmpty())
        w.image = QUrl(item.imageUrl);
    else if (m_failed.contains(key) || oembedBase() == "off")
        return;
    else
        w.web = (item.kind == "track" || item.kind == "album") && !item.albumId.isEmpty()
                    ? "https://open.spotify.com/album/" + item.albumId
                    : item.webUrl();
    m_pending.insert(key);
    m_queue << w;
    pump();
}

void SpotifyArt::pump() {
    while (m_running < 3 && !m_queue.isEmpty()) {
        const Want w = m_queue.takeFirst();
        ++m_running;
        if (w.image.isValid())
            download(w.key, w.image);
        else
            lookUp(w.key, w.web);
    }
}

void SpotifyArt::finishOne(const QString &key) {
    m_pending.remove(key);
    --m_running;
    pump();
}

void SpotifyArt::lookUp(const QString &key, const QString &webUrl) {
    QUrl url(oembedBase());
    QUrlQuery q;
    q.addQueryItem("url", webUrl);
    url.setQuery(q);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Omamp/" + QCoreApplication::applicationVersion());
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        reply->deleteLater();
        const QString thumb =
            QJsonDocument::fromJson(reply->readAll()).object().value("thumbnail_url").toString();
        if (reply->error() != QNetworkReply::NoError || thumb.isEmpty()) {
            m_failed.insert(key);
            finishOne(key);
            return;
        }
        // The slot stays taken for the picture itself.
        download(key, QUrl(thumb));
    });
}

void SpotifyArt::lookUpTitle(const QString &webUrl, std::function<void(const QString &)> done) {
    if (oembedBase() == "off") {
        QTimer::singleShot(0, this, [done] { done({}); });
        return;
    }
    QUrl url(oembedBase());
    QUrlQuery q;
    q.addQueryItem("url", webUrl);
    url.setQuery(q);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Omamp/" + QCoreApplication::applicationVersion());
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        done(reply->error() == QNetworkReply::NoError
                 ? QJsonDocument::fromJson(reply->readAll()).object().value("title").toString()
                 : QString());
    });
}

void SpotifyArt::download(const QString &key, const QUrl &url) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Omamp/" + QCoreApplication::applicationVersion());
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        reply->deleteLater();
        QImage image = QImage::fromData(reply->readAll());
        bool saved = false;
        if (reply->error() == QNetworkReply::NoError && !image.isNull() && QDir().mkpath(m_dir)) {
            if (image.width() > 600 || image.height() > 600)
                image = image.scaled(600, 600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QSaveFile out(m_dir + '/' + key + ".jpg");
            saved = out.open(QIODevice::WriteOnly) &&
                    image.convertToFormat(QImage::Format_RGB888).save(&out, "jpg", 88) && out.commit();
        }
        if (!saved)
            m_failed.insert(key);
        finishOne(key);
        if (saved)
            emit artReady(key);
    });
}

// --- SpotifyItemsModel ------------------------------------------------------------

SpotifyItemsModel::SpotifyItemsModel(SpotifyArt *art, QObject *parent) : QAbstractListModel(parent), m_art(art) {}

QHash<int, QByteArray> SpotifyItemsModel::roleNames() const {
    return {{KindRole, "kind"},       {ItemIdRole, "itemId"}, {UriRole, "uri"},         {NameRole, "name"},
            {ArtistsRole, "artists"}, {AlbumRole, "album"},   {DurationRole, "duration"}, {YearRole, "year"},
            {OwnerRole, "owner"},     {SectionRole, "section"}, {ArtRole, "art"},        {DetailRole, "detail"}};
}

QVariant SpotifyItemsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const Spotify::Item &i = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return i.name;
    case KindRole: return i.kind;
    case ItemIdRole: return i.id;
    case UriRole: return i.uri();
    case ArtistsRole: return i.artistText();
    case AlbumRole: return i.album;
    case DurationRole: return i.durationMs > 0 ? minutes(i.durationMs) : QString();
    case YearRole: return i.year;
    case OwnerRole: return i.owner;
    case SectionRole: return m_sections.value(index.row());
    case ArtRole: return m_art ? fileUrl(m_art->cachedFile(i)) : QString();
    case DetailRole: return detailOf(i);
    }
    return {};
}

void SpotifyItemsModel::setItems(const QList<Spotify::Item> &items, const QStringList &sections) {
    beginResetModel();
    const bool changed = items.size() != m_items.size();
    m_items = items;
    m_sections = sections;
    endResetModel();
    if (changed)
        emit countChanged();
}

QVariantMap SpotifyItemsModel::get(int row) const {
    QVariantMap m = m_items.value(row).toMap();
    m.insert("section", m_sections.value(row));
    m.insert("detail", detailOf(m_items.value(row)));
    return m;
}

QList<Spotify::Item> SpotifyItemsModel::tracks() const {
    QList<Spotify::Item> out;
    for (const Spotify::Item &i : m_items)
        if (i.kind == "track")
            out << i;
    return out;
}

int SpotifyItemsModel::trackIndex(int row) const {
    if (row < 0 || row >= m_items.size() || m_items.at(row).kind != "track")
        return -1;
    int n = 0;
    for (int i = 0; i < row; ++i)
        n += m_items.at(i).kind == "track";
    return n;
}

void SpotifyItemsModel::wantArt(int row) {
    if (m_art && row >= 0 && row < m_items.size())
        m_art->want(m_items.at(row));
}

void SpotifyItemsModel::artArrived(const QString &key) {
    for (int r = 0; r < m_items.size(); ++r)
        if (SpotifyArt::keyFor(m_items.at(r)) == key)
            emit dataChanged(index(r), index(r), {ArtRole});
}

// --- SpotifyPlayback ----------------------------------------------------------------

SpotifyPlayback::SpotifyPlayback(SpotifyController *controller) : m_c(controller) {
    m_poll.setInterval(m_pollMs);
    connect(&m_poll, &QTimer::timeout, this, &SpotifyPlayback::pollNow);
    // The bus answers this, not Spotify: it corrects the position's drift.
    m_position.setInterval(2000);
    connect(&m_position, &QTimer::timeout, this, [this] { m_c->mpris()->refreshPosition(); });
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        if (m_state != AudioEngine::Playing)
            return;
        emit positionChanged(position());
        // Near the end, look more often, so the next track follows soon.
        // setInterval restarts a running timer, so only on a change.
        const bool nearEnd = duration() > 0 && position() >= duration() - 3000;
        if (followsMpris()) {
            const int interval = nearEnd ? 500 : 2000;
            if (m_position.interval() != interval)
                m_position.setInterval(interval);
        } else {
            // While a start waits to be seen, a little sooner too.
            const int interval = nearEnd || m_starting ? qMin(1500, m_pollMs) : m_pollMs;
            if (m_poll.interval() != interval)
                m_poll.setInterval(interval);
        }
    });
    connect(m_c->mpris(), &Spotify::MprisWatch::changed, this, &SpotifyPlayback::onMpris);
    connect(m_c->mpris(), &Spotify::MprisWatch::availableChanged, this, &SpotifyPlayback::startFollowing);
}

void SpotifyPlayback::setTimings(int pollMs, int startTimeoutMs) {
    m_pollMs = pollMs;
    m_startTimeoutMs = startTimeoutMs;
    m_poll.setInterval(pollMs);
}

bool SpotifyPlayback::followsMpris() const { return m_c->mpris()->available(); }

bool SpotifyPlayback::isOurs(const Spotify::Playback &p) const {
    if (!p.valid)
        return false;
    if (m_context) {
        // The backend's state names the context; MPRIS doesn't, so there a
        // start counts once something new plays, and after that whatever
        // plays is the context's next track.
        if (!p.contextUri.isEmpty())
            return p.contextUri == m_contextItem.uri();
        if (!m_starting)
            return true;
        return p.playing && (!m_beforePlaying || p.track.name != m_beforeTitle || m_startedAt.elapsed() > 6000);
    }
    if (!p.track.id.isEmpty() && !m_track.id.isEmpty())
        return p.track.id == m_track.id;
    if (m_track.name.isEmpty() || p.track.name.compare(m_track.name, Qt::CaseInsensitive) != 0)
        return false;
    // Two tracks of the same name: the length tells them apart.
    return p.track.durationMs <= 0 || m_track.durationMs <= 0 || qAbs(p.track.durationMs - m_track.durationMs) <= 3000;
}

QVariantMap SpotifyPlayback::metaData() const {
    // A context entry shows the track Spotify is on; before the first one
    // is known, the playlist's own name.
    if (m_context && m_track.name.isEmpty())
        return {{"title", m_contextItem.name}, {"artist", m_contextItem.artistText()}, {"album", QString()}};
    return {{"title", m_track.name}, {"artist", m_track.artistText()}, {"album", m_track.album}};
}

qint64 SpotifyPlayback::position() const {
    if (m_state == AudioEngine::Playing && m_confirmed && m_since.isValid()) {
        const qint64 p = m_base + m_since.elapsed();
        return duration() > 0 ? qMin(p, duration()) : p;
    }
    return m_base;
}

void SpotifyPlayback::setBase(qint64 ms) {
    m_base = qMax<qint64>(0, ms);
    m_since.restart();
}

void SpotifyPlayback::setState(AudioEngine::State state) {
    if (state == m_state)
        return;
    m_state = state;
    if (state == AudioEngine::Playing) {
        m_tick.start();
        if (m_started && followsMpris())
            m_position.start();
    } else {
        m_tick.stop();
        m_position.stop();
    }
    emit stateChanged();
}

void SpotifyPlayback::load(const QString &path) {
    ++m_generation;
    m_poll.stop();
    m_tick.stop();
    m_position.stop();
    m_path = path;
    QString kind, id;
    Spotify::parseReference(path, &kind, &id);
    m_context = !kind.isEmpty() && kind != "track";
    m_shuffle = Spotify::queuePathShuffles(path);
    m_contextItem = m_context ? m_c->knownContext(kind, id) : Spotify::Item();
    m_track = m_context ? Spotify::Item() : m_c->knownTrack(id);
    if (!m_track.isValid()) {
        m_track = Spotify::Item();
        m_track.kind = "track";
        m_track.id = m_context ? QString() : id;
    }
    m_state = AudioEngine::Stopped;
    m_started = m_starting = m_confirmed = m_pausedByUs = m_askedOnce = false;
    m_base = 0;
    m_since.invalidate();
    emit metaDataChanged();
    emit durationChanged(m_track.durationMs);
    emit positionChanged(0);
}

void SpotifyPlayback::unload() {
    if (m_state == AudioEngine::Playing && m_started)
        m_c->runControl([](Spotify::Backend &b) { b.pause(nullptr); });
    ++m_generation;
    m_poll.stop();
    m_tick.stop();
    m_position.stop();
    m_path.clear();
    m_state = AudioEngine::Stopped;
    m_started = m_starting = m_confirmed = false;
}

void SpotifyPlayback::fail(const QString &detail) {
    const bool wasStarting = m_starting;
    m_starting = false;
    m_poll.stop();
    m_position.stop();
    if (wasStarting)
        emit bufferingChanged();
    setState(AudioEngine::Stopped);
    const QString path = m_path;
    // Later, so the Player isn't changing tracks inside our own call.
    QTimer::singleShot(0, this, [this, path, detail] {
        if (path == m_path)
            emit failed(path, detail);
    });
}

// Follows the track: through MPRIS when the backend publishes it (nothing
// asked of Spotify), else by reading its state through the command line
// every few seconds.
void SpotifyPlayback::startFollowing() {
    if (m_path.isEmpty() || !m_started) {
        m_poll.stop();
        m_position.stop();
        return;
    }
    if (followsMpris()) {
        m_poll.stop();
        if (m_state == AudioEngine::Playing)
            m_position.start();
        onMpris();
        // spotify_player's MPRIS has no id; a track whose name Omamp
        // doesn't know yet is confirmed by one read of the state.
        if (m_starting && m_track.name.isEmpty() && !m_askedOnce) {
            m_askedOnce = true;
            const int generation = m_generation;
            QTimer::singleShot(1500, this, [this, generation] {
                if (generation == m_generation && m_starting)
                    readState();
            });
        }
    } else {
        m_position.stop();
        if (m_starting)
            m_poll.setInterval(qMin(1500, m_pollMs));
        if (!m_poll.isActive())
            m_poll.start();
    }
}

void SpotifyPlayback::play() {
    if (m_path.isEmpty())
        return;
    if (!m_c->enabled()) {
        fail(tr("Spotify is off. Switch it on in Settings → Spotify."));
        return;
    }
    if (m_context ? m_contextItem.id.isEmpty() : m_track.id.isEmpty()) {
        fail(tr("not a Spotify track"));
        return;
    }
    m_pausedByUs = false;
    const int generation = m_generation;
    if (m_started && m_confirmed) {
        ++m_commands;
        m_lastCommand.start();
        m_c->runControl([](Spotify::Backend &b) { b.resume(nullptr); });
        m_since.restart();
        setState(AudioEngine::Playing);
        startFollowing();
        return;
    }
    m_started = true;
    m_starting = true;
    m_confirmed = false;
    m_startedAt.start();
    emit bufferingChanged();
    setState(AudioEngine::Playing);
    const QString id = m_track.id;
    const Spotify::Item context = m_contextItem;
    const bool contextEntry = m_context, shuffle = m_shuffle;
    if (m_context) {
        const Spotify::Playback before = m_c->mpris()->available() ? m_c->mpris()->state() : Spotify::Playback();
        m_beforeTitle = before.valid ? before.track.name : QString();
        m_beforePlaying = before.valid && before.playing;
    }
    QPointer<SpotifyPlayback> self(this);
    m_c->request(
        Spotify::Requests::Control, {},
        [id, context, contextEntry, shuffle](Spotify::Backend &b, Spotify::Result &r) {
            if (contextEntry)
                b.playContext(context, shuffle, &r.error);
            else
                b.playTrack(id, &r.error);
        },
        [self, generation](const Spotify::Result &r) {
            if (self && !r.error.isEmpty() && generation == self->m_generation)
                self->fail(r.error);
        });
    startFollowing();
}

void SpotifyPlayback::pause() {
    if (m_state != AudioEngine::Playing)
        return;
    setBase(position());
    m_pausedByUs = true;
    ++m_commands;
    m_lastCommand.start();
    if (m_started)
        m_c->runControl([](Spotify::Backend &b) { b.pause(nullptr); });
    setState(AudioEngine::Paused);
}

void SpotifyPlayback::stop() {
    if (m_started && m_state == AudioEngine::Playing)
        m_c->runControl([](Spotify::Backend &b) { b.pause(nullptr); });
    ++m_generation;
    m_pausedByUs = true;
    m_poll.stop();
    m_position.stop();
    const bool wasStarting = m_starting;
    m_started = m_starting = m_confirmed = false;
    if (wasStarting)
        emit bufferingChanged();
    setBase(0);
    setState(AudioEngine::Stopped);
    emit positionChanged(0);
}

void SpotifyPlayback::seek(qint64 ms) {
    ms = qBound<qint64>(0, ms, duration() > 0 ? duration() : ms);
    if (m_started && m_confirmed) {
        const qint64 from = position();
        ++m_commands;
        m_lastCommand.start();
        m_c->runControl([ms, from](Spotify::Backend &b) { b.seekTo(ms, from, nullptr); });
    }
    setBase(ms);
    emit positionChanged(ms);
}

void SpotifyPlayback::pollNow() {
    if (m_path.isEmpty())
        return;
    if (followsMpris()) {
        m_c->mpris()->refreshAll();
        return;
    }
    readState();
}

// One read of the backend's state through its command line: a Web API
// request, shared with any other read already waiting.
void SpotifyPlayback::readState() {
    if (m_polling || m_path.isEmpty())
        return;
    m_polling = true;
    const int generation = m_generation, commands = m_commands;
    QPointer<SpotifyPlayback> self(this);
    m_c->request(
        Spotify::Requests::Control, "playback",
        [](Spotify::Backend &b, Spotify::Result &r) { r.playback = b.playback(&r.error); },
        [self, generation, commands](const Spotify::Result &r) {
            if (!self)
                return;
            self->m_polling = false;
            if (generation == self->m_generation && commands == self->m_commands)
                self->onPolled(r.playback, r.error);
        });
}

void SpotifyPlayback::onMpris() {
    if (m_path.isEmpty() || !m_started || !followsMpris())
        return;
    onPolled(m_c->mpris()->state(), {});
}

void SpotifyPlayback::onPolled(const Spotify::Playback &p, const QString &error) {
    if (!error.isEmpty()) {
        if (m_starting && m_startedAt.elapsed() > m_startTimeoutMs)
            fail(error);
        return;
    }
    m_c->noteNowPlaying(p);
    const bool ours = isOurs(p);
    if (m_starting) {
        if (ours && p.playing) {
            m_starting = false;
            m_confirmed = true;
            emit bufferingChanged();
            // The playback state has the album and its picture; MPRIS has
            // no ids, so what Omamp knew stays.
            const qint64 oldDuration = m_track.durationMs;
            if (m_context) {
                m_track = p.track;
            } else if (!p.track.id.isEmpty() && !p.track.albumId.isEmpty()) {
                m_track = p.track;
            } else {
                if (m_track.name.isEmpty())
                    m_track.name = p.track.name;
                if (m_track.artists.isEmpty())
                    m_track.artists = p.track.artists;
                if (m_track.album.isEmpty())
                    m_track.album = p.track.album;
                if (p.track.durationMs > 0)
                    m_track.durationMs = p.track.durationMs;
                if (m_track.imageUrl.isEmpty())
                    m_track.imageUrl = p.track.imageUrl;
            }
            if (m_track.durationMs <= 0)
                m_track.durationMs = oldDuration;
            emit metaDataChanged();
            emit durationChanged(m_track.durationMs);
            setBase(p.progressMs);
            if (followsMpris())
                m_position.start();
            m_c->noteStarted(p);
        } else if (m_startedAt.elapsed() > m_startTimeoutMs) {
            fail(m_c->startFailureHint());
        }
        return;
    }
    if (!m_confirmed || m_state == AudioEngine::Stopped)
        return;
    const qint64 length = duration();
    const bool nearEnd = length > 0 && position() >= length - 4000;
    // Just after our own command the backend may not have caught up;
    // spotify_player refreshes its MPRIS state once a second.
    const int settle = followsMpris() ? 2500 : 1500;
    const bool settled = !m_lastCommand.isValid() || m_lastCommand.elapsed() > settle;
    if (!ours) {
        if (m_state != AudioEngine::Playing)
            return;
        if (nearEnd) {
            finishTrack();
        } else if (settled) {
            // Something else plays now (someone chose it in Spotify), or
            // nothing does.
            setBase(position());
            m_pausedByUs = true;
            setState(AudioEngine::Paused);
            if (p.valid)
                m_c->setMessage(tr("Spotify is playing something else now, so Omamp paused its queue."));
        }
        return;
    }
    if (m_context && !p.track.name.isEmpty() &&
        (p.track.name != m_track.name || (!p.track.id.isEmpty() && p.track.id != m_track.id))) {
        // Spotify moved on to the context's next track.
        takeTrack(p);
        if (p.playing && m_state != AudioEngine::Playing) {
            m_pausedByUs = false;
            setState(AudioEngine::Playing);
        }
        return;
    }
    if (p.playing) {
        if (m_state != AudioEngine::Playing && settled) {
            m_pausedByUs = false;
            setState(AudioEngine::Playing);
        }
        if (m_state == AudioEngine::Playing && settled)
            setBase(p.progressMs);
        return;
    }
    if (m_state != AudioEngine::Playing || m_pausedByUs)
        return;
    // Stopped at the end (or back at the start after it) is the end;
    // anywhere else it was paused from another device.
    if (length > 0 && (p.progressMs >= length - 2500 || (p.progressMs == 0 && nearEnd))) {
        finishTrack();
    } else if (settled) {
        setBase(p.progressMs);
        setState(AudioEngine::Paused);
    }
}

void SpotifyPlayback::takeTrack(const Spotify::Playback &p) {
    m_track = p.track;
    if (m_track.kind.isEmpty())
        m_track.kind = "track";
    if (m_track.isValid())
        m_c->rememberTracks({m_track});
    emit metaDataChanged();
    emit durationChanged(m_track.durationMs);
    setBase(p.progressMs);
    emit positionChanged(position());
}

void SpotifyPlayback::finishTrack() {
    ++m_generation;
    m_poll.stop();
    m_position.stop();
    m_started = m_starting = m_confirmed = false;
    setBase(duration());
    m_state = AudioEngine::Stopped;
    m_tick.stop();
    emit finished();
}

// --- SpotifyController ------------------------------------------------------------

QString SpotifyController::View::key() const {
    if (section == "tracks")
        return "tracks:" + container.kind + ':' + container.id;
    if (section == "search")
        return "search:" + search.toLower();
    return "library:" + section;
}

SpotifyController::SpotifyController(const QString &settingsPath, const QString &artDir, QObject *parent)
    : QObject(parent), m_settingsPath(settingsPath.isEmpty() ? Spotify::Settings::defaultPath() : settingsPath),
      m_settings(Spotify::Settings::load(m_settingsPath)), m_art(artDir), m_items(&m_art), m_blend(&m_art),
      m_playback(this), m_launch(defaultLaunch) {
    const auto envMs = [](const char *name, int fallback) {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue(name, &ok);
        return ok && v >= 0 ? v : fallback;
    };
    m_requests.setGapMs(envMs("OMAMP_SPOTIFY_GAP_MS", 300));
    m_warmupMs = envMs("OMAMP_SPOTIFY_WARMUP_MS", 6000);
    m_slowMs = envMs("OMAMP_SPOTIFY_SLOW_MS", 4000);
    makeBackend();
    // What Spotify refused today, per client ID, beside the pictures.
    m_refusals.setFile(m_art.dir() + "/refused.json");
    m_refusals.setClientId(Spotify::SpotifyPlayerBackend::effectiveClientId());

    connect(&m_art, &SpotifyArt::artReady, this, [this](const QString &key) {
        m_items.artArrived(key);
        m_blend.artArrived(key);
        ++m_artRevision;
        emit artRevisionChanged();
        if (m_nowPlaying.value("artKey").toString() == key) {
            m_nowPlaying.insert("art", fileUrl(m_art.dir() + '/' + key + ".jpg"));
            emit nowPlayingChanged();
        }
        Spotify::Item playing = knownTrack(m_playback.trackId());
        if (playing.isValid() && SpotifyArt::keyFor(playing) == key)
            emit playingArtChanged();
    });

    connect(&m_mpris, &Spotify::MprisWatch::changed, this, &SpotifyController::noteMpris);
    connect(&m_mpris, &Spotify::MprisWatch::availableChanged, this, &SpotifyController::statusChanged);

    m_signInWait.setInterval(3000);
    connect(&m_signInWait, &QTimer::timeout, this, [this] {
        if (m_backend->signedIn() || ++m_signInChecks > 100) {
            m_signInWait.stop();
            refreshStatus();
        }
    });
    m_blendTimer.setSingleShot(true);
    m_blendTimer.setInterval(400);
    connect(&m_blendTimer, &QTimer::timeout, this, &SpotifyController::runBlend);
    m_paletteTimer.setSingleShot(true);
    m_paletteTimer.setInterval(400);
    connect(&m_paletteTimer, &QTimer::timeout, this, &SpotifyController::runPaletteSearch);

    // While a list loads: the seconds, "slow", the rate limit's countdown.
    m_loadTick.setInterval(1000);
    connect(&m_loadTick, &QTimer::timeout, this, [this] {
        if (m_loading) {
            m_loadingSlow = m_loadClock.elapsed() >= m_slowMs;
            checkRateLimit(false, {});
        } else if (m_rateLimited && retryIn() == 0 && !m_retryTimer.isActive()) {
            setRateLimited(false);
        }
        if (!m_loading && !m_rateLimited)
            m_loadTick.stop();
        emit loadingChanged();
    });
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] { load(m_view, true, true); });

    followMpris();
    if (m_settings.enabled)
        refreshStatus();
}

SpotifyController::~SpotifyController() {
    *m_alive = false;
    m_requests.cancelAll();
}

void SpotifyController::waitForIdle() {
    m_requests.waitForIdle();
    QCoreApplication::processEvents();
}

void SpotifyController::makeBackend() {
    m_backend = Spotify::Backend::create(m_settings.backend);
    m_requests.cancelAll();
    m_requests.setBackend(m_backend);
}

void SpotifyController::followMpris() {
    m_mpris.setService(m_settings.enabled ? m_backend->mprisName() : QString());
}

void SpotifyController::saveSettings() {
    m_settings.save(m_settingsPath);
    emit settingsChanged();
}

void SpotifyController::setEnabled(bool on) {
    if (on == m_settings.enabled)
        return;
    m_settings.enabled = on;
    saveSettings();
    followMpris();
    if (on) {
        refreshStatus();
        // The list loads when the view is shown, not now: switching on
        // shouldn't fire every request at once.
        m_viewStale = true;
        if (m_viewVisible)
            load(m_view);
    } else {
        m_requests.cancelAll();
        m_retryTimer.stop();
        setLoading(false);
        m_items.setItems({});
        m_blend.setItems({});
        if (m_palette)
            m_palette->setExternalResults("Spotify", {}, {});
    }
}

void SpotifyController::setBackendId(const QString &id) {
    if (id == m_settings.backend || !Spotify::Backend::ids().contains(id))
        return;
    m_settings.backend = id;
    makeBackend();
    followMpris();
    m_status = Status();
    m_checking = false;
    m_playlists.clear();
    m_cache.clear();
    m_items.setItems({});
    setLoading(false);
    saveSettings();
    emit playlistsChanged();
    emit statusChanged();
    emit clientIdChanged();
    if (m_settings.enabled) {
        refreshStatus();
        m_back.clear();
        m_view = View();
        m_viewStale = true;
        if (m_viewVisible)
            load(m_view);
    }
}

void SpotifyController::setBlendSearch(bool on) {
    if (on == m_settings.blendSearch)
        return;
    m_settings.blendSearch = on;
    saveSettings();
    blendQuery(on ? m_blendText : QString());
}

void SpotifyController::setBlendPalette(bool on) {
    if (on == m_settings.blendPalette)
        return;
    m_settings.blendPalette = on;
    saveSettings();
    paletteTextChanged();
}

void SpotifyController::setStartDaemon(bool on) {
    if (on == m_settings.startDaemon)
        return;
    m_settings.startDaemon = on;
    saveSettings();
}

void SpotifyController::setPalette(Palette *palette) {
    m_palette = palette;
    if (!palette)
        return;
    connect(palette, &Palette::textChanged, this, &SpotifyController::paletteTextChanged);
    connect(palette, &Palette::externalActivated, this, [this](const QString &source, const QString &reference) {
        if (source == "Spotify")
            activateUri(reference);
    });
}

void SpotifyController::setMessage(const QString &message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void SpotifyController::request(Spotify::Requests::Lane lane, const QString &key, Spotify::Requests::Work work,
                                Spotify::Requests::Done done) {
    m_requests.submit(lane, key, std::move(work), this, [this, lane, done](const Spotify::Result &r) {
        if (lane == Spotify::Requests::Data && !r.cancelled)
            checkRateLimit(!r.ok(), r.error);
        if (done)
            done(r);
    });
}

void SpotifyController::runControl(std::function<void(Spotify::Backend &)> work) {
    m_requests.submit(
        Spotify::Requests::Control, {}, [work](Spotify::Backend &b, Spotify::Result &) { work(b); }, this, {});
}

void SpotifyController::rememberTracks(const QList<Spotify::Item> &tracks) {
    for (const Spotify::Item &t : tracks)
        if (t.kind == "track" && t.isValid()) {
            Spotify::Item merged = t;
            const Spotify::Item old = m_known.value(t.id);
            if (merged.imageUrl.isEmpty())
                merged.imageUrl = old.imageUrl;
            m_known.insert(t.id, merged);
            m_art.remember(t);
        }
}

// --- Rate limits ------------------------------------------------------------------

int SpotifyController::retryIn() const {
    if (!m_rateLimited || !m_retryUntil.isValid())
        return 0;
    return int(qMax<qint64>(0, QDateTime::currentDateTimeUtc().secsTo(m_retryUntil)));
}

void SpotifyController::setRateLimited(bool on, const QDateTime &until) {
    if (on == m_rateLimited && until == m_retryUntil)
        return;
    m_rateLimited = on;
    m_retryUntil = on ? until : QDateTime();
    if (on)
        m_loadTick.start();
    emit loadingChanged();
}

// Whether Spotify is rate-limiting spotify_player: its error says so, or
// its log does. While it is, the data lane waits until Spotify said to try
// again, so Omamp doesn't add to the pile.
void SpotifyController::checkRateLimit(bool failed, const QString &error) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    Spotify::LogNews news;
    if (m_settings.backend == "spotify_player") {
        news = Spotify::readLogNews(Spotify::SpotifyPlayerBackend::cacheFolder(), &m_logFile, &m_logOffset, 120, now);
        learnRefusals(news);
    }
    const bool hit = Spotify::isRateLimitText(error) || news.rateLimits > 0 ||
                     (failed && m_rateLimited && error.contains("in time"));
    if (hit) {
        QDateTime until = news.retryUntil.isValid() ? news.retryUntil : m_retryUntil;
        if (failed && (!until.isValid() || until < now.addSecs(2)))
            until = now.addSecs(10);
        if (!until.isValid())
            until = now;
        if (failed)
            m_requests.holdData(int(qMin<qint64>(now.msecsTo(until), 120000)));
        if (!m_rateLimited || until > m_retryUntil)
            setRateLimited(true, until);
    } else if (!failed && m_rateLimited && now >= m_retryUntil && !m_loading) {
        setRateLimited(false);
    }
}

// Remembers what Spotify refused your client ID, so Omamp doesn't ask for
// it again today. A playlist that is loading now and turns out refused
// stops waiting: spotify_player is asking ncspot's shared, rate-limited ID
// instead, which can take a minute. Its answer still fills the cache if it
// comes.
void SpotifyController::learnRefusals(const Spotify::LogNews &news) {
    if (news.refusals.isEmpty())
        return;
    m_refusals.setClientId(Spotify::SpotifyPlayerBackend::effectiveClientId());
    bool changed = false;
    for (const Spotify::Refusal &r : news.refusals)
        changed = m_refusals.learn(Spotify::refusalKey(r.url), r.status) || changed;
    if (changed)
        emit refusedChanged();
    if (m_loading && m_view.section == "tracks" && m_view.container.kind == "playlist" && !m_view.showTracks &&
        !m_view.restricted && isRestricted(m_view.container)) {
        ++m_viewGeneration;
        m_retryTimer.stop();
        m_view.restricted = true;
        setLoading(false);
        emit viewChanged();
    }
}

bool SpotifyController::isRestricted(const Spotify::Item &item) const {
    return item.kind == "playlist" && (item.spotifyMade() || m_refusals.refused("playlist:" + item.id));
}

QString SpotifyController::viewNote() const {
    if (m_view.section != "tracks")
        return {};
    if (m_view.container.kind == "artist")
        return tr("Spotify doesn't let new apps read an artist's top tracks or related artists, so these come from "
                  "search. Play plays the artist on Spotify.");
    if (!m_view.restricted)
        return {};
    const QString why =
        m_view.container.spotifyMade()
            ? tr("Spotify made this playlist, and it lets only its own apps read the tracks of playlists it makes.")
            : tr("Spotify wouldn't let your app read this playlist's tracks today.");
    if (m_view.showTracks)
        return why + ' ' + tr("Listing them goes through a shared connection that Spotify keeps busy, so it can take a "
                              "minute.");
    return why + ' ' + tr("Omamp can still play it: Spotify plays the whole playlist, and the deck follows each track.");
}

// --- Status ---------------------------------------------------------------------------

QString SpotifyController::statusText() const {
    if (!m_settings.enabled)
        return tr("Spotify is off. Switch it on to see Spotify in the library tree.");
    if (!m_status.installed)
        return m_settings.backend == "spotifast"
                   ? tr("Spotifast isn't installed. Install it with: omarchy-pkg-aur-add spotifast-bin")
                   : tr("spotify_player isn't installed. Install it with: omarchy-pkg-add spotify-player");
    if (m_settings.backend == "spotifast") {
        if (!m_status.running)
            return tr("Spotifast isn't open. Open it, sign in there, then check again.");
        return tr("Spotifast is open. Omamp can play Spotify links in Now Playing, pause and seek them, but can't search or "
                  "read your library through Spotifast.");
    }
    if (!m_status.signedIn)
        return tr("Not signed in. Sign in opens spotify_player's own sign-in in a terminal; your browser asks Spotify "
                  "for permission. Omamp never sees your password.");
    if (!m_status.running)
        return m_status.error.isEmpty() ? tr("Signed in, but spotify_player isn't running.") : m_status.error;
    QString text = m_status.account.isEmpty() ? tr("Connected.") : tr("Connected as %1.").arg(m_status.account);
    if (m_status.premium == "yes")
        text += ' ' + tr("Premium: playback works here.");
    else if (m_status.premium == "no")
        text += ' ' + tr("This account can't play through spotify_player (Spotify needs Premium for that). You can "
                         "still browse and search.");
    else
        text += ' ' + tr("No Spotify Connect device called “%1” yet: spotify_player makes one only for a Premium "
                         "account. Browsing and search work either way.")
                          .arg(m_status.deviceName);
    return text;
}

QString SpotifyController::startFailureHint() const {
    if (m_status.premium == "no")
        return tr("Spotify needs a Premium account to play here. You can still browse and search.");
    bool device = false;
    for (const QVariant &d : m_status.devices)
        device = device || d.toMap().value("name").toString() == m_status.deviceName;
    if (m_settings.backend == "spotify_player" && m_status.running && !device)
        return tr("spotify_player has no Spotify Connect device on this computer, so nothing could play. It makes one "
                  "only for a Premium account.");
    if (m_rateLimited)
        return tr("Spotify is rate-limiting requests, so the track didn't start in time. Try it again in a minute.");
    return tr("Spotify didn't start the track. %1's log may say why.").arg(m_backend->name());
}

QString SpotifyController::followMode() const {
    if (!m_settings.enabled)
        return "none";
    if (m_mpris.available())
        return "mpris";
    return m_status.running ? "polling" : "none";
}

void SpotifyController::refreshStatus() {
    if (m_checking)
        return;
    m_checking = true;
    emit statusChanged();
    emit clientIdChanged();
    const int generation = ++m_statusGeneration;
    const bool autoStart = m_settings.enabled && m_settings.startDaemon && m_settings.backend == "spotify_player";
    // First what this computer can say without Spotify: installed, signed
    // in, running (and starting it).
    request(
        Spotify::Requests::Control, "status",
        [autoStart](Spotify::Backend &b, Spotify::Result &r) {
            const bool installed = b.installed(), signedIn = b.signedIn();
            bool running = b.running();
            bool started = false;
            if (autoStart && installed && signedIn && !running) {
                b.start(&r.error);
                running = b.running();
                started = running;
            }
            r.extra = {{"installed", installed}, {"signedIn", signedIn}, {"running", running},
                       {"started", started},     {"deviceName", b.deviceName()}};
        },
        [this, generation](const Spotify::Result &r) {
            if (generation != m_statusGeneration)
                return;
            Status s;
            s.installed = r.extra.value("installed").toBool();
            s.signedIn = r.extra.value("signedIn").toBool();
            s.running = r.extra.value("running").toBool();
            s.deviceName = r.extra.value("deviceName").toString();
            s.error = r.error;
            // What was learned before stays until it is read again.
            if (s.running && m_status.running) {
                s.account = m_status.account;
                s.devices = m_status.devices;
                s.device = m_status.device;
            }
            if (m_status.premium == "no" || (s.running && m_status.running))
                s.premium = m_status.premium;
            m_status = s;
            if (!s.running || !m_backend->caps().library) {
                if (s.running && !m_backend->caps().library) {
                    fetchDevicesAndAccount(generation);
                    return;
                }
                m_checking = false;
                emit statusChanged();
                return;
            }
            // spotify_player reads the whole library from Spotify as it
            // starts; Omamp waits before it asks for more.
            if (r.extra.value("started").toBool())
                m_requests.holdData(m_warmupMs);
            emit statusChanged();
            fetchDevicesAndAccount(generation);
        });
}

// The devices (one request) and the account (from the playlists, which the
// view needs anyway, through the cache).
void SpotifyController::fetchDevicesAndAccount(int generation) {
    request(
        Spotify::Requests::Data, "devices",
        [](Spotify::Backend &b, Spotify::Result &r) { r.devices = b.devices(&r.error); },
        [this, generation](const Spotify::Result &r) {
            if (generation != m_statusGeneration)
                return;
            if (r.ok()) {
                m_status.devices.clear();
                m_status.device.clear();
                bool ours = false;
                for (const Spotify::Device &d : r.devices) {
                    m_status.devices << QVariantMap{{"id", d.id}, {"name", d.name}, {"type", d.type}, {"active", d.active}};
                    if (d.active)
                        m_status.device = d.name;
                    ours = ours || d.name == m_status.deviceName;
                }
                if (ours)
                    m_status.premium = "yes";
                else if (m_status.premium != "no")
                    m_status.premium = "unknown";
            } else if (!r.cancelled) {
                m_status.error = r.error;
            }
            if (!m_backend->caps().library) {
                m_checking = false;
                emit statusChanged();
                return;
            }
            View playlists;
            playlists.section = "playlists";
            fetchList(playlists, false, [this, generation](const Spotify::Result &l) {
                if (generation != m_statusGeneration)
                    return;
                if (l.ok()) {
                    QHash<QString, int> owners;
                    for (const Spotify::Item &p : l.items)
                        if (!p.owner.isEmpty() && p.owner != "Spotify")
                            ++owners[p.owner];
                    int most = 0;
                    for (auto it = owners.cbegin(); it != owners.cend(); ++it)
                        if (it.value() > most || (it.value() == most && it.key() < m_status.account))
                            m_status.account = it.key(), most = it.value();
                } else if (!l.cancelled && m_status.error.isEmpty()) {
                    m_status.error = l.error;
                }
                m_checking = false;
                emit statusChanged();
                emit playlistsChanged();
            });
        });
}

bool SpotifyController::signIn() {
    const QStringList command = m_backend->signInCommand();
    if (command.isEmpty()) {
        // Spotifast signs in in its own window.
        startBackend();
        return true;
    }
    if (!m_launch || !m_launch(command)) {
        setMessage(tr("Couldn't open a terminal. Run this yourself, then check again: %1").arg(command.join(' ')));
        return false;
    }
    m_signInChecks = 0;
    m_signInWait.start();
    setMessage(tr("Finish signing in in the terminal and your browser. Omamp notices when you're done."));
    emit statusChanged();
    return true;
}

void SpotifyController::startBackend() {
    request(
        Spotify::Requests::Control, "start",
        [](Spotify::Backend &b, Spotify::Result &r) { b.start(&r.error); },
        [this](const Spotify::Result &r) {
            if (!r.error.isEmpty())
                setMessage(r.error);
            refreshStatus();
        });
}

void SpotifyController::connectDevice(const QString &id) {
    request(
        Spotify::Requests::Control, "connect:" + id,
        [id](Spotify::Backend &b, Spotify::Result &r) { b.connectDevice(id, &r.error); },
        [this](const Spotify::Result &r) {
            if (!r.error.isEmpty())
                setMessage(r.error);
            refreshStatus();
        });
}

// --- Your own client ID --------------------------------------------------------------

QString SpotifyController::appConfigPath() const { return Spotify::SpotifyPlayerBackend::configFolder() + "/app.toml"; }

QString SpotifyController::clientId() const {
    return Spotify::SpotifyPlayerBackend::effectiveClientId();
}

QString SpotifyController::clientIdKind() const {
    using B = Spotify::SpotifyPlayerBackend;
    if (B::hasClientIdCommand())
        return "command";
    const QString id = B::configuredClientId();
    if (id.isEmpty() || id.compare(B::ncspotClientId(), Qt::CaseInsensitive) == 0)
        return "default";
    if (id.compare(B::spotifyDesktopClientId(), Qt::CaseInsensitive) == 0)
        return "desktop";
    return "own";
}

bool SpotifyController::useOwnClientId(const QString &id) {
    using B = Spotify::SpotifyPlayerBackend;
    auto say = [this](const QString &text) {
        m_clientIdMessage = text;
        emit clientIdChanged();
    };
    if (!B::isValidClientId(id)) {
        say(tr("That isn't a Spotify client ID. A client ID is 32 letters and digits, shown on your app's page at "
               "developer.spotify.com."));
        return false;
    }
    if (B::hasClientIdCommand()) {
        say(tr("app.toml sets client_id_command, which spotify_player uses instead of client_id. Remove that line from "
               "%1 first.")
                .arg(appConfigPath()));
        return false;
    }
    m_clientIdBusy = true;
    say(tr("Saving…"));
    const QString folder = B::configFolder();
    // On the control lane, so no command runs against spotify_player while
    // it stops.
    request(
        Spotify::Requests::Control, "client-id",
        [id, folder](Spotify::Backend &, Spotify::Result &r) {
            QString backup;
            if (!B::writeClientId(folder, id, &backup, &r.error))
                return;
            r.extra.insert("backup", backup);
            // spotify_player reads the ID as it starts. It needs a new
            // sign-in for it, and started without one it would wait for a
            // browser, so it is stopped now and started after the sign-in.
            B player;
            const bool wasRunning = player.running();
            QString stopError;
            if (wasRunning && !player.stopInstances(&stopError))
                r.extra.insert("stopError", stopError);
            r.extra.insert("stopped", wasRunning && stopError.isEmpty());
        },
        [this, say](const Spotify::Result &r) {
            m_clientIdBusy = false;
            if (!r.ok()) {
                say(r.error);
                return;
            }
            m_cache.clear();
            m_refusals.setClientId(Spotify::SpotifyPlayerBackend::effectiveClientId());
            emit refusedChanged();
            QString text = tr("Saved your client ID in %1.").arg(appConfigPath());
            const QString backup = r.extra.value("backup").toString();
            if (!backup.isEmpty())
                text += ' ' + tr("The previous file is kept as %1.").arg(backup);
            if (r.extra.value("stopped").toBool())
                text += ' ' + tr("spotify_player was stopped.");
            else if (r.extra.contains("stopError"))
                text += ' ' + r.extra.value("stopError").toString();
            text += ' ' + tr("Now sign in again with the new ID: press Sign in…, or run “spotify_player authenticate” in "
                             "a terminal. Omamp starts spotify_player once you're signed in.");
            say(text);
            refreshStatus();
        });
    return true;
}

// --- The view -------------------------------------------------------------------------

QString SpotifyController::viewTitle() const {
    if (m_view.section == "tracks")
        return m_view.container.name.isEmpty() ? kindLabel(m_view.container.kind) : m_view.container.name;
    if (m_view.section == "search")
        return tr("Search: %1").arg(m_view.search);
    if (m_view.section == "liked")
        return tr("Liked Songs");
    if (m_view.section == "albums")
        return tr("Saved albums");
    if (m_view.section == "artists")
        return tr("Followed artists");
    return tr("Your playlists");
}

QVariantList SpotifyController::ownPlaylists() const {
    QVariantList out;
    for (const Spotify::Item &p : m_playlists)
        if (m_status.account.isEmpty() || p.owner == m_status.account)
            out << QVariantMap{{"id", p.id}, {"name", p.name}};
    return out;
}

void SpotifyController::setLoading(bool on) {
    if (on == m_loading)
        return;
    m_loading = on;
    m_loadingSlow = false;
    if (on) {
        m_loadClock.start();
        m_loadTick.start();
    } else {
        m_loadClock.invalidate();
    }
    emit loadingChanged();
}

int SpotifyController::ttlFor(const View &view) const {
    // Searches go stale sooner than your own library.
    return view.section == "search" ? qMin(m_cacheTtlMs, 5 * 60 * 1000) : m_cacheTtlMs;
}

// Every list goes through here: the cache when it is fresh, else one
// request on the data lane (shared with any other asking for the same).
void SpotifyController::fetchList(const View &view, bool force, std::function<void(const Spotify::Result &)> done) {
    const QString key = view.key();
    const auto cached = m_cache.constFind(key);
    if (!force && cached != m_cache.constEnd() && cached->age.elapsed() < ttlFor(view)) {
        Spotify::Result r;
        r.items = cached->items;
        r.sections = cached->sections;
        r.search = cached->search;
        done(r);
        return;
    }
    request(
        Spotify::Requests::Data, key,
        [view](Spotify::Backend &b, Spotify::Result &r) {
            if (view.section == "search") {
                r.search = b.search(view.search, &r.error);
                auto add = [&](const QList<Spotify::Item> &list, const QString &heading) {
                    for (const Spotify::Item &i : list) {
                        r.items << i;
                        r.sections << heading;
                    }
                };
                add(r.search.tracks, tr("Tracks"));
                add(r.search.albums, tr("Albums"));
                add(r.search.artists, tr("Artists"));
                add(r.search.playlists, tr("Playlists"));
            } else if (view.section == "tracks" && view.container.kind == "artist") {
                // From search only: never `get item … artist`.
                r.items = b.artistPage(view.container, &r.sections, &r.error);
            } else if (view.section == "tracks") {
                r.items = b.tracksOf(view.container, &r.error);
            } else {
                r.items = b.library(view.section, &r.error);
            }
        },
        [this, view, key, done](const Spotify::Result &r) {
            if (r.ok()) {
                Cached c;
                c.items = r.items;
                c.sections = r.sections;
                c.search = r.search;
                c.age.start();
                m_cache.insert(key, c);
                rememberTracks(r.items);
                rememberNames(r.items);
                if (view.section == "playlists") {
                    m_playlists = r.items;
                    emit playlistsChanged();
                }
                if (view.section == "liked") {
                    m_liked.clear();
                    for (const Spotify::Item &i : r.items)
                        m_liked.insert(i.id);
                    m_likedKnown = true;
                }
            }
            done(r);
        });
}

void SpotifyController::load(const View &view, bool force, bool retry) {
    if (!retry) {
        m_autoRetries = 0;
        m_retryTimer.stop();
    }
    // Leaving a list that is still loading stops it, unless something else
    // waits for the same answer.
    if (m_loading && view.key() != m_view.key())
        m_requests.cancel(m_view.key());
    m_view = view;
    m_view.error.clear();
    m_viewStale = false;
    const int generation = ++m_viewGeneration;
    emit viewChanged();
    if (!m_settings.enabled) {
        m_items.setItems({});
        setLoading(false);
        return;
    }
    if (!m_backend->caps().library) {
        m_items.setItems({});
        setLoading(false);
        m_view.error = tr("%1 can't show your library or search. Choose spotify_player in Settings → Spotify for that; "
                          "links you paste still play.")
                           .arg(m_backend->name());
        emit viewChanged();
        return;
    }
    // A playlist Spotify keeps from your app: shown with Play and Shuffle,
    // its tracks only when asked for (or when a copy is already here).
    m_view.restricted = m_view.section == "tracks" && isRestricted(m_view.container);
    if (m_view.restricted && !m_view.showTracks) {
        const auto kept = m_cache.constFind(m_view.key());
        m_items.setItems(kept != m_cache.constEnd() ? kept->items : QList<Spotify::Item>());
        setLoading(false);
        emit viewChanged();
        return;
    }
    // A copy from before shows at once, while a fresh one is read.
    const auto cached = m_cache.constFind(m_view.key());
    const bool fresh = !force && cached != m_cache.constEnd() && cached->age.elapsed() < ttlFor(m_view);
    if (cached != m_cache.constEnd())
        m_items.setItems(cached->items, cached->sections);
    else if (!retry)
        m_items.setItems({});
    if (fresh) {
        setLoading(false);
        if (m_items.count() == 0 && m_view.section == "search")
            m_view.error = tr("Nothing on Spotify matches “%1”.").arg(m_view.search);
        emit viewChanged();
        return;
    }
    setLoading(true);
    const View v = m_view;
    fetchList(v, true, [this, v, generation](const Spotify::Result &r) {
        if (generation != m_viewGeneration)
            return;
        if (r.cancelled) {
            setLoading(false);
            m_view.error = tr("Stopped loading. Press Retry to try again.");
            emit viewChanged();
            return;
        }
        if (!r.ok()) {
            // Rate-limited: try again once Spotify allows, twice at most,
            // still showing "loading".
            if (m_rateLimited && m_autoRetries < 2) {
                ++m_autoRetries;
                m_retryTimer.start(qMax(retryIn(), 3) * 1000);
                emit loadingChanged();
                return;
            }
            setLoading(false);
            m_view.error = m_rateLimited ? tr("Spotify is rate-limiting requests, so this didn't load. Press Retry in "
                                              "a minute, or use your own client ID (Settings → Spotify).")
                                         : r.error;
            emit viewChanged();
            return;
        }
        m_autoRetries = 0;
        setLoading(false);
        m_items.setItems(r.items, r.sections);
        if (r.items.isEmpty() && v.section == "search")
            m_view.error = tr("Nothing on Spotify matches “%1”.").arg(v.search);
        if (m_rateLimited && retryIn() == 0)
            setRateLimited(false);
        emit viewChanged();
    });
}

void SpotifyController::openSection(const QString &section) {
    m_back.clear();
    View v;
    v.section = section;
    load(v);
}

void SpotifyController::openRow(int row) {
    const Spotify::Item item = m_items.at(row);
    if (item.kind == "track" || !item.isValid())
        return;
    openContainer(item, true);
}

void SpotifyController::openContainer(const Spotify::Item &given, bool keepBack) {
    if (keepBack)
        m_back << m_view;
    else
        m_back.clear();
    View v;
    v.section = "tracks";
    v.container = withName(given);
    rememberNames({v.container});
    if (v.container.kind == "artist" && v.container.name.isEmpty()) {
        // An artist link: search needs the name, and Spotify's public
        // oEmbed page gives it without the Web API.
        m_view = v;
        m_view.error.clear();
        setLoading(true);
        emit viewChanged();
        const int generation = ++m_viewGeneration;
        m_art.lookUpTitle(v.container.webUrl(), [this, v, generation](const QString &title) {
            if (generation != m_viewGeneration)
                return;
            View named = v;
            named.container.name = title;
            if (!title.isEmpty())
                rememberNames({named.container});
            load(named);
        });
        return;
    }
    load(v);
}

Spotify::Item SpotifyController::knownContext(const QString &kind, const QString &id) const {
    Spotify::Item item = m_contexts.value(kind + ':' + id);
    if (!item.isValid()) {
        item.kind = kind;
        item.id = id;
    }
    return item;
}

void SpotifyController::rememberNames(const QList<Spotify::Item> &items) {
    for (const Spotify::Item &i : items) {
        if (i.kind != "track" && i.isValid() && !i.name.isEmpty())
            m_contexts.insert(i.kind + ':' + i.id, i);
        // A track or album names its artists too.
        for (int a = 0; a < i.artistIds.size() && a < i.artists.size(); ++a) {
            const QString key = "artist:" + i.artistIds.at(a);
            if (!i.artistIds.at(a).isEmpty() && !m_contexts.contains(key)) {
                Spotify::Item artist;
                artist.kind = "artist";
                artist.id = i.artistIds.at(a);
                artist.name = i.artists.at(a);
                m_contexts.insert(key, artist);
            }
        }
    }
}

Spotify::Item SpotifyController::withName(Spotify::Item item) const {
    if (item.name.isEmpty()) {
        const Spotify::Item known = m_contexts.value(item.kind + ':' + item.id);
        if (!known.name.isEmpty())
            return known;
    }
    return item;
}

void SpotifyController::back() {
    if (m_back.isEmpty())
        return;
    load(m_back.takeLast());
}

void SpotifyController::search(const QString &text) {
    if (text.trimmed().isEmpty())
        return;
    if (m_view.section != "search")
        m_back << m_view;
    View v;
    v.section = "search";
    v.search = text.trimmed();
    load(v);
}

void SpotifyController::reload() {
    load(m_view, true);
    refreshStatus();
}

void SpotifyController::cancelLoad() {
    m_retryTimer.stop();
    if (!m_loading)
        return;
    m_requests.cancel(m_view.key());
    // A list that was waiting for an automatic retry has nothing running.
    if (m_loading && !m_requests.has(m_view.key())) {
        ++m_viewGeneration;
        setLoading(false);
        m_view.error = tr("Stopped loading. Press Retry to try again.");
        emit viewChanged();
    }
}

void SpotifyController::setViewVisible(bool visible) {
    m_viewVisible = visible;
    if (!visible || !m_settings.enabled)
        return;
    if (m_viewStale)
        load(m_view);
    // What plays now: MPRIS says it for free. Without it, one read when
    // the view is shown (at most every 15 s), never a timer: while Omamp
    // plays a Spotify track its own following keeps this fresh.
    if (m_mpris.available()) {
        noteMpris();
    } else if (m_status.running && m_playback.state() != AudioEngine::Playing &&
               (!m_nowRead.isValid() || m_nowRead.elapsed() > 15000)) {
        m_nowRead.start();
        request(
            Spotify::Requests::Control, "playback",
            [](Spotify::Backend &b, Spotify::Result &r) { r.playback = b.playback(&r.error); },
            [this](const Spotify::Result &r) {
                if (r.ok())
                    noteNowPlaying(r.playback);
            });
    }
}

void SpotifyController::noteMpris() {
    if (m_settings.enabled && m_mpris.available())
        noteNowPlaying(m_mpris.state());
}

void SpotifyController::noteNowPlaying(const Spotify::Playback &given) {
    Spotify::Playback p = given;
    // MPRIS from spotify_player names no id: find the track among those
    // Omamp has seen, so its cover and Liked state are known.
    if (p.valid && p.track.id.isEmpty() && !p.track.name.isEmpty()) {
        Spotify::Item match;
        if (m_playback.isOurs(p))
            match = knownTrack(m_playback.trackId());
        if (!match.isValid())
            for (const Spotify::Item &k : std::as_const(m_known))
                if (k.name.compare(p.track.name, Qt::CaseInsensitive) == 0 &&
                    (k.album.isEmpty() || p.track.album.isEmpty() || k.album == p.track.album) &&
                    (k.durationMs <= 0 || p.track.durationMs <= 0 || qAbs(k.durationMs - p.track.durationMs) <= 3000)) {
                    match = k;
                    break;
                }
        if (match.isValid()) {
            const QString image = p.track.imageUrl;
            p.track = match;
            if (!image.isEmpty())
                p.track.imageUrl = image;
        }
    }
    QVariantMap m{{"active", p.valid}};
    if (p.valid) {
        if (p.track.isValid())
            rememberTracks({p.track});
        m_art.want(p.track);
        const QString key = p.track.isValid() ? SpotifyArt::keyFor(p.track) : QString();
        m.insert("playing", p.playing);
        m.insert("title", p.track.name);
        m.insert("artists", p.track.artistText());
        m.insert("album", p.track.album);
        m.insert("progressMs", p.progressMs);
        m.insert("durationMs", p.track.durationMs);
        m.insert("device", p.deviceName);
        m.insert("uri", p.track.isValid() ? p.track.uri() : QString());
        m.insert("artKey", key);
        m.insert("art", p.track.isValid() ? fileUrl(m_art.cachedFile(p.track)) : QString());
        m.insert("liked", m_liked.contains(p.track.id));
        m.insert("likedKnown", m_likedKnown);
    }
    if (m != m_nowPlaying) {
        m_nowPlaying = m;
        emit nowPlayingChanged();
    }
}

void SpotifyController::noteStarted(const Spotify::Playback &p) {
    m_status.premium = "yes";
    if (!p.deviceName.isEmpty())
        m_status.device = p.deviceName;
    emit statusChanged();
    // NowPlaying may have looked before the album was known.
    emit playingArtChanged();
}

// --- Now Playing ------------------------------------------------------------------------

void SpotifyController::queue(const QList<Spotify::Item> &tracks, int start, Mode mode) {
    if (!m_player || tracks.isEmpty())
        return;
    rememberTracks(tracks);
    QList<Playlist::Track> list;
    for (const Spotify::Item &t : tracks) {
        Playlist::Track pt;
        pt.path = t.queuePath();
        pt.title = t.name.isEmpty() ? t.uri() : t.name;
        pt.artist = t.artistText();
        pt.durationMs = t.durationMs;
        list << pt;
    }
    int n = 0;
    if (mode == Play)
        n = m_player->playTracks(list, qMax(0, start));
    else if (mode == Next)
        n = m_player->queueNext(list);
    else
        n = m_player->appendTracks(list);
    if (mode != Play)
        setMessage(tr("Added %n Spotify track(s) to Now Playing", "", n));
}

void SpotifyController::queueContext(const Spotify::Item &given, Mode mode, bool shuffle) {
    if (!m_player || !given.isValid())
        return;
    const Spotify::Item c = withName(given);
    rememberNames({c});
    m_art.want(c);
    Playlist::Track pt;
    pt.path = Spotify::contextQueuePath(c, shuffle);
    pt.title = c.name.isEmpty() ? kindLabel(c.kind) : c.name;
    if (c.kind == "playlist")
        pt.artist = c.owner.isEmpty() ? tr("Spotify playlist") : tr("Spotify playlist by %1").arg(c.owner);
    else if (c.kind == "album")
        pt.artist = c.artistText();
    else
        pt.artist = tr("Artist on Spotify");
    if (mode == Play)
        m_player->playTracks({pt}, 0);
    else if (mode == Next)
        m_player->queueNext({pt});
    else
        m_player->appendTracks({pt});
    if (mode != Play)
        setMessage(tr("Added “%1” to Now Playing").arg(pt.title));
}

void SpotifyController::playView(bool shuffle) {
    if (m_settings.enabled && m_view.section == "tracks")
        queueContext(m_view.container, Play, shuffle);
}

void SpotifyController::enqueueView(bool next) {
    if (m_settings.enabled && m_view.section == "tracks")
        queueContext(m_view.container, next ? Next : Append, false);
}

void SpotifyController::showRestrictedTracks() {
    if (m_view.section != "tracks" || !m_view.restricted)
        return;
    View v = m_view;
    v.showTracks = true;
    load(v);
}

void SpotifyController::playFrom(SpotifyItemsModel *model, int row, Mode mode) {
    if (!m_settings.enabled)
        return;
    const Spotify::Item item = model->at(row);
    if (!item.isValid())
        return;
    if (item.kind == "track") {
        if (mode == Play)
            queue(model->tracks(), model->trackIndex(row), Play);
        else
            queue({item}, 0, mode);
        return;
    }
    // An artist, or a playlist Spotify keeps from your app, plays as one
    // entry: Spotify plays it, and nothing reads its tracks.
    if (item.kind == "artist" || isRestricted(item)) {
        queueContext(item, mode, false);
        return;
    }
    View v;
    v.section = "tracks";
    v.container = item;
    fetchList(v, false, [this, mode](const Spotify::Result &r) {
        if (r.cancelled)
            return;
        if (!r.ok())
            setMessage(m_rateLimited ? tr("Spotify is rate-limiting requests. Try again in a minute.") : r.error);
        else
            queue(r.items, 0, mode);
    });
}

void SpotifyController::openBlendRow(int row) {
    const Spotify::Item item = m_blend.at(row);
    if (!item.isValid())
        return;
    if (item.kind == "track") {
        queue({item}, 0, Next);
        return;
    }
    openContainer(item, false);
    emit showRequested();
}

QString SpotifyController::activateUri(const QString &text) {
    QString kind, id;
    if (!Spotify::parseReference(text, &kind, &id) || !m_settings.enabled)
        return {};
    if (kind != "track") {
        Spotify::Item container;
        container.kind = kind;
        container.id = id;
        for (const Spotify::Item &p : m_playlists)
            if (p.id == id)
                container = p;
        if (!m_backend->caps().library) {
            setMessage(tr("%1 can only play single tracks from Omamp.").arg(m_backend->name()));
            return {};
        }
        openContainer(container, false);
        emit showRequested();
        return "opened";
    }
    auto playIt = [this](const Spotify::Item &t) {
        if (!m_player)
            return;
        rememberTracks({t});
        const int current = m_player->currentIndex();
        if (current < 0) {
            queue({t}, 0, Play);
            return;
        }
        queue({t}, 0, Next);
        m_player->playIndex(current + 1);
    };
    const Spotify::Item known = knownTrack(id);
    if (known.isValid() && !known.name.isEmpty()) {
        playIt(known);
        return "played";
    }
    request(
        Spotify::Requests::Data, "track:" + id,
        [id](Spotify::Backend &b, Spotify::Result &r) {
            Spotify::Item t = b.track(id, &r.error);
            if (!t.isValid()) {
                t = Spotify::Item();
                t.kind = "track";
                t.id = id;
            }
            r.items = {t};
        },
        [playIt](const Spotify::Result &r) {
            if (!r.cancelled && !r.items.isEmpty())
                playIt(r.items.first());
        });
    return "played";
}

void SpotifyController::addToPlaylist(const Spotify::Item &item, const QString &playlistId) {
    if (!item.isValid() || playlistId.isEmpty())
        return;
    QString name = playlistId;
    for (const Spotify::Item &p : m_playlists)
        if (p.id == playlistId)
            name = p.name;
    request(
        Spotify::Requests::Data, {},
        [item, playlistId](Spotify::Backend &b, Spotify::Result &r) { b.addToPlaylist(playlistId, item, &r.error); },
        [this, item, name, playlistId](const Spotify::Result &r) {
            if (r.cancelled)
                return;
            if (r.ok())
                m_cache.remove("tracks:playlist:" + playlistId);
            setMessage(r.ok() ? tr("Added “%1” to %2").arg(item.name, name) : r.error);
        });
}

void SpotifyController::addRowToPlaylist(int row, const QString &playlistId) { addToPlaylist(m_items.at(row), playlistId); }
void SpotifyController::addBlendRowToPlaylist(int row, const QString &playlistId) {
    addToPlaylist(m_blend.at(row), playlistId);
}

void SpotifyController::setPlayingLiked(bool liked) {
    const QString id = m_nowPlaying.value("uri").toString().section(':', 2);
    request(
        Spotify::Requests::Control, {},
        [liked](Spotify::Backend &b, Spotify::Result &r) { b.like(liked, &r.error); },
        [this, liked, id](const Spotify::Result &r) {
            if (!r.ok()) {
                setMessage(r.error);
                return;
            }
            if (!id.isEmpty()) {
                if (liked)
                    m_liked.insert(id);
                else
                    m_liked.remove(id);
            }
            m_cache.remove("library:liked");
            m_nowPlaying.insert("liked", liked);
            emit nowPlayingChanged();
            setMessage(liked ? tr("Saved to Liked Songs") : tr("Removed from Liked Songs"));
        });
}

// --- Blending -----------------------------------------------------------------------------

void SpotifyController::blendQuery(const QString &text) {
    m_blendText = text.trimmed();
    ++m_blendGeneration;
    if (!m_settings.enabled || !m_settings.blendSearch || !m_backend->caps().search || m_blendText.size() < 2) {
        m_blendTimer.stop();
        m_blend.setItems({});
        m_blendLoading = false;
        m_blendError.clear();
        emit blendChanged();
        return;
    }
    m_blendTimer.start();
    emit blendChanged();
}

void SpotifyController::runBlend() {
    const int generation = ++m_blendGeneration;
    m_blendLoading = true;
    emit blendChanged();
    View v;
    v.section = "search";
    v.search = m_blendText;
    // The same search as the palette's or the view's shares one request.
    fetchList(v, false, [this, generation](const Spotify::Result &r) {
        if (generation != m_blendGeneration)
            return;
        QList<Spotify::Item> items;
        QStringList sections;
        auto add = [&](const QList<Spotify::Item> &list, int max, const QString &heading) {
            for (int i = 0; i < list.size() && i < max; ++i) {
                items << list.at(i);
                sections << heading;
            }
        };
        add(r.search.tracks, 8, tr("Tracks"));
        add(r.search.albums, 6, tr("Albums"));
        add(r.search.artists, 4, tr("Artists"));
        add(r.search.playlists, 4, tr("Playlists"));
        rememberTracks(items);
        m_blend.setItems(items, sections);
        m_blendLoading = false;
        m_blendError = r.cancelled ? QString() : r.error;
        emit blendChanged();
    });
}

void SpotifyController::paletteTextChanged() {
    if (!m_palette)
        return;
    const QString text = m_palette->text().trimmed();
    ++m_paletteGeneration;
    if (!m_settings.enabled || !m_settings.blendPalette || !m_backend->caps().search || text.size() < 2 ||
        text.startsWith('>') || text.contains(':')) {
        m_paletteTimer.stop();
        return;
    }
    m_paletteText = text;
    m_paletteTimer.start();
}

void SpotifyController::runPaletteSearch() {
    const QString text = m_paletteText;
    const int generation = ++m_paletteGeneration;
    View v;
    v.section = "search";
    v.search = text;
    fetchList(v, false, [this, text, generation](const Spotify::Result &r) {
        if (!m_palette || generation != m_paletteGeneration)
            return;
        QList<Palette::Item> rows;
        auto add = [&](const QList<Spotify::Item> &list, int max) {
            for (int i = 0; i < list.size() && i < max; ++i) {
                const Spotify::Item &s = list.at(i);
                Palette::Item p;
                p.title = s.name;
                p.subtitle = detailOf(s);
                p.detail = s.durationMs > 0 ? minutes(s.durationMs) : s.year;
                p.actionId = s.uri();
                p.kindLabel = kindLabel(s.kind);
                rows << p;
            }
        };
        add(r.search.tracks, 4);
        add(r.search.albums, 2);
        add(r.search.artists, 2);
        add(r.search.playlists, 2);
        rememberTracks(r.search.tracks);
        m_palette->setExternalResults("Spotify", text, rows);
    });
}
