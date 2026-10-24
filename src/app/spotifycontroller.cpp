#include "spotifycontroller.h"

#include <QAudio>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <algorithm>

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
                ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thrum/spotify"
                : cacheDir) {}

QString SpotifyArt::oembedBase() {
    const QString env = qEnvironmentVariable("THRUM_SPOTIFY_OEMBED");
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
    request.setHeader(QNetworkRequest::UserAgentHeader, "Thrum/" + QCoreApplication::applicationVersion());
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
    request.setHeader(QNetworkRequest::UserAgentHeader, "Thrum/" + QCoreApplication::applicationVersion());
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
    request.setHeader(QNetworkRequest::UserAgentHeader, "Thrum/" + QCoreApplication::applicationVersion());
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
            {OwnerRole, "owner"},     {SectionRole, "section"}, {ArtRole, "art"},        {DetailRole, "detail"},
            {SelectedRole, "rowSelected"}, {CurrentRole, "rowCurrent"}};
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
    case SelectedRole: return m_selected.contains(index.row());
    case CurrentRole: return index.row() == m_current;
    }
    return {};
}

void SpotifyItemsModel::setItems(const QList<Spotify::Item> &items, const QStringList &sections) {
    // A fresh copy of the same list (a refresh) keeps what was chosen.
    bool same = items.size() == m_items.size();
    for (int i = 0; same && i < items.size(); ++i)
        same = items.at(i).kind == m_items.at(i).kind && items.at(i).id == m_items.at(i).id;
    beginResetModel();
    const bool changed = items.size() != m_items.size();
    m_items = items;
    m_sections = sections;
    if (!same) {
        m_selected.clear();
        m_current = m_anchor = items.isEmpty() ? -1 : 0;
    }
    endResetModel();
    if (changed)
        emit countChanged();
    if (!same) {
        emit currentRowChanged();
        emit selectionChanged();
    }
}

void SpotifyItemsModel::selectionDataChanged() {
    if (!m_items.isEmpty())
        emit dataChanged(index(0), index(m_items.size() - 1), {SelectedRole, CurrentRole});
    emit selectionChanged();
}

void SpotifyItemsModel::setCurrentRow(int row) {
    if (row == m_current || row < -1 || row >= m_items.size())
        return;
    const int old = m_current;
    m_current = row;
    if (old >= 0 && old < m_items.size())
        emit dataChanged(index(old), index(old), {CurrentRole});
    if (row >= 0)
        emit dataChanged(index(row), index(row), {CurrentRole});
    emit currentRowChanged();
}

// The same rules as TrackListModel::select, so the Spotify lists and the
// library's track list behave alike.
void SpotifyItemsModel::select(int row, int modifiers) {
    if (row < 0 || row >= m_items.size())
        return;
    const bool shift = modifiers & Qt::ShiftModifier;
    const bool ctrl = modifiers & Qt::ControlModifier;
    if (shift && m_anchor >= 0 && m_anchor < m_items.size()) {
        if (!ctrl)
            m_selected.clear();
        for (int i = qMin(m_anchor, row); i <= qMax(m_anchor, row); ++i)
            m_selected.insert(i);
    } else if (ctrl) {
        if (!m_selected.remove(row))
            m_selected.insert(row);
        m_anchor = row;
    } else {
        m_selected.clear();
        m_selected.insert(row);
        m_anchor = row;
    }
    setCurrentRow(row);
    selectionDataChanged();
}

void SpotifyItemsModel::moveCurrent(int delta, int modifiers) {
    if (m_items.isEmpty())
        return;
    const int row = qBound(0, (m_current < 0 ? (delta > 0 ? -1 : int(m_items.size())) : m_current) + delta,
                           int(m_items.size()) - 1);
    // Ctrl moves the cursor without changing the selection, as in the library.
    if ((modifiers & Qt::ControlModifier) && !(modifiers & Qt::ShiftModifier))
        setCurrentRow(row);
    else
        select(row, modifiers & Qt::ShiftModifier);
}

void SpotifyItemsModel::selectAll() {
    m_selected.clear();
    for (int i = 0; i < m_items.size(); ++i)
        m_selected.insert(i);
    if (m_current < 0 && !m_items.isEmpty())
        setCurrentRow(0);
    selectionDataChanged();
}

void SpotifyItemsModel::clearSelection() {
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    selectionDataChanged();
}

QList<int> SpotifyItemsModel::chosenRows() const {
    QList<int> rows(m_selected.cbegin(), m_selected.cend());
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty() && m_current >= 0 && m_current < m_items.size())
        rows << m_current;
    return rows;
}

QList<Spotify::Item> SpotifyItemsModel::chosenItems() const {
    QList<Spotify::Item> out;
    for (int r : chosenRows())
        out << m_items.at(r);
    return out;
}

QList<Spotify::Item> SpotifyItemsModel::chosenTracks() const {
    QList<Spotify::Item> out;
    for (const Spotify::Item &i : chosenItems())
        if (i.kind == "track")
            out << i;
    return out;
}

int SpotifyItemsModel::chosenTrackCount() const { return chosenTracks().size(); }

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
    m_expand = NotExpanded;
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
        // spotify_player's MPRIS has no id; a track whose name Thrum
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
    // Thrum can play the tracks itself: they take the entry's place in Now
    // Playing, and spotify_player isn't asked. Only if that fails does
    // Spotify play it as one, below.
    if (m_context && m_expand != ExpandFailed && !m_started && m_c->expandsContexts()) {
        // Asked again while reading (Next loads, then plays): still reading.
        if (m_expand == Expanding) {
            setState(AudioEngine::Playing);
            return;
        }
        m_expand = Expanding;
        m_starting = true;
        emit bufferingChanged();
        setState(AudioEngine::Playing);
        QPointer<SpotifyPlayback> self(this);
        m_c->expandEntry(m_path, [self, generation] {
            if (!self || generation != self->m_generation)
                return;
            self->m_expand = ExpandFailed;
            self->m_starting = false;
            self->play();
        });
        return;
    }
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

void SpotifyPlayback::pauseForQuit(int timeoutMs) {
    if (m_path.isEmpty() || !m_started || m_state != AudioEngine::Playing)
        return;
    if (followsMpris()) {
        if (m_c->mpris()->status() == QLatin1String("Playing"))
            m_c->mpris()->pauseNow(timeoutMs);
        return;
    }
    // No MPRIS: the backend's command line, on this thread.
    if (const auto backend = m_c->backend())
        backend->pause(nullptr);
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
            // no ids, so what Thrum knew stays.
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
                m_c->setMessage(tr("Spotify is playing something else now, so Thrum paused its queue."));
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
    m_requests.setGapMs(envMs("THRUM_SPOTIFY_GAP_MS", 300));
    m_warmupMs = envMs("THRUM_SPOTIFY_WARMUP_MS", 6000);
    m_slowMs = envMs("THRUM_SPOTIFY_SLOW_MS", 4000);
    m_eventRelay = std::make_shared<QObject>();
    m_eventTarget = std::make_shared<std::function<void(const QJsonObject &)>>(
        [this](const QJsonObject &event) { onConnectEvent(event); });
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

    connect(this, &SpotifyController::settingsChanged, this, &SpotifyController::availabilityChanged);
    connect(this, &SpotifyController::statusChanged, this, &SpotifyController::availabilityChanged);
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
    // Events still on their way find nobody.
    *m_eventTarget = nullptr;
    if (m_player)
        m_player->setSkipHandler({});
    // A session of the controller's own ends with it (the installed one is
    // main()'s, and stays a Connect device as long as Thrum runs).
    if (m_audio && m_audio != SpotifyAudio::installed()) {
        m_audio->setEventHandler({});
        m_audio->shutdown();
    }
    *m_alive = false;
    m_requests.cancelAll();
}

void SpotifyController::waitForIdle() {
    m_requests.waitForIdle();
    QCoreApplication::processEvents();
}

void SpotifyController::makeBackend() {
    m_backend = Spotify::Backend::create(m_settings.backend);
    // Thrum's one Spotify session: the one the engines play through, or,
    // without one (tests, a window with no sidecar installed at start), a
    // session of the controller's own.
    const QString reader = Spotify::SessionReader::findProgram();
    if (!m_audio && !reader.isEmpty()) {
        m_audio = SpotifyAudio::installed();
        if (!m_audio)
            m_audio = std::make_shared<SpotifyAudio>(reader, Spotify::SessionReader::credentialsPath());
        auto target = m_eventTarget;
        QObject *relay = m_eventRelay.get();
        auto keep = m_eventRelay;
        m_audio->setEventHandler([target, relay, keep](const QJsonObject &event) {
            QMetaObject::invokeMethod(
                relay,
                [target, keep, event] {
                    if (*target)
                        (*target)(event);
                },
                Qt::QueuedConnection);
        });
        applyConnectSettings();
    }
    // Spotify's own playlists and artists' top tracks the way Spotify's own
    // app reads them, when the reader is installed and switched on.
    if (m_settings.backend == "spotify_player" && m_settings.sessionReads && m_audio) {
        if (!m_session)
            m_session = std::make_shared<Spotify::SessionReader>(m_audio);
        m_backend->setCatalogue(std::make_unique<Spotify::SessionCatalogue>(m_backend.get(), m_session));
    } else {
        m_session.reset();
    }
    // Made here, on this thread, before any worker can ask for it.
    m_backend->catalogue();
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
    applyConnectSettings();
    noteThrumPlaying();
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

void SpotifyController::setSessionReads(bool on) {
    if (on == m_settings.sessionReads)
        return;
    m_settings.sessionReads = on;
    makeBackend();
    // Lists read one way may differ from the other's.
    m_cache.clear();
    saveSettings();
    if (m_settings.enabled && m_viewVisible)
        load(m_view);
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
// again, so Thrum doesn't add to the pile.
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

// Remembers what Spotify refused your client ID, so Thrum doesn't ask for
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
    if (item.kind != "playlist")
        return false;
    Spotify::Catalogue *catalogue = m_backend->catalogue();
    // What the Web API refused counts only while the Web API is the source.
    return !catalogue->listsQuickly(item) || (catalogue->id() == "web-api" && m_refusals.refused("playlist:" + item.id));
}

QString SpotifyController::viewNote() const {
    if (m_view.section != "tracks")
        return {};
    if (m_view.container.kind == "artist" && m_backend->catalogue()->id() != "web-api")
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
    return why + ' ' + tr("Thrum can still play it: Spotify plays the whole playlist, and the deck follows each track.");
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
        return tr("Spotifast is open. Thrum can play Spotify links in Now Playing, pause and seek them, but can't search or "
                  "read your library through Spotifast.");
    }
    if (!m_status.signedIn)
        return tr("Not signed in. Sign in opens spotify_player's own sign-in in a terminal; your browser asks Spotify "
                  "for permission. Thrum never sees your password.");
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
            s.known = true;
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
            // starts; Thrum waits before it asks for more.
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
    setMessage(tr("Finish signing in in the terminal and your browser. Thrum notices when you're done."));
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
                             "a terminal. Thrum starts spotify_player once you're signed in.");
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
                // Through the catalogue: from search on the Web API, never
                // `get item … artist`.
                r.items = b.catalogue()->artistPage(view.container, &r.sections, &r.error);
            } else if (view.section == "tracks" && view.container.kind == "playlist") {
                r.items = b.catalogue()->playlistTracks(view.container, &r.error);
            } else if (view.section == "tracks" && view.container.kind == "album") {
                r.items = b.catalogue()->albumTracks(view.container, &r.error);
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
            // Spotify's own playlist that couldn't be read: it still plays
            // as a whole.
            if (v.section == "tracks" && v.container.spotifyMade()) {
                m_view.restricted = true;
                m_view.showTracks = false;
            }
            m_view.error = m_rateLimited ? tr("Spotify is rate-limiting requests, so this didn't load. Press Retry in "
                                              "a minute, or use your own client ID (Settings → Spotify).")
                                         : r.error;
            emit viewChanged();
            return;
        }
        m_autoRetries = 0;
        setLoading(false);
        m_items.setItems(r.items, r.sections);
        // An artist opened by a link learns its name from its own tracks.
        if (v.section == "tracks" && v.container.kind == "artist" && m_view.container.name.isEmpty())
            for (const Spotify::Item &i : r.items) {
                const int at = i.artistIds.indexOf(v.container.id);
                if (at >= 0 && at < i.artists.size()) {
                    m_view.container.name = i.artists.at(at);
                    rememberNames({m_view.container});
                    break;
                }
            }
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
    // A playlist shown whole because reading it failed: Retry reads it.
    if (m_view.restricted && !m_view.showTracks && !m_view.error.isEmpty()) {
        View v = m_view;
        v.showTracks = true;
        load(v, true);
        refreshStatus();
        return;
    }
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
    // the view is shown (at most every 15 s), never a timer: while Thrum
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
    // While Thrum plays a Spotify track itself, that is what plays on
    // Spotify as far as Thrum's views go.
    if (thrumTrack().isValid()) {
        noteThrumPlaying();
        return;
    }
    Spotify::Playback p = given;
    // MPRIS from spotify_player names no id: find the track among those
    // Thrum has seen, so its cover and Liked state are known.
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
    const QList<Playlist::Track> list = queueTracks(tracks);
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

void SpotifyController::queueContext(const Spotify::Item &given, Mode mode, bool shuffle, bool expand) {
    if (!m_player || !given.isValid())
        return;
    // Thrum plays Spotify itself: the tracks go in, so Thrum's queue,
    // shuffle and repeat hold for them. Reading them fails: one entry, as
    // before, that spotify_player plays.
    if (expand && expandsContexts()) {
        Spotify::Item c = withName(given);
        readContextTracks(c, [this, c, mode, shuffle](const Spotify::Result &r) {
            if (r.cancelled)
                return;
            if (!r.ok() || r.items.isEmpty()) {
                queueContext(c, mode, shuffle, false);
                return;
            }
            QList<Spotify::Item> tracks = r.items;
            if (shuffle)
                std::shuffle(tracks.begin(), tracks.end(), *QRandomGenerator::global());
            queue(tracks, 0, mode);
        });
        return;
    }
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
            setMessage(tr("%1 can only play single tracks from Thrum.").arg(m_backend->name()));
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

QString SpotifyController::unavailableReason() const {
    if (!m_settings.enabled)
        return tr("Spotify is off — turn it on in Settings → Spotify");
    // Until the status is read, Spotify counts as there.
    if (!m_status.known)
        return {};
    if (!m_status.installed)
        return tr("%1 isn't installed — see Settings → Spotify").arg(m_backend->name());
    if (!m_status.signedIn)
        return m_backend->id() == "spotifast" ? tr("Spotifast isn't open — open it to play Spotify tracks")
                                              : tr("Spotify isn't signed in — sign in in Settings → Spotify");
    return {};
}

QVariantMap SpotifyController::entryFor(const Spotify::Item &t) {
    return {{"path", t.queuePath()},
            {"title", t.name},
            {"artist", t.artistText()},
            {"album", t.album},
            {"art", !t.albumId.isEmpty() ? t.albumId : t.imageUrl},
            {"durationMs", t.durationMs}};
}

void SpotifyController::playChosen(SpotifyItemsModel *model, const QString &how) {
    if (!model || !m_settings.enabled)
        return;
    const Mode mode = how == "next" ? Next : how == "append" ? Append : Play;
    const QList<int> rows = model->chosenRows();
    if (rows.size() == 1) {
        playFrom(model, rows.first(), mode);
        return;
    }
    const QList<Spotify::Item> tracks = model->chosenTracks();
    if (tracks.isEmpty()) {
        setMessage(tr("Choose tracks to play several at once"));
        return;
    }
    queue(tracks, 0, mode);
}

void SpotifyController::addChosenToSpotifyPlaylist(SpotifyItemsModel *model, const QString &playlistId) {
    if (!model || playlistId.isEmpty())
        return;
    const QList<int> rows = model->chosenRows();
    const QList<Spotify::Item> items = rows.size() == 1 ? model->chosenItems() : model->chosenTracks();
    if (items.size() == 1) {
        addToPlaylist(items.first(), playlistId);
        return;
    }
    QString name = playlistId;
    for (const Spotify::Item &p : m_playlists)
        if (p.id == playlistId)
            name = p.name;
    // One command per track, in order, on the data lane; one message at the end.
    request(
        Spotify::Requests::Data, {},
        [items, playlistId](Spotify::Backend &b, Spotify::Result &r) {
            int added = 0;
            for (const Spotify::Item &i : items) {
                QString error;
                if (b.addToPlaylist(playlistId, i, &error))
                    ++added;
                else if (r.error.isEmpty())
                    r.error = error;
            }
            r.extra = {{"added", added}};
        },
        [this, name, playlistId, items](const Spotify::Result &r) {
            if (r.cancelled)
                return;
            const int added = r.extra.value("added").toInt();
            if (added > 0)
                m_cache.remove("tracks:playlist:" + playlistId);
            setMessage(added == items.size() ? tr("Added %n tracks to %1", "", added).arg(name)
                                             : tr("Added %1 of %2 tracks to %3: %4")
                                                   .arg(added)
                                                   .arg(items.size())
                                                   .arg(name, r.error));
        });
}

void SpotifyController::addChosenToNewSpotifyPlaylist(SpotifyItemsModel *model, const QString &given) {
    const QString name = given.trimmed();
    if (!model || name.isEmpty() || !canCreatePlaylists())
        return;
    const QList<int> rows = model->chosenRows();
    const QList<Spotify::Item> items = rows.size() == 1 ? model->chosenItems() : model->chosenTracks();
    request(
        Spotify::Requests::Data, {},
        [name, items](Spotify::Backend &b, Spotify::Result &r) {
            // spotify_player's `playlist new` doesn't say the new id for
            // certain, so the list of playlists is read again and the
            // newest one with that name is it.
            QList<Spotify::Item> before = b.library("playlists", nullptr);
            QSet<QString> old;
            for (const Spotify::Item &p : before)
                old.insert(p.id);
            if (!b.createPlaylist(name, &r.error))
                return;
            QString id;
            for (const Spotify::Item &p : b.library("playlists", &r.error))
                if (p.name == name && !old.contains(p.id)) {
                    id = p.id;
                    break;
                }
            if (id.isEmpty()) {
                if (r.error.isEmpty())
                    r.error = QObject::tr("Spotify made the playlist but didn't list it yet; add the tracks in a minute");
                return;
            }
            r.error.clear();
            int added = 0;
            for (const Spotify::Item &i : items)
                if (b.addToPlaylist(id, i, nullptr))
                    ++added;
            r.extra = {{"id", id}, {"added", added}};
        },
        [this, name](const Spotify::Result &r) {
            if (r.cancelled)
                return;
            if (!r.ok()) {
                setMessage(r.error);
                return;
            }
            // Your playlists, read again, so the new one is in the menus
            // (and in the list, when it is on show).
            if (m_view.section == "playlists") {
                load(m_view, true);
            } else {
                View v;
                v.section = "playlists";
                fetchList(v, true, [](const Spotify::Result &) {});
            }
            setMessage(tr("Made the Spotify playlist %1 with %n tracks", "", r.extra.value("added").toInt()).arg(name));
        });
}

bool SpotifyController::addChosenToThrumPlaylist(SpotifyItemsModel *model, qint64 playlistId) {
    if (!model || playlistId <= 0)
        return false;
    const QList<int> rows = model->chosenRows();
    if (rows.size() == 1 && model->at(rows.first()).kind != "track") {
        // One album or playlist: its tracks, read first.
        const Spotify::Item item = model->at(rows.first());
        if (item.kind == "artist" || isRestricted(item)) {
            setMessage(tr("Open it and choose its tracks to put them in a Thrum playlist"));
            return false;
        }
        View v;
        v.section = "tracks";
        v.container = item;
        fetchList(v, false, [this, playlistId](const Spotify::Result &r) {
            if (r.cancelled)
                return;
            if (!r.ok()) {
                setMessage(r.error);
                return;
            }
            rememberTracks(r.items);
            QVariantList entries;
            for (const Spotify::Item &t : r.items)
                if (t.kind == "track")
                    entries << entryFor(t);
            emit thrumEntriesReady(playlistId, entries);
        });
        return true;
    }
    const QList<Spotify::Item> tracks = model->chosenTracks();
    if (tracks.isEmpty())
        return false;
    rememberTracks(tracks);
    QVariantList entries;
    for (const Spotify::Item &t : tracks)
        entries << entryFor(t);
    emit thrumEntriesReady(playlistId, entries);
    return true;
}

QString SpotifyController::copyChosenLinks(SpotifyItemsModel *model) {
    if (!model)
        return {};
    QStringList links;
    for (const Spotify::Item &i : model->chosenItems())
        links << i.webUrl();
    const QString text = links.join('\n');
    if (text.isEmpty())
        return {};
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        app->clipboard()->setText(text);
    setMessage(tr("Copied %n Spotify links", "", links.size()));
    return text;
}
void SpotifyController::addBlendRowToPlaylist(int row, const QString &playlistId) {
    addToPlaylist(m_blend.at(row), playlistId);
}

void SpotifyController::setPlayingLiked(bool liked) {
    const QString id = m_nowPlaying.value("uri").toString().section(':', 2);
    // Thrum's own track: saved by id, through the session. spotify_player's
    // command line could only like what spotify_player plays.
    const bool thrum = m_nowPlaying.value("thrum").toBool();
    const std::shared_ptr<SpotifyAudio> audio = m_audio;
    if (thrum && (id.isEmpty() || !audio)) {
        setMessage(tr("Thrum can't change Liked Songs here."));
        return;
    }
    request(
        Spotify::Requests::Control, {},
        [liked, thrum, id, audio](Spotify::Backend &b, Spotify::Result &r) {
            if (!thrum) {
                b.like(liked, &r.error);
                return;
            }
            const QJsonObject a = Spotify::SessionReader(audio).ask(
                {{"cmd", "like"}, {"tracks", QJsonArray{id}}, {"on", liked}}, Spotify::Backend::timeoutMs());
            if (!a.value("ok").toBool())
                r.error = a.value("error").toString(tr("Couldn't change Liked Songs."));
        },
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
                m_likedAsked.insert(id, liked);
            }
            m_cache.remove("library:liked");
            m_nowPlaying.insert("liked", liked);
            emit nowPlayingChanged();
            setMessage(liked ? tr("Saved to Liked Songs") : tr("Removed from Liked Songs"));
        });
}

// --- Thrum's own Spotify track ------------------------------------------------------

void SpotifyController::setPlayer(Player *player) {
    if (m_player) {
        disconnect(m_player, nullptr, this, nullptr);
        m_player->setSkipHandler({});
    }
    m_player = player;
    if (!player)
        return;
    connect(player, &Player::trackChanged, this, [this] {
        noteThrumPlaying();
        onPlayerForConnect();
    });
    connect(player, &Player::stateChanged, this, [this] {
        noteThrumPlaying();
        onPlayerForConnect();
    });
    connect(player, &Player::volumeChanged, this, [this] {
        const int cur = m_player->currentIndex();
        if (m_connect.active && cur >= 0 && SpotifyAudio::isConnect(m_player->playlist()->at(cur).path))
            tellConnect("volume", m_player->volume() / 100.0);
    });
    player->setSkipHandler([this](const QString &path, int direction) { return connectSkip(path, direction); });
    // A Connect entry left from the last run plays nothing now.
    dropConnectEntry();
}

Spotify::Item SpotifyController::thrumTrack() const {
    if (!m_player || !m_settings.enabled)
        return {};
    const int cur = m_player->currentIndex();
    if (cur < 0 || cur >= m_player->playlist()->count())
        return {};
    const Playlist::Track &entry = m_player->playlist()->at(cur);
    if (SpotifyAudio::isConnect(entry.path))
        return m_connect.track;
    const QString id = SpotifyAudio::trackId(entry.path);
    // Handed to spotify_player: its own state says what plays.
    if (id.isEmpty() || m_player->playsElsewhere())
        return {};
    Spotify::Item t = knownTrack(id);
    if (!t.isValid()) {
        t.kind = "track";
        t.id = id;
    }
    if (t.name.isEmpty())
        t.name = m_player->title().isEmpty() ? entry.title : m_player->title();
    if (t.artists.isEmpty() && !entry.artist.isEmpty())
        t.artists = QStringList{m_player->artist().isEmpty() ? entry.artist : m_player->artist()};
    if (t.album.isEmpty())
        t.album = m_player->album();
    if (t.durationMs <= 0)
        t.durationMs = m_player->duration();
    return t;
}

void SpotifyController::noteThrumPlaying() {
    const Spotify::Item t = thrumTrack();
    const QString state = m_player ? m_player->state() : QString();
    if (!t.isValid() || state == QLatin1String("stopped")) {
        if (m_nowPlaying.value("thrum").toBool()) {
            // Thrum is off Spotify: what the backend plays shows again.
            m_nowPlaying.clear();
            emit nowPlayingChanged();
            noteMpris();
        }
        return;
    }
    const int cur = m_player->currentIndex();
    const bool connect = SpotifyAudio::isConnect(m_player->playlist()->at(cur).path);
    m_art.want(t);
    const QString key = SpotifyArt::keyFor(t);
    QVariantMap m{{"active", true}, {"thrum", true}};
    m.insert("playing", state == QLatin1String("playing"));
    m.insert("title", t.name);
    m.insert("artists", t.artistText());
    m.insert("album", t.album);
    m.insert("progressMs", connect ? m_connect.positionMs : m_player->position());
    m.insert("durationMs", t.durationMs);
    m.insert("device", connect && !m_connect.client.isEmpty() ? tr("Thrum, from %1").arg(m_connect.client)
                                                              : QStringLiteral("Thrum"));
    m.insert("uri", t.uri());
    m.insert("artKey", key);
    m.insert("art", fileUrl(m_art.cachedFile(t)));
    const auto asked = m_likedAsked.constFind(t.id);
    m.insert("liked", asked != m_likedAsked.constEnd() ? asked.value() : m_liked.contains(t.id));
    m.insert("likedKnown", asked != m_likedAsked.constEnd() || m_likedKnown);
    if (asked == m_likedAsked.constEnd())
        checkLiked(t.id);
    if (m != m_nowPlaying) {
        m_nowPlaying = m;
        emit nowPlayingChanged();
    }
}

// Asks the session whether one track is in Liked Songs, once.
void SpotifyController::checkLiked(const QString &id) {
    if (!m_audio || id.isEmpty() || m_likedAsking.contains(id))
        return;
    m_likedAsking.insert(id);
    const std::shared_ptr<SpotifyAudio> audio = m_audio;
    request(
        Spotify::Requests::Control, "liked:" + id,
        [audio, id](Spotify::Backend &, Spotify::Result &r) {
            const QJsonObject a =
                Spotify::SessionReader(audio).ask({{"cmd", "liked"}, {"tracks", QJsonArray{id}}}, Spotify::Backend::timeoutMs());
            const QJsonArray found = a.value("liked").toArray();
            if (!a.value("ok").toBool() || found.isEmpty())
                r.error = a.value("error").toString("no answer");
            else
                r.extra.insert("liked", found.first().toBool());
        },
        [this, id](const Spotify::Result &r) {
            m_likedAsking.remove(id);
            if (!r.ok())
                return;
            const bool liked = r.extra.value("liked").toBool();
            m_likedAsked.insert(id, liked);
            if (liked)
                m_liked.insert(id);
            else if (m_likedKnown)
                m_liked.remove(id);
            noteThrumPlaying();
        });
}

// --- Whole playlists, albums and artists, as their tracks -------------------------------

bool SpotifyController::expandsContexts() const {
    const auto installed = SpotifyAudio::installed();
    return m_settings.enabled && installed && installed->enabled() && installed == m_audio;
}

QList<Playlist::Track> SpotifyController::queueTracks(const QList<Spotify::Item> &tracks) const {
    QList<Playlist::Track> list;
    for (const Spotify::Item &t : tracks) {
        Playlist::Track pt;
        pt.path = t.queuePath();
        pt.title = t.name.isEmpty() ? t.uri() : t.name;
        pt.artist = t.artistText();
        pt.durationMs = t.durationMs;
        list << pt;
    }
    return list;
}

void SpotifyController::readContextTracks(const Spotify::Item &context,
                                          std::function<void(const Spotify::Result &)> done) {
    View v;
    v.section = "tracks";
    v.container = context;
    fetchList(v, false, [done](const Spotify::Result &given) {
        Spotify::Result r = given;
        // An artist's page lists albums after the tracks: the tracks play.
        r.items.erase(std::remove_if(r.items.begin(), r.items.end(),
                                     [](const Spotify::Item &i) { return i.kind != "track"; }),
                      r.items.end());
        done(r);
    });
}

void SpotifyController::expandEntry(const QString &path, std::function<void()> failed) {
    QString kind, id;
    Spotify::parseReference(path, &kind, &id);
    Spotify::Item c = knownContext(kind, id);
    if (!c.isValid()) {
        c.kind = kind;
        c.id = id;
    }
    const bool shuffle = Spotify::queuePathShuffles(path);
    readContextTracks(c, [this, path, shuffle, failed](const Spotify::Result &r) {
        if (!m_player || r.cancelled)
            return;
        const int row = m_player->currentIndex();
        // Thrum moved on while it read.
        if (row < 0 || m_player->playlist()->at(row).path != path)
            return;
        if (!r.ok() || r.items.isEmpty()) {
            failed();
            return;
        }
        QList<Spotify::Item> tracks = r.items;
        if (shuffle)
            std::shuffle(tracks.begin(), tracks.end(), *QRandomGenerator::global());
        rememberTracks(tracks);
        m_player->expandEntry(row, queueTracks(tracks));
    });
}

// --- Thrum as a Spotify Connect device ---------------------------------------------------

void SpotifyController::setConnectDevice(bool on) {
    if (on == m_settings.connectDevice)
        return;
    m_settings.connectDevice = on;
    saveSettings();
    applyConnectSettings();
}

void SpotifyController::setConnectName(const QString &given) {
    const QString name = given.trimmed().left(64);
    if (name.isEmpty() || name == m_settings.connectName)
        return;
    m_settings.connectName = name;
    saveSettings();
    applyConnectSettings();
}

void SpotifyController::applyConnectSettings() {
    if (!m_audio)
        return;
    const bool on = m_settings.enabled && m_settings.connectDevice;
    m_audio->setConnectName(on ? m_settings.connectName : QString());
    if (!on) {
        m_connect = Connect();
        dropConnectEntry();
    }
    emit connectChanged();
    // The sidecar signs in on its own thread; the state is looked at until
    // it settles.
    if (on) {
        auto alive = m_alive;
        auto check = std::make_shared<std::function<void(int)>>();
        *check = [this, alive, check](int left) {
            if (!*alive)
                return;
            emit connectChanged();
            if (left > 0 && connectState() == QLatin1String("starting"))
                QTimer::singleShot(500, this, [check, left] { (*check)(left - 1); });
        };
        QTimer::singleShot(500, this, [check] { (*check)(80); });
    }
}

QString SpotifyController::connectState() const {
    if (!m_audio || !m_settings.enabled || !m_settings.connectDevice)
        return "off";
    if (!m_audio->connectFailure().isEmpty())
        return "failed";
    if (!m_audio->ready())
        return m_audio->lastFailure().isEmpty() ? "starting" : "failed";
    return m_connect.playing ? "playing" : "ready";
}

QString SpotifyController::connectError() const {
    if (!m_audio)
        return {};
    const QString connect = m_audio->connectFailure();
    return connect.isEmpty() ? m_audio->lastFailure() : connect;
}

void SpotifyController::tellConnect(const QString &action, const QJsonValue &value) {
    const std::shared_ptr<SpotifyAudio> audio = m_audio;
    if (!audio)
        return;
    QJsonObject request{{"cmd", "connect"}, {"action", action}};
    if (action == QLatin1String("seek"))
        request.insert("position_ms", value);
    else if (!value.isUndefined() && !value.isNull())
        request.insert("value", value);
    this->request(
        Spotify::Requests::Control, {},
        [audio, request](Spotify::Backend &, Spotify::Result &) {
            Spotify::SessionReader(audio).ask(request, Spotify::Backend::timeoutMs());
        },
        {});
}

bool SpotifyController::connectSkip(const QString &path, int direction) {
    if (!SpotifyAudio::isConnect(path) || !m_connect.active)
        return false;
    tellConnect(direction > 0 ? "next" : "previous");
    return true;
}

void SpotifyController::dropConnectEntry() {
    if (!m_player)
        return;
    Playlist *list = m_player->playlist();
    for (int row = list->count() - 1; row >= 0; --row)
        if (SpotifyAudio::isConnect(list->at(row).path) && row != m_player->currentIndex())
            m_player->remove(row);
}

void SpotifyController::onConnectEvent(const QJsonObject &event) {
    if (event.value("event").toString() != QLatin1String("connect") || !m_settings.enabled)
        return;
    m_connect.active = true;
    if (event.contains("client"))
        m_connect.client = event.value("client").toString();
    if (event.contains("position_ms"))
        m_connect.positionMs = event.value("position_ms").toInteger();
    const int cur = m_player ? m_player->currentIndex() : -1;
    const bool onConnect = cur >= 0 && SpotifyAudio::isConnect(m_player->playlist()->at(cur).path);
    if (event.contains("track")) {
        const QJsonObject o = event.value("track").toObject();
        Spotify::Item t;
        t.kind = "track";
        t.id = o.value("id").toString();
        t.name = o.value("title").toString();
        for (const QJsonValue &a : o.value("artists").toArray())
            t.artists << a.toString();
        t.album = o.value("album").toString();
        t.durationMs = o.value("duration_ms").toInteger();
        t.imageUrl = o.value("art").toString();
        m_connect.track = t;
        if (t.isValid())
            rememberTracks({t});
        if (onConnect)
            m_player->setLiveInfo(t.name, t.artistText(), t.album);
    }
    if (event.contains("volume") && onConnect)
        m_player->setVolume(qRound(event.value("volume").toDouble() * 100));
    const QString state = event.value("state").toString();
    if (state == QLatin1String("playing")) {
        m_connect.playing = true;
        if (m_player && !onConnect) {
            // The phone chose Thrum: its sound plays next, now.
            Playlist::Track pt;
            pt.path = SpotifyAudio::connectPath();
            pt.title = tr("Spotify Connect");
            pt.artist = m_connect.client.isEmpty() ? tr("Played from another device") : m_connect.client;
            if (cur < 0) {
                m_player->playTracks({pt}, 0);
            } else {
                m_player->queueNext({pt});
                m_player->playIndex(cur + 1);
            }
            const Spotify::Item t = m_connect.track;
            if (t.isValid())
                m_player->setLiveInfo(t.name, t.artistText(), t.album);
        } else if (m_player && m_player->state() != QLatin1String("playing")) {
            m_player->play();
        }
    } else if (state == QLatin1String("paused") || state == QLatin1String("stopped")) {
        m_connect.playing = false;
        if (onConnect && m_player->state() == QLatin1String("playing"))
            m_player->pause();
    }
    noteThrumPlaying();
    emit connectChanged();
}

// Thrum's own buttons, while it plays the Connect device: the phone sees
// Thrum pause and play, and Thrum moving on pauses the device.
void SpotifyController::onPlayerForConnect() {
    if (!m_player)
        return;
    const int cur = m_player->currentIndex();
    const QString path = cur >= 0 ? m_player->playlist()->at(cur).path : QString();
    const QString state = m_player->state();
    if (SpotifyAudio::isConnect(path)) {
        if (state == QLatin1String("playing") && !m_connect.playing && m_connect.active) {
            m_connect.playing = true;
            tellConnect("play");
        } else if (m_connect.playing && (state == QLatin1String("paused") ||
                                         (state == QLatin1String("stopped") && m_lastPlayerPath == path &&
                                          m_lastPlayerState == QLatin1String("playing")))) {
            m_connect.playing = false;
            tellConnect("pause");
        }
        if (m_connect.track.isValid())
            m_player->setLiveInfo(m_connect.track.name, m_connect.track.artistText(), m_connect.track.album);
    } else if (SpotifyAudio::isConnect(m_lastPlayerPath)) {
        if (m_connect.playing) {
            m_connect.playing = false;
            tellConnect("pause");
        }
        // Later: the Player is in the middle of changing tracks.
        QTimer::singleShot(0, this, &SpotifyController::dropConnectEntry);
        emit connectChanged();
    }
    m_lastPlayerPath = path;
    m_lastPlayerState = state;
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

// Thrum's volume slider sets the Spotify player's volume too, over its MPRIS
// service. Without MPRIS (Spotifast's own volume, or no service) it is left.
//
// The engines take a linear gain, made from the slider's perceptual scale.
// librespot's mixer applies its own curve, so it gets the slider's value
// back, or 30 % on the slider would be nearly silent on Spotify.
void SpotifyPlayback::setVolume(qreal linear) {
    m_c->mpris()->setVolume(QAudio::convertVolume(linear, QAudio::LinearVolumeScale, QAudio::LogarithmicVolumeScale));
}
