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
    if (!Spotify::parseReference(path, &kind, &id) || kind != "track")
        return {};
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
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        if (m_state != AudioEngine::Playing)
            return;
        emit positionChanged(position());
        // Near the end, look more often, so the next track follows soon.
        const bool nearEnd = duration() > 0 && position() >= duration() - 3000;
        const int interval = nearEnd ? qMin(400, m_pollMs) : m_pollMs;
        // setInterval restarts a running timer, so only on a change.
        if (m_poll.interval() != interval)
            m_poll.setInterval(interval);
    });
}

void SpotifyPlayback::setTimings(int pollMs, int startTimeoutMs) {
    m_pollMs = pollMs;
    m_startTimeoutMs = startTimeoutMs;
    m_poll.setInterval(pollMs);
}

QVariantMap SpotifyPlayback::metaData() const {
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
    if (state == AudioEngine::Playing)
        m_tick.start();
    else
        m_tick.stop();
    emit stateChanged();
}

void SpotifyPlayback::load(const QString &path) {
    ++m_generation;
    m_poll.stop();
    m_tick.stop();
    m_path = path;
    QString kind, id;
    Spotify::parseReference(path, &kind, &id);
    m_track = m_c->knownTrack(id);
    if (!m_track.isValid()) {
        m_track = Spotify::Item();
        m_track.kind = "track";
        m_track.id = id;
    }
    m_state = AudioEngine::Stopped;
    m_started = m_starting = m_confirmed = m_pausedByUs = false;
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
    m_path.clear();
    m_state = AudioEngine::Stopped;
    m_started = m_starting = m_confirmed = false;
}

void SpotifyPlayback::fail(const QString &detail) {
    const bool wasStarting = m_starting;
    m_starting = false;
    m_poll.stop();
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

void SpotifyPlayback::play() {
    if (m_path.isEmpty())
        return;
    if (!m_c->enabled()) {
        fail(tr("Spotify is off. Switch it on in Settings → Spotify."));
        return;
    }
    if (m_track.id.isEmpty()) {
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
        m_poll.start();
        return;
    }
    m_started = true;
    m_starting = true;
    m_confirmed = false;
    m_startedAt.start();
    emit bufferingChanged();
    setState(AudioEngine::Playing);
    const QString id = m_track.id;
    QPointer<SpotifyPlayback> self(this);
    m_c->runControl([self, id, generation](Spotify::Backend &b) {
        QString error;
        if (b.playTrack(id, &error))
            return;
        QMetaObject::invokeMethod(
            self,
            [self, generation, error] {
                if (self && generation == self->m_generation)
                    self->fail(error);
            },
            Qt::QueuedConnection);
    });
    m_poll.start();
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
    if (m_polling || m_path.isEmpty())
        return;
    m_polling = true;
    const int generation = m_generation, commands = m_commands;
    QPointer<SpotifyPlayback> self(this);
    m_c->runControl([self, generation, commands](Spotify::Backend &b) {
        QString error;
        const Spotify::Playback p = b.playback(&error);
        QMetaObject::invokeMethod(
            self,
            [self, generation, commands, p, error] {
                if (!self)
                    return;
                self->m_polling = false;
                if (generation == self->m_generation && commands == self->m_commands)
                    self->onPolled(p, error);
            },
            Qt::QueuedConnection);
    });
}

void SpotifyPlayback::onPolled(const Spotify::Playback &p, const QString &error) {
    if (!error.isEmpty()) {
        if (m_starting && m_startedAt.elapsed() > m_startTimeoutMs)
            fail(error);
        return;
    }
    m_c->noteNowPlaying(p);
    const bool ours = p.valid && p.track.id == m_track.id;
    if (m_starting) {
        if (ours && p.playing) {
            m_starting = false;
            m_confirmed = true;
            emit bufferingChanged();
            // The playback state has the album and its picture.
            const qint64 oldDuration = m_track.durationMs;
            m_track = p.track;
            if (m_track.durationMs <= 0)
                m_track.durationMs = oldDuration;
            emit metaDataChanged();
            emit durationChanged(m_track.durationMs);
            setBase(p.progressMs);
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
    if (!ours) {
        if (m_state != AudioEngine::Playing)
            return;
        if (nearEnd) {
            finishTrack();
        } else {
            // Something else plays now: someone chose it in Spotify.
            setBase(position());
            m_pausedByUs = true;
            setState(AudioEngine::Paused);
            m_c->setMessage(tr("Spotify is playing something else now, so Omamp paused its queue."));
        }
        return;
    }
    // Just after our own command the backend may not have caught up.
    const bool settled = !m_lastCommand.isValid() || m_lastCommand.elapsed() > 1500;
    if (p.playing) {
        if (m_state != AudioEngine::Playing && settled) {
            m_pausedByUs = false;
            setState(AudioEngine::Playing);
        }
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

void SpotifyPlayback::finishTrack() {
    ++m_generation;
    m_poll.stop();
    m_started = m_starting = m_confirmed = false;
    setBase(duration());
    m_state = AudioEngine::Stopped;
    m_tick.stop();
    emit finished();
}

// --- SpotifyController ------------------------------------------------------------

SpotifyController::SpotifyController(const QString &settingsPath, const QString &artDir, QObject *parent)
    : QObject(parent), m_settingsPath(settingsPath.isEmpty() ? Spotify::Settings::defaultPath() : settingsPath),
      m_settings(Spotify::Settings::load(m_settingsPath)), m_art(artDir), m_items(&m_art), m_blend(&m_art),
      m_playback(this), m_launch(defaultLaunch) {
    m_control.setMaxThreadCount(1);
    m_data.setMaxThreadCount(1);
    makeBackend();

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

    m_signInWait.setInterval(3000);
    connect(&m_signInWait, &QTimer::timeout, this, [this] {
        if (m_backend->signedIn() || ++m_signInChecks > 100) {
            m_signInWait.stop();
            refreshStatus();
        }
    });
    m_viewPoll.setInterval(4000);
    connect(&m_viewPoll, &QTimer::timeout, this, &SpotifyController::pollNowPlaying);
    m_blendTimer.setSingleShot(true);
    m_blendTimer.setInterval(400);
    connect(&m_blendTimer, &QTimer::timeout, this, &SpotifyController::runBlend);
    m_paletteTimer.setSingleShot(true);
    m_paletteTimer.setInterval(400);
    connect(&m_paletteTimer, &QTimer::timeout, this, &SpotifyController::runPaletteSearch);

    if (m_settings.enabled)
        refreshStatus();
}

SpotifyController::~SpotifyController() {
    *m_alive = false;
    m_control.waitForDone();
    m_data.waitForDone();
}

void SpotifyController::waitForIdle() {
    m_control.waitForDone();
    m_data.waitForDone();
    QCoreApplication::processEvents();
}

void SpotifyController::makeBackend() { m_backend = Spotify::Backend::create(m_settings.backend); }

void SpotifyController::saveSettings() {
    m_settings.save(m_settingsPath);
    emit settingsChanged();
}

void SpotifyController::setEnabled(bool on) {
    if (on == m_settings.enabled)
        return;
    m_settings.enabled = on;
    saveSettings();
    if (on) {
        refreshStatus();
        load(m_view);
    } else {
        m_viewPoll.stop();
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
    m_status = Status();
    m_playlists.clear();
    m_items.setItems({});
    saveSettings();
    emit playlistsChanged();
    emit statusChanged();
    if (m_settings.enabled) {
        refreshStatus();
        load(View());
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

void SpotifyController::runControl(std::function<void(Spotify::Backend &)> work) {
    auto backend = m_backend;
    m_control.start([backend, work] { work(*backend); });
}

void SpotifyController::runData(std::function<void(Spotify::Backend &)> work) {
    auto backend = m_backend;
    m_data.start([backend, work] { work(*backend); });
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
    return tr("Spotify didn't start the track. %1's log may say why.").arg(m_backend->name());
}

void SpotifyController::refreshStatus() {
    if (m_checking)
        return;
    m_checking = true;
    emit statusChanged();
    const bool autoStart = m_settings.enabled && m_settings.startDaemon && m_settings.backend == "spotify_player";
    QPointer<SpotifyController> self(this);
    runData([self, autoStart](Spotify::Backend &b) {
        Status s;
        s.installed = b.installed();
        s.signedIn = b.signedIn();
        s.running = b.running();
        s.deviceName = b.deviceName();
        if (autoStart && s.installed && s.signedIn && !s.running) {
            b.start(&s.error);
            s.running = b.running();
        }
        QList<Spotify::Item> playlists;
        if (s.running) {
            QString error;
            const QList<Spotify::Device> devices = b.devices(&error);
            bool ours = false;
            for (const Spotify::Device &d : devices) {
                s.devices << QVariantMap{{"id", d.id}, {"name", d.name}, {"type", d.type}, {"active", d.active}};
                if (d.active)
                    s.device = d.name;
                ours = ours || d.name == s.deviceName;
            }
            if (ours)
                s.premium = "yes";
            if (b.caps().library) {
                playlists = b.library("playlists", &error);
                QHash<QString, int> owners;
                for (const Spotify::Item &p : playlists)
                    if (!p.owner.isEmpty() && p.owner != "Spotify")
                        ++owners[p.owner];
                int most = 0;
                for (auto it = owners.cbegin(); it != owners.cend(); ++it)
                    if (it.value() > most)
                        s.account = it.key(), most = it.value();
            }
            if (!error.isEmpty())
                s.error = error;
        }
        QMetaObject::invokeMethod(
            self,
            [self, s, playlists] {
                if (!self)
                    return;
                // A failure to play learned this session outlasts a check.
                const QString premium = self->m_status.premium;
                self->m_status = s;
                if (s.premium == "unknown" && premium == "no")
                    self->m_status.premium = premium;
                self->m_checking = false;
                if (s.running && self->m_backend->caps().library) {
                    self->m_playlists = playlists;
                    emit self->playlistsChanged();
                }
                emit self->statusChanged();
            },
            Qt::QueuedConnection);
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
    QPointer<SpotifyController> self(this);
    runData([self](Spotify::Backend &b) {
        QString error;
        const bool ok = b.start(&error);
        QMetaObject::invokeMethod(
            self,
            [self, ok, error] {
                if (!self)
                    return;
                if (!ok)
                    self->setMessage(error);
                self->refreshStatus();
            },
            Qt::QueuedConnection);
    });
}

void SpotifyController::connectDevice(const QString &id) {
    QPointer<SpotifyController> self(this);
    runControl([self, id](Spotify::Backend &b) {
        QString error;
        b.connectDevice(id, &error);
        QMetaObject::invokeMethod(
            self,
            [self, error] {
                if (!self)
                    return;
                if (!error.isEmpty())
                    self->setMessage(error);
                self->refreshStatus();
            },
            Qt::QueuedConnection);
    });
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
    emit loadingChanged();
}

void SpotifyController::load(const View &view) {
    m_view = view;
    m_view.error.clear();
    const int generation = ++m_viewGeneration;
    emit viewChanged();
    if (!m_settings.enabled) {
        m_items.setItems({});
        return;
    }
    if (!m_backend->caps().library) {
        m_items.setItems({});
        m_view.error = tr("%1 can't show your library or search. Choose spotify_player in Settings → Spotify for that; "
                          "links you paste still play.")
                           .arg(m_backend->name());
        emit viewChanged();
        return;
    }
    setLoading(true);
    QPointer<SpotifyController> self(this);
    const View v = m_view;
    runData([self, v, generation](Spotify::Backend &b) {
        QString error;
        QList<Spotify::Item> items;
        QStringList sections;
        if (v.section == "search") {
            const Spotify::SearchResults r = b.search(v.search, &error);
            auto add = [&](const QList<Spotify::Item> &list, const QString &heading) {
                for (const Spotify::Item &i : list) {
                    items << i;
                    sections << heading;
                }
            };
            add(r.tracks, tr("Tracks"));
            add(r.albums, tr("Albums"));
            add(r.artists, tr("Artists"));
            add(r.playlists, tr("Playlists"));
        } else if (v.section == "tracks") {
            items = b.tracksOf(v.container, &error);
        } else {
            items = b.library(v.section, &error);
        }
        QMetaObject::invokeMethod(
            self,
            [self, v, generation, items, sections, error] {
                if (!self || generation != self->m_viewGeneration)
                    return;
                self->setLoading(false);
                self->m_view.error = error;
                self->rememberTracks(items);
                self->m_items.setItems(items, sections);
                if (v.section == "playlists" && error.isEmpty()) {
                    self->m_playlists = items;
                    emit self->playlistsChanged();
                }
                if (v.section == "liked" && error.isEmpty()) {
                    self->m_liked.clear();
                    for (const Spotify::Item &i : items)
                        self->m_liked.insert(i.id);
                    self->m_likedKnown = true;
                }
                if (items.isEmpty() && error.isEmpty() && v.section == "search")
                    self->m_view.error = tr("Nothing on Spotify matches “%1”.").arg(v.search);
                emit self->viewChanged();
            },
            Qt::QueuedConnection);
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
    m_back << m_view;
    View v;
    v.section = "tracks";
    v.container = item;
    load(v);
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
    load(m_view);
    refreshStatus();
}

void SpotifyController::setViewVisible(bool visible) {
    m_viewVisible = visible;
    if (visible && m_settings.enabled) {
        pollNowPlaying();
        m_viewPoll.start();
    } else {
        m_viewPoll.stop();
    }
}

void SpotifyController::pollNowPlaying() {
    // While Omamp plays a Spotify track, its own polling keeps this fresh.
    if (m_nowPolling || !m_settings.enabled || !m_status.running || m_playback.state() == AudioEngine::Playing)
        return;
    m_nowPolling = true;
    QPointer<SpotifyController> self(this);
    runData([self](Spotify::Backend &b) {
        QString error;
        const Spotify::Playback p = b.playback(&error);
        QMetaObject::invokeMethod(
            self,
            [self, p, error] {
                if (!self)
                    return;
                self->m_nowPolling = false;
                if (error.isEmpty())
                    self->noteNowPlaying(p);
            },
            Qt::QueuedConnection);
    });
}

void SpotifyController::noteNowPlaying(const Spotify::Playback &p) {
    QVariantMap m{{"active", p.valid}};
    if (p.valid) {
        if (p.track.isValid())
            rememberTracks({p.track});
        m_art.want(p.track);
        const QString key = SpotifyArt::keyFor(p.track);
        m.insert("playing", p.playing);
        m.insert("title", p.track.name);
        m.insert("artists", p.track.artistText());
        m.insert("album", p.track.album);
        m.insert("progressMs", p.progressMs);
        m.insert("durationMs", p.track.durationMs);
        m.insert("device", p.deviceName);
        m.insert("uri", p.track.isValid() ? p.track.uri() : QString());
        m.insert("artKey", key);
        m.insert("art", fileUrl(m_art.cachedFile(p.track)));
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
    QPointer<SpotifyController> self(this);
    runData([self, item, mode](Spotify::Backend &b) {
        QString error;
        const QList<Spotify::Item> tracks = b.tracksOf(item, &error);
        QMetaObject::invokeMethod(
            self,
            [self, tracks, error, mode] {
                if (!self)
                    return;
                if (!error.isEmpty())
                    self->setMessage(error);
                else
                    self->queue(tracks, 0, mode);
            },
            Qt::QueuedConnection);
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
    m_back.clear();
    View v;
    v.section = "tracks";
    v.container = item;
    load(v);
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
        m_back.clear();
        View v;
        v.section = "tracks";
        v.container = container;
        load(v);
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
    QPointer<SpotifyController> self(this);
    runData([self, id, playIt](Spotify::Backend &b) {
        QString error;
        Spotify::Item t = b.track(id, &error);
        if (!t.isValid()) {
            t = Spotify::Item();
            t.kind = "track";
            t.id = id;
        }
        QMetaObject::invokeMethod(
            self,
            [self, t, playIt] {
                if (self)
                    playIt(t);
            },
            Qt::QueuedConnection);
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
    QPointer<SpotifyController> self(this);
    runData([self, item, playlistId, name](Spotify::Backend &b) {
        QString error;
        const bool ok = b.addToPlaylist(playlistId, item, &error);
        QMetaObject::invokeMethod(
            self,
            [self, ok, error, item, name] {
                if (self)
                    self->setMessage(ok ? tr("Added “%1” to %2").arg(item.name, name) : error);
            },
            Qt::QueuedConnection);
    });
}

void SpotifyController::addRowToPlaylist(int row, const QString &playlistId) { addToPlaylist(m_items.at(row), playlistId); }
void SpotifyController::addBlendRowToPlaylist(int row, const QString &playlistId) {
    addToPlaylist(m_blend.at(row), playlistId);
}

void SpotifyController::setPlayingLiked(bool liked) {
    const QString id = m_nowPlaying.value("uri").toString().section(':', 2);
    QPointer<SpotifyController> self(this);
    runControl([self, liked, id](Spotify::Backend &b) {
        QString error;
        const bool ok = b.like(liked, &error);
        QMetaObject::invokeMethod(
            self,
            [self, ok, error, liked, id] {
                if (!self)
                    return;
                if (!ok) {
                    self->setMessage(error);
                    return;
                }
                if (liked)
                    self->m_liked.insert(id);
                else
                    self->m_liked.remove(id);
                self->m_nowPlaying.insert("liked", liked);
                emit self->nowPlayingChanged();
                self->setMessage(liked ? tr("Saved to Liked Songs") : tr("Removed from Liked Songs"));
            },
            Qt::QueuedConnection);
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
    const QString text = m_blendText;
    const int generation = ++m_blendGeneration;
    m_blendLoading = true;
    emit blendChanged();
    QPointer<SpotifyController> self(this);
    runData([self, text, generation](Spotify::Backend &b) {
        QString error;
        const Spotify::SearchResults r = b.search(text, &error);
        QMetaObject::invokeMethod(
            self,
            [self, r, error, generation] {
                if (!self || generation != self->m_blendGeneration)
                    return;
                QList<Spotify::Item> items;
                QStringList sections;
                auto add = [&](const QList<Spotify::Item> &list, int max, const QString &heading) {
                    for (int i = 0; i < list.size() && i < max; ++i) {
                        items << list.at(i);
                        sections << heading;
                    }
                };
                add(r.tracks, 8, tr("Tracks"));
                add(r.albums, 6, tr("Albums"));
                add(r.artists, 4, tr("Artists"));
                add(r.playlists, 4, tr("Playlists"));
                self->rememberTracks(items);
                self->m_blend.setItems(items, sections);
                self->m_blendLoading = false;
                self->m_blendError = error;
                emit self->blendChanged();
            },
            Qt::QueuedConnection);
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
    QPointer<SpotifyController> self(this);
    runData([self, text, generation](Spotify::Backend &b) {
        QString error;
        const Spotify::SearchResults r = b.search(text, &error);
        QMetaObject::invokeMethod(
            self,
            [self, r, text, generation] {
                if (!self || !self->m_palette || generation != self->m_paletteGeneration)
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
                add(r.tracks, 4);
                add(r.albums, 2);
                add(r.artists, 2);
                add(r.playlists, 2);
                self->rememberTracks(r.tracks);
                self->m_palette->setExternalResults("Spotify", text, rows);
            },
            Qt::QueuedConnection);
    });
}
