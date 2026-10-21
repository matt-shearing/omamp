#include "radiocontroller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "http.h"
#include "library.h"
#include "m3u.h"
#include "player.h"
#include "scrobble.h"
#include "streaming.h"

// ---- The model -----------------------------------------------------------

QVariant RadioStationsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Radio::Station &s = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return s.name;
    case KeyRole: return s.key;
    case FullNameRole: return s.fullName();
    case NetworkRole: return s.network;
    case NetworkNameRole: return s.networkName();
    case DescriptionRole: return s.description;
    case ColorRole: return s.color.isEmpty() ? QStringLiteral("#5b6ee1") : s.color;
    case FavouriteRole: return s.favourite;
    case NowPlayingRole: return nowPlaying ? nowPlaying(s) : QString();
    case PlayingRole: return !playingKey.isEmpty() && s.key == playingKey;
    case ArtUrlRole: return artUrl ? artUrl(s) : QString();
    case BuiltinRole: return s.builtin;
    case HomepageRole: return s.homepage;
    case DetailRole: {
        QStringList bits;
        if (!s.codec.isEmpty())
            bits << (s.bitrate > 0 ? QStringLiteral("%1 %2 kbps").arg(s.codec).arg(s.bitrate) : s.codec);
        if (!s.country.isEmpty())
            bits << s.country;
        return bits.join(QStringLiteral(" · "));
    }
    }
    return {};
}

QHash<int, QByteArray> RadioStationsModel::roleNames() const {
    return {{KeyRole, "key"},
            {NameRole, "name"},
            {FullNameRole, "fullName"},
            {NetworkRole, "network"},
            {NetworkNameRole, "networkName"},
            {DescriptionRole, "description"},
            {ColorRole, "stationColor"},
            {FavouriteRole, "favourite"},
            {NowPlayingRole, "nowPlaying"},
            {PlayingRole, "playing"},
            {ArtUrlRole, "artUrl"},
            {BuiltinRole, "builtin"},
            {HomepageRole, "homepage"},
            {DetailRole, "detail"}};
}

int RadioStationsModel::rowOf(const QString &key) const {
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].key == key)
            return i;
    return -1;
}

void RadioStationsModel::reset(const QList<Radio::Station> &rows) {
    const int before = m_rows.size();
    beginResetModel();
    m_rows = rows;
    endResetModel();
    if (before != m_rows.size())
        emit countChanged();
}

void RadioStationsModel::refreshRole(int role) {
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {role});
}

void RadioStationsModel::refreshKey(const QString &key, int role) {
    const int row = rowOf(key);
    if (row >= 0)
        emit dataChanged(index(row), index(row), {role});
}

// ---- The controller ------------------------------------------------------

namespace {
qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }
QByteArray hashOf(const QString &text) {
    return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
}
} // namespace

RadioController::RadioController(const QString &databasePath, QObject *parent)
    : QObject(parent), m_dbPath(databasePath.isEmpty() ? Library::defaultPath() : databasePath),
      m_cacheDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thrum/radio") {
    m_model.nowPlaying = [this](const Radio::Station &s) {
        if (s.key == m_current.key && !m_songTitle.isEmpty())
            return m_songArtist.isEmpty() ? m_songTitle : m_songArtist + QStringLiteral(" – ") + m_songTitle;
        if (s.metaKind == QLatin1String("nightride"))
            return m_feed.value(s.metaKey).line();
        return QString();
    };
    m_model.artUrl = [this](const Radio::Station &s) { return tilePath(s); };
    m_feedRetry.setSingleShot(true);
    connect(&m_feedRetry, &QTimer::timeout, this, &RadioController::connectFeed);

    m_importPicker.setup(tr("Import radio stations"), tr("Import"), false, true, tr("Station lists"),
                         {"*.pls", "*.m3u", "*.m3u8"});
    m_exportPicker.setup(tr("Save the radio stations in a folder"), tr("Save here"), true, false);
    connect(&m_importPicker, &FolderPicker::picked, this, [this](const QStringList &files) {
        int added = 0;
        QStringList errors;
        for (const QString &f : files) {
            const QVariantMap r = importFile(f);
            if (r.value("ok").toBool())
                added += r.value("added").toInt();
            else
                errors << r.value("error").toString();
        }
        emit toast(errors.isEmpty() ? (added == 1 ? tr("Added 1 station") : tr("Added %1 stations").arg(added)) : errors.join('\n'));
    });
    connect(&m_exportPicker, &FolderPicker::picked, this, [this](const QStringList &folders) {
        if (folders.isEmpty())
            return;
        const QVariantMap r = exportFile(QDir(folders.first()).filePath("Thrum radio.m3u8"), "all");
        emit toast(r.value("ok").toBool() ? tr("Saved %1").arg(r.value("file").toString()) : r.value("error").toString());
    });
    for (FolderPicker *p : {&m_importPicker, &m_exportPicker})
        connect(p, &FolderPicker::failed, this, &RadioController::toast);
}

RadioController::~RadioController() {
    if (m_feedReply)
        m_feedReply->abort();
    if (m_icyReply)
        m_icyReply->abort();
    ++m_searchGen;
    if (m_searchThread)
        m_searchThread->wait();
    for (QThread *t : m_threads)
        t->wait();
}

Library *RadioController::library() {
    if (m_library)
        return m_library.get();
    // Stations live in the library database, so using the radio makes one.
    auto library = std::make_unique<Library>(m_dbPath);
    QString error;
    if (!library->open(&error)) {
        emit toast(tr("Can't open the library for the radio: %1").arg(error));
        return nullptr;
    }
    m_library = std::move(library);
    ensureSeeded();
    return m_library.get();
}

void RadioController::ensureSeeded() {
    if (m_seeded || !m_library)
        return;
    QString error;
    if (!Radio::Stations(*m_library).seed(&error))
        emit toast(tr("Can't add the Nightride channels: %1").arg(error));
    m_seeded = true;
    reload();
}

void RadioController::reload() {
    if (!m_library) {
        // Before the radio is first used, the built-in channels show from
        // memory, so looking at them makes no database.
        QList<Radio::Station> rows;
        for (const Radio::Station &s : Radio::builtinStations())
            if (m_group == s.network || m_group == QLatin1String("all"))
                rows << s;
        m_model.reset(rows);
        return;
    }
    m_model.reset(Radio::Stations(*m_library).group(m_group));
    emit stationsChanged();
}

void RadioController::setGroup(const QString &group) {
    if (group == m_group || group.isEmpty())
        return;
    m_group = group;
    if (group != QLatin1String("nightride") && group != QLatin1String("rekt"))
        library();
    reload();
    emit groupChanged();
}

QVariantList RadioController::groups() const {
    return {QVariantMap{{"id", "nightride"}, {"label", tr("Nightride FM")}},
            QVariantMap{{"id", "rekt"}, {"label", tr("Rekt Network")}},
            QVariantMap{{"id", "favourites"}, {"label", tr("Favourites")}},
            QVariantMap{{"id", "mine"}, {"label", tr("My stations")}}};
}

QVariantMap RadioController::current() const {
    if (!m_current.isValid())
        return {};
    QVariantMap m = m_current.toMap();
    m.insert("nowPlaying", nowPlaying());
    return m;
}

QString RadioController::nowPlaying() const {
    if (m_songTitle.isEmpty())
        return {};
    return m_songArtist.isEmpty() ? m_songTitle : m_songArtist + QStringLiteral(" – ") + m_songTitle;
}

void RadioController::setPickerVisible(bool on) {
    if (on == m_pickerVisible)
        return;
    m_pickerVisible = on;
    if (on) {
        if (m_model.count() == 0)
            reload();
        refreshCatalog(false);
    }
    updateFeed();
    emit pickerVisibleChanged();
}

bool RadioController::scrobble() const {
    if (!QFileInfo::exists(m_dbPath))
        return false;
    auto *self = const_cast<RadioController *>(this);
    Library *lib = self->library();
    return lib && lib->setting("radio.scrobble", 0).toInt() == 1;
}

void RadioController::setScrobble(bool on) {
    if (Library *lib = library()) {
        lib->setSetting("radio.scrobble", on ? 1 : 0);
        emit scrobbleChanged();
    }
}

void RadioController::setPlayer(Player *player) {
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (!player)
        return;
    connect(player, &Player::trackChanged, this, &RadioController::onTrackChanged);
    connect(player, &Player::positionChanged, this, &RadioController::onPosition);
    connect(player, &Player::stateChanged, this, [this] {
        if (m_player && m_player->state() == QLatin1String("stopped") && m_current.isValid()) {
            // Stopped: what was heard counts, and the next play starts a new
            // listening session.
            finishSong();
            if (Library *lib = m_library.get())
                Radio::Stations(*lib).addListened(m_current.key, m_lastPos - m_listenStart);
            m_lastPos = m_listenStart = 0;
            m_firstSong = true;
        }
        updateFeed();
        updateIcyReader();
    });
    connect(player, &Player::liveStreamFailed, this, &RadioController::onLiveFailed);
    connect(player->engine(), &AudioEngine::metaDataChanged, this, &RadioController::onEngineMeta);
    onTrackChanged();
}

QUrl RadioController::resolve(const QString &path, QString *error) {
    const QString key = Radio::keyFromPath(path);
    Radio::Station station = key == m_current.key ? m_current : Radio::Station();
    if (!station.isValid()) {
        if (Library *lib = library())
            station = Radio::Stations(*lib).byKey(key);
    }
    if (!station.isValid() || station.streams.isEmpty()) {
        if (error)
            *error = tr("the station is no longer in the radio list");
        return {};
    }
    const int attempt = key == m_current.key ? m_attempt : 0;
    if (attempt >= station.streams.size()) {
        if (error)
            *error = tr("none of its addresses work");
        return {};
    }
    return QUrl(station.streams.at(attempt));
}

bool RadioController::tune(const QString &text) {
    if (!m_player)
        return false;
    Library *lib = library();
    if (!lib)
        return false;
    QStringList ambiguous;
    const Radio::Station station = Radio::Stations(*lib).find(text, &ambiguous);
    if (!station.isValid()) {
        emit toast(ambiguous.isEmpty() ? tr("No station called “%1”").arg(text)
                                       : tr("Which one? %1").arg(ambiguous.join(", ")));
        return false;
    }
    Playlist::Track track;
    track.path = station.path();
    track.title = station.name;
    track.artist = station.networkName();
    const int row = m_player->currentIndex();
    const bool replace = row >= 0 && m_player->live();
    // The station goes in after the current track and plays; a station
    // already playing gives way to it, so channel hopping leaves one row.
    m_attempt = 0;
    m_player->queueNext({track});
    m_player->playIndex(row + 1);
    if (replace)
        m_player->remove(row);
    if (station.network == QLatin1String("radio-browser") && !Net::Config::forcedOffline(lib)) {
        // radio-browser.info asks clients to say when a station is played.
        const QString uuid = station.key.mid(3);
        runInThread("thrum-radio-click", [uuid] {
            Net::HttpClient http;
            Radio::Browser::countClick(http, Radio::Browser::baseUrl(&http), uuid);
        });
    }
    return true;
}

bool RadioController::step(int delta) {
    if (!m_current.isValid() || !library())
        return false;
    // Through the playing station's own group, round and round.
    const QString group = m_current.favourite && m_group == QLatin1String("favourites") ? m_group : m_current.group();
    const QList<Radio::Station> list = Radio::Stations(*m_library).group(group);
    int at = -1;
    for (int i = 0; i < list.size(); ++i)
        if (list[i].key == m_current.key)
            at = i;
    if (at < 0 || list.size() < 2)
        return false;
    const int next = ((at + delta) % list.size() + list.size()) % list.size();
    return tune(list[next].key);
}

void RadioController::onTrackChanged() {
    if (!m_player)
        return;
    const int row = m_player->currentIndex();
    const QString path = row >= 0 && row < m_player->playlist()->count() ? m_player->playlist()->at(row).path : QString();
    const QString key = Radio::keyFromPath(path);
    if (key != m_current.key) {
        leaveStation();
        if (!key.isEmpty()) {
            if (Library *lib = library())
                m_current = Radio::Stations(*lib).byKey(key);
            if (m_current.isValid()) {
                Radio::Stations(*m_library).notePlayed(key);
                m_current.lastPlayed = nowSecs();
                m_firstSong = true;
                m_lastPos = m_listenStart = 0;
                // What the feed already knows plays at once.
                if (m_current.metaKind == QLatin1String("nightride") && m_feed.contains(m_current.metaKey)) {
                    const Radio::FeedRow r = m_feed.value(m_current.metaKey);
                    applySong(r.artist, r.title);
                }
            }
        }
        m_model.playingKey = m_current.key;
        m_model.refreshRole(RadioStationsModel::PlayingRole);
        m_model.refreshRole(RadioStationsModel::NowPlayingRole);
        emit currentChanged();
        emit nowPlayingChanged();
        updateFeed();
        updateIcyReader();
        return;
    }
    // The Player clears a station's song when it (re)loads it.
    if (m_current.isValid())
        pushLiveInfo();
}

void RadioController::pushLiveInfo() {
    if (!m_player || !m_current.isValid() || m_songTitle.isEmpty())
        return;
    m_player->setLiveInfo(m_songTitle, m_songArtist, m_current.fullName());
}

void RadioController::leaveStation() {
    if (!m_current.isValid())
        return;
    finishSong();
    if (Library *lib = m_library.get())
        Radio::Stations(*lib).addListened(m_current.key, m_lastPos - m_listenStart);
    if (m_icyReply)
        m_icyReply->abort();
    m_current = Radio::Station();
    m_songArtist.clear();
    m_songTitle.clear();
    m_attempt = 0;
    m_lastPos = m_listenStart = 0;
}

void RadioController::onPosition() {
    if (!m_player || !m_current.isValid() || !m_player->live())
        return;
    const qint64 pos = m_player->position();
    if (pos < m_lastPos) {
        // A reconnect (or a fallback address) restarted the clock.
        m_listenStart -= m_lastPos;
        m_songStartPos -= m_lastPos;
    }
    m_lastPos = pos;
    // Playing again: the next failure tries the first address again.
    if (pos > 5000)
        m_attempt = 0;
}

void RadioController::onEngineMeta() {
    if (!m_player || !m_current.isValid() || !m_player->live())
        return;
    const QString streamTitle = m_player->engine()->metaData().value("streamTitle").toString();
    if (streamTitle.isEmpty())
        return;
    // A Nightride channel's feed knows better (separate artist and title);
    // its in-stream title only stands in until the feed answers.
    if (m_current.metaKind == QLatin1String("none"))
        return;
    if (m_current.metaKind == QLatin1String("nightride") && m_feed.contains(m_current.metaKey))
        return;
    QString artist, title;
    Radio::splitStreamTitle(streamTitle, &artist, &title);
    applySong(artist, title);
}

void RadioController::applySong(const QString &artist, const QString &title) {
    if (!m_current.isValid() || (artist == m_songArtist && title == m_songTitle)) {
        pushLiveInfo();
        return;
    }
    finishSong();
    m_songArtist = artist;
    m_songTitle = title;
    m_songStartPos = m_lastPos;
    m_songStarted = nowSecs();
    // The song playing when the station was tuned began before we came in.
    m_songFromStart = !m_firstSong;
    m_firstSong = false;
    if (Library *lib = m_library.get())
        m_heardId = Radio::Stations(*lib).noteHeard(m_current.key, artist, title, m_songFromStart, m_songStarted);
    pushLiveInfo();
    m_model.refreshKey(m_current.key, RadioStationsModel::NowPlayingRole);
    emit nowPlayingChanged();
    emit songChanged(artist, title);
}

void RadioController::finishSong() {
    if (m_songTitle.isEmpty() || !m_library)
        return;
    const qint64 heard = qMax<qint64>(0, m_lastPos - m_songStartPos);
    if (m_heardId)
        Radio::Stations(*m_library).setHeardMs(m_heardId, heard);
    // Optional scrobbling: only a song heard from its start, for 30 s or
    // more, and only to services the listener set up.
    if (m_songFromStart && heard >= 30000 && !m_songArtist.isEmpty() &&
        m_library->setting("radio.scrobble", 0).toInt() == 1) {
        Scrobble::Scrobbler scrobbler(*m_library);
        Scrobble::Listen listen;
        listen.artist = m_songArtist;
        listen.title = m_songTitle;
        listen.durationMs = heard;
        listen.listenedAt = m_songStarted;
        int queued = 0;
        for (const QString &service : scrobbler.enabledServices())
            queued += scrobbler.enqueue(service, listen);
        if (queued > 0)
            emit scrobbleQueued();
    }
    m_heardId = 0;
    m_songArtist.clear();
    m_songTitle.clear();
}

void RadioController::onLiveFailed(const QString &path, const QString &detail) {
    if (!m_player || Radio::keyFromPath(path) != m_current.key)
        return;
    // The next address on the list, if the station has one.
    if (m_attempt + 1 < m_current.streams.size()) {
        ++m_attempt;
        m_player->playIndex(m_player->currentIndex());
        return;
    }
    m_attempt = 0;
    m_player->stop();
    m_player->reportError(detail.isEmpty() ? tr("Can't play %1").arg(m_current.fullName())
                                           : tr("Can't play %1: %2").arg(m_current.fullName(), detail));
}

// ---- The Nightride feed ----------------------------------------------------

void RadioController::updateFeed() {
    const bool playingNightride = m_current.isValid() && m_current.metaKind == QLatin1String("nightride") &&
                                  m_player && m_player->state() != QLatin1String("stopped");
    const bool wanted = (m_pickerVisible || playingNightride) && !Net::Config::forcedOffline(m_library.get());
    if (wanted && !m_feedReply && !m_feedRetry.isActive())
        connectFeed();
    else if (!wanted) {
        m_feedRetry.stop();
        if (m_feedReply) {
            QNetworkReply *reply = m_feedReply;
            m_feedReply = nullptr;
            reply->abort();
            reply->deleteLater();
        }
        if (m_feedConnected) {
            m_feedConnected = false;
            emit feedChanged();
        }
    }
}

void RadioController::connectFeed() {
    if (m_feedReply)
        return;
    QNetworkRequest request{QUrl(Radio::nightrideFeedUrl())};
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("User-Agent", Net::userAgent().toUtf8());
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(0);
    m_sse.reset();
    QNetworkReply *reply = m_nam.get(request);
    m_feedReply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != m_feedReply)
            return;
        if (!m_feedConnected) {
            m_feedConnected = true;
            m_feedBackoffMs = 2000;
            emit feedChanged();
        }
        for (const QByteArray &event : m_sse.feed(reply->readAll()))
            onFeedData(event);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_feedReply)
            return;
        m_feedReply = nullptr;
        if (m_feedConnected) {
            m_feedConnected = false;
            emit feedChanged();
        }
        // The feed closes now and then; come back after a pause that grows
        // while it keeps failing.
        m_feedRetry.start(m_feedBackoffMs);
        m_feedBackoffMs = qMin(60000, m_feedBackoffMs * 2);
    });
}

void RadioController::onFeedData(const QByteArray &data) {
    QSet<QString> known;
    for (const Radio::Station &s : Radio::builtinStations())
        known << s.metaKey;
    if (m_library)
        for (const Radio::Station &s : Radio::Stations(*m_library).group("nightride") +
                                           Radio::Stations(*m_library).group("rekt"))
            known << s.metaKey;
    for (const Radio::FeedRow &row : Radio::parseFeedData(data)) {
        m_feed.insert(row.station, row);
        if (!known.contains(row.station) && !m_unknownFeedIds.contains(row.station)) {
            m_unknownFeedIds << row.station;
            refreshCatalog(true);
        }
        for (int i = 0; i < m_model.count(); ++i)
            if (m_model.at(i).metaKey == row.station && m_model.at(i).metaKind == QLatin1String("nightride"))
                m_model.refreshKey(m_model.at(i).key, RadioStationsModel::NowPlayingRole);
        if (m_current.isValid() && m_current.metaKind == QLatin1String("nightride") && m_current.metaKey == row.station)
            applySong(row.artist, row.title);
    }
}

QString RadioController::nowPlayingFor(const QString &key) const {
    for (const Radio::Station &s : m_model.stations())
        if (s.key == key)
            return m_model.nowPlaying(s);
    const QString id = key.startsWith("nightride/") ? key.mid(10) : key;
    return m_feed.value(id).line();
}

// ---- ICY titles without the native engine ---------------------------------

void RadioController::updateIcyReader() {
    // The native engine reads a station's ICY titles itself. With the Qt
    // engine, a second, lightweight connection reads them (and drops the
    // audio) while the station plays.
    const bool wanted = m_player && m_current.isValid() && m_current.metaKind == QLatin1String("icy") &&
                        m_player->engineName() != QLatin1String("native") &&
                        m_player->state() == QLatin1String("playing") && !m_current.streams.isEmpty();
    if (!wanted) {
        if (m_icyReply) {
            QNetworkReply *reply = m_icyReply;
            m_icyReply = nullptr;
            reply->abort();
            reply->deleteLater();
        }
        return;
    }
    if (m_icyReply)
        return;
    QNetworkRequest request{QUrl(m_current.streams.value(m_attempt, m_current.streams.first()))};
    request.setRawHeader("Icy-MetaData", "1");
    request.setRawHeader("User-Agent", Streaming::userAgent().toUtf8());
    request.setTransferTimeout(0);
    QNetworkReply *reply = m_nam.get(request);
    m_icyReply = reply;
    m_icy.setMetaInt(0);
    const QString key = m_current.key;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, key] {
        if (reply != m_icyReply || key != m_current.key)
            return;
        if (m_icy.metaInt() == 0) {
            const int metaInt = reply->rawHeader("icy-metaint").toInt();
            if (metaInt <= 0) {
                // The station sends no titles: stop reading its audio.
                m_icyReply = nullptr;
                reply->abort();
                return;
            }
            m_icy.setMetaInt(metaInt);
        }
        for (const QString &t : m_icy.feed(reply->readAll())) {
            QString artist, title;
            Radio::splitStreamTitle(t, &artist, &title);
            applySong(artist, title);
        }
    });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

// ---- Stations ----------------------------------------------------------

bool RadioController::toggleFavourite(const QString &key) {
    Library *lib = library();
    if (!lib)
        return false;
    Radio::Stations stations(*lib);
    const Radio::Station s = stations.byKey(key);
    if (!s.isValid())
        return false;
    stations.setFavourite(key, !s.favourite);
    if (m_current.key == key)
        m_current.favourite = !s.favourite;
    emit toast(!s.favourite ? tr("%1 is a favourite").arg(s.fullName()) : tr("%1 is no longer a favourite").arg(s.fullName()));
    reload();
    emit currentChanged();
    return true;
}

bool RadioController::isFavourite(const QString &key) const {
    for (const Radio::Station &s : m_model.stations())
        if (s.key == key)
            return s.favourite;
    return m_current.key == key && m_current.favourite;
}

void RadioController::addUrl(const QString &text, const QString &name, bool play) {
    const QString url = text.trimmed();
    const QUrl parsed(url);
    if (!parsed.isValid() || (parsed.scheme() != "http" && parsed.scheme() != "https")) {
        emit toast(tr("That isn't an http or https address"));
        return;
    }
    const QString lower = parsed.path().toLower();
    if (lower.endsWith(".pls") || lower.endsWith(".m3u") || lower.endsWith(".m3u8")) {
        // A playlist that points at the stream: read it for the address.
        const QString file = parsed.fileName();
        runInThread("thrum-radio-list", [this, url, name, play, file] {
            Net::HttpClient http;
            const Net::Response r = http.get(Net::Service::Other, QUrl(url));
            QString stream, error;
            if (r.ok()) {
                QTemporaryDir dir;
                const QString path = dir.filePath(file.isEmpty() ? "list.pls" : file);
                QFile f(path);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(r.body);
                    f.close();
                }
                for (const PlaylistFiles::FileEntry &e : PlaylistFiles::read(path).entries)
                    if (e.location.startsWith("http", Qt::CaseInsensitive)) {
                        stream = e.location.trimmed();
                        break;
                    }
                if (stream.isEmpty())
                    error = tr("The list has no stream address in it");
            } else {
                error = r.error.isEmpty() ? tr("The address answered %1").arg(r.status) : r.error;
            }
            QMetaObject::invokeMethod(this, [this, stream, error, name, play] {
                if (!error.isEmpty()) {
                    emit toast(error);
                    return;
                }
                addUrl(stream, name, play);
            });
        });
        return;
    }
    Library *lib = library();
    if (!lib)
        return;
    Radio::Station s;
    s.name = name.trimmed();
    s.streams = {url};
    QString error;
    bool existed = false;
    const Radio::Station added = Radio::Stations(*lib).add(s, &error, &existed);
    if (!added.isValid()) {
        emit toast(tr("Can't add the station: %1").arg(error));
        return;
    }
    emit toast(existed ? tr("%1 is already in %2").arg(added.name, added.networkName())
                       : tr("Added %1 to My stations").arg(added.name));
    reload();
    if (play)
        tune(added.key);
}

bool RadioController::removeStation(const QString &key) {
    Library *lib = library();
    QString error;
    if (!lib || !Radio::Stations(*lib).remove(key, &error)) {
        emit toast(error.isEmpty() ? tr("Can't remove the station") : error);
        return false;
    }
    reload();
    return true;
}

bool RadioController::renameStation(const QString &key, const QString &name) {
    Library *lib = library();
    if (!lib || !Radio::Stations(*lib).rename(key, name))
        return false;
    if (m_current.key == key)
        m_current.name = name.trimmed();
    reload();
    emit currentChanged();
    return true;
}

QVariantList RadioController::allStations() {
    QList<Radio::Station> list = m_library ? Radio::Stations(*m_library).all() : Radio::builtinStations();
    if (!m_library && QFileInfo::exists(m_dbPath) && library())
        list = Radio::Stations(*m_library).all();
    QVariantList out;
    for (const Radio::Station &s : list)
        out << QVariantMap{{"key", s.key}, {"fullName", s.fullName()}, {"network", s.network},
                           {"networkName", s.networkName()}, {"description", s.description}};
    return out;
}

QVariantList RadioController::recentlyHeard(int limit) {
    QVariantList out;
    if (Library *lib = library())
        for (const QVariantMap &m : Radio::Stations(*lib).recentlyHeard(limit))
            out << m;
    return out;
}

int RadioController::countIn(const QString &group) {
    if (!m_library && (group == QLatin1String("nightride") || group == QLatin1String("rekt"))) {
        int n = 0;
        for (const Radio::Station &s : Radio::builtinStations())
            n += s.network == group;
        return n;
    }
    Library *lib = library();
    return lib ? int(Radio::Stations(*lib).group(group).size()) : 0;
}

QString RadioController::tilePath(const Radio::Station &s) {
    if (!s.artPath.isEmpty() && QFileInfo::exists(s.artPath))
        return QUrl::fromLocalFile(s.artPath).toString();
    const QString id = QString::fromLatin1(hashOf(s.key + '|' + s.name + '|' + s.color + '|' + s.network));
    auto it = m_tiles.constFind(id);
    if (it != m_tiles.constEnd())
        return *it;
    QDir().mkpath(m_cacheDir);
    const QString file = QDir(m_cacheDir).filePath("tile-" + id + ".png");
    if (!QFileInfo::exists(file))
        Radio::tile(s, 256).save(file, "PNG");
    const QString url = QUrl::fromLocalFile(file).toString();
    m_tiles.insert(id, url);
    return url;
}

// ---- Find stations (radio-browser.info) ----------------------------------

void RadioController::search(const QString &text) {
    const QString query = text.trimmed();
    if (query.isEmpty())
        return;
    const int gen = ++m_searchGen;
    m_searchedFor = query;
    m_searchError.clear();
    if (m_searchThread) {
        // The earlier search's answer is dropped when it arrives.
        QThread *old = m_searchThread;
        m_searchThread = nullptr;
        m_threads << old;
        connect(old, &QThread::finished, this, [this, old] {
            m_threads.removeAll(old);
            old->deleteLater();
        });
    }
    m_searchThread = QThread::create([this, query, gen] {
        Net::HttpClient http;
        Radio::Browser::Result result;
        if (Net::Config::forcedOffline())
            result.error = tr("Thrum is set to stay offline");
        else
            result = Radio::Browser::search(http, Radio::Browser::baseUrl(&http), query, 60);
        QMetaObject::invokeMethod(this, [this, result, gen] {
            if (gen != m_searchGen)
                return;
            if (m_searchThread) {
                m_searchThread->wait();
                m_searchThread->deleteLater();
                m_searchThread = nullptr;
            }
            m_results = result.stations;
            m_searchError = result.error;
            m_resultsKnown.clear();
            if (Library *lib = library()) {
                Radio::Stations stations(*lib);
                for (const Radio::Station &s : m_results)
                    if (stations.byKey(s.key).isValid() || stations.byStreamUrl(s.streams.value(0)).isValid())
                        m_resultsKnown << s.key;
            }
            emit searchChanged();
        });
    });
    m_searchThread->setObjectName("thrum-radio-search");
    m_searchThread->start(QThread::LowPriority);
    emit searchChanged();
}

QVariantList RadioController::results() const {
    QVariantList out;
    for (const Radio::Station &s : m_results) {
        QVariantMap m = s.toMap();
        m.insert("known", m_resultsKnown.contains(s.key));
        out << m;
    }
    return out;
}

QString RadioController::addResult(int index, bool play) {
    if (index < 0 || index >= m_results.size())
        return {};
    Library *lib = library();
    if (!lib)
        return {};
    QString error;
    bool existed = false;
    const Radio::Station added = Radio::Stations(*lib).add(m_results.at(index), &error, &existed);
    if (!added.isValid()) {
        emit toast(tr("Can't add the station: %1").arg(error));
        return {};
    }
    m_resultsKnown << m_results.at(index).key;
    emit searchChanged();
    if (!existed) {
        emit toast(tr("Added %1 to My stations").arg(added.name));
        if (!added.artUrl.isEmpty())
            fetchArt(added);
    }
    reload();
    if (play)
        tune(added.key);
    return added.key;
}

bool RadioController::previewResult(int index) {
    // Playing a result means keeping it: a station in the queue must be in
    // the list to resolve. It can be removed again from My stations.
    return !addResult(index, true).isEmpty();
}

void RadioController::fetchArt(const Radio::Station &station) {
    if (Net::Config::forcedOffline(m_library.get()))
        return;
    const QString key = station.key, url = station.artUrl;
    const QString file = QDir(m_cacheDir).filePath("icon-" + QString::fromLatin1(hashOf(key)));
    runInThread("thrum-radio-art", [this, key, url, file] {
        Net::HttpClient http;
        Net::Request r;
        r.service = Net::Service::Other;
        r.url = QUrl(url);
        r.retry = false;
        r.timeoutMs = 10000;
        const Net::Response answer = http.send(r);
        const QImage image = QImage::fromData(answer.body);
        if (!answer.ok() || image.isNull())
            return;
        QDir().mkpath(QFileInfo(file).path());
        const QString saved = file + ".png";
        if (!image.save(saved, "PNG"))
            return;
        QMetaObject::invokeMethod(this, [this, key, saved] {
            if (Library *lib = library()) {
                Radio::Stations(*lib).setArtPath(key, saved);
                reload();
            }
        });
    });
}

// ---- Lists and the catalogue -----------------------------------------------

void RadioController::importLists() { m_importPicker.pickFolders(); }
void RadioController::exportList() { m_exportPicker.pickFolders(); }

QVariantMap RadioController::importFile(const QString &file) {
    Library *lib = library();
    if (!lib)
        return {{"ok", false}, {"error", tr("The library isn't open")}};
    QString error;
    const int added = Radio::Stations(*lib).importFile(file, &error);
    if (added < 0)
        return {{"ok", false}, {"error", error}};
    reload();
    return {{"ok", true}, {"added", added}};
}

QVariantMap RadioController::exportFile(const QString &file, const QString &group) {
    Library *lib = library();
    if (!lib)
        return {{"ok", false}, {"error", tr("The library isn't open")}};
    QString error;
    if (!Radio::Stations(*lib).exportFile(file, group, &error))
        return {{"ok", false}, {"error", error}};
    return {{"ok", true}, {"file", file}};
}

void RadioController::refreshCatalog(bool force) {
    Library *lib = QFileInfo::exists(m_dbPath) || force ? library() : nullptr;
    if (!lib || m_catalogRunning || Net::Config::forcedOffline(lib))
        return;
    const qint64 last = lib->setting("radio.catalogChecked", 0).toLongLong();
    if (!force && nowSecs() - last < 24 * 3600)
        return;
    lib->setSetting("radio.catalogChecked", nowSecs());
    m_catalogRunning = true;
    const QStringList feedIds = m_feed.keys();
    runInThread("thrum-radio-catalog", [this, feedIds] {
        Net::HttpClient http;
        const Net::Response r = http.get(Net::Service::Other, QUrl(Radio::nightrideBase() + "/"), 6 * 3600);
        const QList<Radio::CatalogEntry> site =
            r.ok() ? Radio::parseNightrideHomepage(QString::fromUtf8(r.body)) : QList<Radio::CatalogEntry>();
        QMetaObject::invokeMethod(this, [this, site, feedIds, ok = r.ok()] {
            m_catalogRunning = false;
            Library *lib = library();
            // Without the page, a channel only in the feed can't be told
            // apart; it waits for the next check.
            if (!lib || !ok)
                return;
            const int added = Radio::Stations(*lib).mergeNightrideCatalog(site, feedIds);
            if (added > 0) {
                emit toast(added == 1 ? tr("Nightride has a new channel") : tr("Nightride has %1 new channels").arg(added));
                reload();
            }
        });
    });
}

void RadioController::runInThread(const QString &name, std::function<void()> work) {
    QThread *t = QThread::create(std::move(work));
    t->setObjectName(name);
    m_threads << t;
    connect(t, &QThread::finished, this, [this, t] {
        m_threads.removeAll(t);
        t->deleteLater();
    });
    t->start(QThread::LowPriority);
}

bool RadioController::openUri(const QString &uri) {
    if (Radio::isRadioPath(uri))
        return tune(uri);
    const QUrl url(uri);
    if (url.scheme() == "http" || url.scheme() == "https") {
        // A stream address from outside: keep it as a station and play it.
        addUrl(uri, {}, true);
        return true;
    }
    return false;
}
