#include "spotify.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QTimeZone>
#include <csignal>
#include <sys/types.h>

namespace Spotify {

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Spotify", text); }

QJsonArray toArray(const QStringList &list) {
    QJsonArray a;
    for (const QString &s : list)
        a << s;
    return a;
}

// The Spotify id inside whatever spotify_player wrote: a bare id, as
// rspotify serialises its id types, or a URI from older versions.
QString bareId(const QJsonValue &v) {
    QString s = v.toString();
    const int colon = s.lastIndexOf(':');
    return colon >= 0 ? s.mid(colon + 1) : s;
}

QString yearOf(const QString &date) { return date.left(4); }

void addArtists(const QJsonArray &artists, Item *item) {
    for (const QJsonValue &a : artists) {
        const QJsonObject o = a.toObject();
        item->artists << o.value("name").toString();
        item->artistIds << bareId(o.value("id"));
    }
}

// The largest picture of rspotify's images list.
QString largestImage(const QJsonArray &images) {
    QString best;
    int bestWidth = -1;
    for (const QJsonValue &v : images) {
        const QJsonObject o = v.toObject();
        const int w = o.value("width").toInt(0);
        if (w > bestWidth) {
            bestWidth = w;
            best = o.value("url").toString();
        }
    }
    return best;
}

// Finds the program on PATH, unless the environment names one.
QString findProgram(const QString &given, const char *envName, const QString &name) {
    QString p = given;
    if (p.isEmpty())
        p = qEnvironmentVariable(envName);
    if (p.isEmpty())
        p = name;
    if (QFileInfo(p).isAbsolute())
        return QFileInfo(p).isExecutable() ? p : QString();
    return QStandardPaths::findExecutable(p);
}

// Every running process: its executable and its arguments.
struct Proc {
    qint64 pid = 0;
    QString exe;
    QStringList arguments;
};
QList<Proc> processes() {
    QList<Proc> out;
    const QStringList pids = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool number = false;
        pid.toLongLong(&number);
        if (!number)
            continue;
        QFile f("/proc/" + pid + "/cmdline");
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray raw = f.readAll();
        if (raw.isEmpty())
            continue;
        Proc p;
        p.pid = pid.toLongLong();
        for (const QByteArray &part : raw.split('\0'))
            p.arguments << QString::fromLocal8Bit(part);
        if (!p.arguments.isEmpty() && p.arguments.last().isEmpty())
            p.arguments.removeLast();
        p.exe = QFileInfo("/proc/" + pid + "/exe").symLinkTarget();
        out << p;
    }
    return out;
}

QString canonical(const QString &path) {
    const QString c = QFileInfo(path).canonicalFilePath();
    return c.isEmpty() ? path : c;
}

// The arguments after the program itself, whether it ran directly (its
// executable) or as a script (the interpreter, then its path).
bool programArguments(const QString &exe, const QStringList &arguments, const QString &program, QStringList *rest) {
    const QString target = canonical(program);
    if (!exe.isEmpty() && canonical(exe) == target) {
        *rest = arguments.mid(1);
        return true;
    }
    if (arguments.size() > 1 && QFileInfo(arguments.at(1)).isAbsolute() && canonical(arguments.at(1)) == target) {
        *rest = arguments.mid(2);
        return true;
    }
    return false;
}

QString firstLine(const QByteArray &text) {
    const QString s = QString::fromUtf8(text).trimmed();
    const int nl = s.indexOf('\n');
    return (nl >= 0 ? s.left(nl) : s).trimmed();
}

} // namespace

// --- Rate limits and cancelling -------------------------------------------------

namespace {
thread_local CancelFlag t_cancel;
}

CancelScope::CancelScope(CancelFlag flag) : m_previous(t_cancel) { t_cancel = std::move(flag); }
CancelScope::~CancelScope() { t_cancel = m_previous; }
bool CancelScope::cancelled() { return t_cancel && t_cancel->load(); }
CancelFlag CancelScope::current() { return t_cancel; }

bool isRateLimitText(const QString &text) {
    return text.contains("429") || text.contains("Too Many Requests", Qt::CaseInsensitive) ||
           text.contains("rate limit", Qt::CaseInsensitive) || text.contains("rate-limit", Qt::CaseInsensitive);
}

LogNews readLogNews(const QString &cacheFolder, QString *file, qint64 *offset, int windowSecs, const QDateTime &now) {
    LogNews news;
    // The newest log: spotify_player starts one per run.
    const QFileInfoList logs = QDir(cacheFolder).entryInfoList({"spotify-player-*.log"}, QDir::Files, QDir::Time);
    if (logs.isEmpty())
        return news;
    const QString newest = logs.first().absoluteFilePath();
    QFile f(newest);
    if (!f.open(QIODevice::ReadOnly))
        return news;
    qint64 from = *offset;
    // A first look reads only the end; anything older is past the window.
    if (newest != *file)
        from = qMax<qint64>(0, f.size() - 256 * 1024);
    if (from > f.size())
        from = 0; // rotated or truncated
    f.seek(from);
    static const QRegularExpression stamp(R"(^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(\.\d+)?Z)");
    static const QRegularExpression retry(R"(retry_after_secs=(\d+))");
    qint64 consumed = from;
    while (!f.atEnd()) {
        const QByteArray raw = f.readLine();
        // A line still being written waits for the next read.
        if (!raw.endsWith('\n'))
            break;
        consumed += raw.size();
        const QString line = QString::fromUtf8(raw);
        // A refusal is a 4xx other than 429: a 429 is Spotify asking to wait.
        const bool refusal = line.contains("retrying with ncspot client") && !line.contains("status=429");
        if (!refusal && !line.contains("rate limit", Qt::CaseInsensitive) && !line.contains("429 Too Many Requests"))
            continue;
        QDateTime at;
        const auto m = stamp.match(line);
        if (m.hasMatch()) {
            at = QDateTime::fromString(m.captured(1), "yyyy-MM-ddTHH:mm:ss");
            at.setTimeZone(QTimeZone::UTC);
        }
        if (refusal) {
            // Kept for the day, whatever the window: what Spotify refuses
            // it keeps refusing.
            static const QRegularExpression fields(R"re(method="?(\w+)"? url="([^"]+)" status=(\d{3}))re");
            const auto f = fields.match(line);
            if (f.hasMatch() && (!at.isValid() || at.date() == now.date())) {
                Refusal r;
                r.method = f.captured(1);
                r.url = f.captured(2);
                r.status = f.captured(3).toInt();
                r.at = at.isValid() ? at : now;
                news.refusals << r;
            }
            continue;
        }
        if (at.isValid() && at.secsTo(now) > windowSecs)
            continue;
        if (!at.isValid())
            at = now;
        ++news.rateLimits;
        if (!news.lastAt.isValid() || at > news.lastAt)
            news.lastAt = at;
        const auto r = retry.match(line);
        if (r.hasMatch()) {
            const QDateTime until = at.addSecs(r.captured(1).toInt());
            if (!news.retryUntil.isValid() || until > news.retryUntil)
                news.retryUntil = until;
        }
    }
    *file = newest;
    *offset = consumed;
    return news;
}

QString refusalKey(const QString &url) {
    QString path = url.section('?', 0, 0);
    if (path.startsWith("http")) {
        const int v1 = path.indexOf("/v1/");
        path = v1 >= 0 ? path.mid(v1 + 4) : QUrl(path).path();
    }
    const QStringList p = path.split('/', Qt::SkipEmptyParts);
    if (p.size() >= 3 && p[0] == "artists")
        return "artist-" + p[2];
    if (p.size() >= 2 && p[0] == "playlists")
        return "playlist:" + p[1];
    if (!p.isEmpty() && p[0] == "recommendations")
        return "recommendations";
    static const QRegularExpression id("^[A-Za-z0-9]{22}$");
    QStringList out;
    for (const QString &part : p)
        out << (id.match(part).hasMatch() ? QString("*") : part);
    return out.join('/');
}

Refusals::Refusals(const QString &file) { setFile(file); }

void Refusals::setFile(const QString &file) {
    m_file = file;
    load();
}

void Refusals::setClientId(const QString &id) {
    if (id.compare(m_clientId, Qt::CaseInsensitive) == 0)
        return;
    m_clientId = id.toLower();
    load();
}

bool Refusals::learn(const QString &key, int status, const QDate &day) {
    if (key.isEmpty())
        return false;
    if (day != m_day) {
        m_status.clear();
        m_day = day;
    }
    const bool fresh = !m_status.contains(key);
    m_status.insert(key, status);
    if (fresh)
        save();
    return fresh;
}

bool Refusals::refused(const QString &key, const QDate &day) const { return day == m_day && m_status.contains(key); }

QStringList Refusals::keys(const QDate &day) const {
    if (day != m_day)
        return {};
    QStringList k = m_status.keys();
    k.sort();
    return k;
}

void Refusals::clear() {
    m_status.clear();
    save();
}

// { "<client id>": { "day": "2026-10-20", "refused": { "<key>": 403 } } }
void Refusals::load() {
    m_status.clear();
    m_day = QDate();
    QFile f(m_file);
    if (m_file.isEmpty() || !f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject mine = QJsonDocument::fromJson(f.readAll()).object().value(m_clientId).toObject();
    m_day = QDate::fromString(mine.value("day").toString(), Qt::ISODate);
    const QJsonObject refused = mine.value("refused").toObject();
    for (auto it = refused.begin(); it != refused.end(); ++it)
        m_status.insert(it.key(), it.value().toInt());
}

void Refusals::save() const {
    if (m_file.isEmpty())
        return;
    QJsonObject all;
    {
        QFile f(m_file);
        if (f.open(QIODevice::ReadOnly))
            all = QJsonDocument::fromJson(f.readAll()).object();
    }
    QJsonObject refused;
    for (auto it = m_status.cbegin(); it != m_status.cend(); ++it)
        refused.insert(it.key(), it.value());
    all.insert(m_clientId, QJsonObject{{"day", m_day.toString(Qt::ISODate)}, {"refused", refused}});
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile out(m_file);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(all).toJson());
        out.commit();
    }
}

// --- Items --------------------------------------------------------------------

QVariantMap Item::toMap() const {
    return {{"kind", kind},           {"id", id},         {"uri", uri()},         {"name", name},
            {"artists", artistText()}, {"album", album},   {"albumId", albumId},   {"durationMs", durationMs},
            {"year", year},           {"owner", owner},   {"imageUrl", imageUrl}, {"explicit", explicitContent},
            {"path", kind == "track" ? queuePath() : QString()}};
}

QJsonObject Item::toJson() const {
    QJsonObject o{{"kind", kind}, {"id", id}, {"uri", uri()}, {"name", name}};
    if (!artists.isEmpty())
        o.insert("artists", toArray(artists));
    if (!album.isEmpty())
        o.insert("album", album);
    if (!albumId.isEmpty())
        o.insert("album_id", albumId);
    if (durationMs > 0)
        o.insert("duration_ms", durationMs);
    if (!year.isEmpty())
        o.insert("year", year);
    if (!owner.isEmpty())
        o.insert("owner", owner);
    if (!imageUrl.isEmpty())
        o.insert("image_url", imageUrl);
    return o;
}

QJsonObject Playback::toJson() const {
    if (!valid)
        return {{"active", false}};
    return {{"active", true},     {"playing", playing},   {"track", track.toJson()},
            {"progress_ms", progressMs}, {"device", deviceName}, {"device_id", deviceId},
            {"volume", volume}};
}

QJsonObject Device::toJson() const {
    return {{"id", id}, {"name", name}, {"type", type}, {"active", active}, {"volume", volume}};
}

QJsonObject SearchResults::toJson() const {
    auto list = [](const QList<Item> &items) {
        QJsonArray a;
        for (const Item &i : items)
            a << i.toJson();
        return a;
    };
    return {{"tracks", list(tracks)}, {"albums", list(albums)}, {"artists", list(artists)},
            {"playlists", list(playlists)}};
}

bool isQueuePath(const QString &path) { return path.startsWith(QLatin1String("spotify://")); }

bool Item::spotifyMade() const {
    return kind == "playlist" &&
           (ownerId == "spotify" || (ownerId.isEmpty() && owner == "Spotify") || id.startsWith("37i9dQZF1"));
}

QString contextQueuePath(const Item &context, bool shuffle) {
    return Item::queuePathFor(context.kind, context.id) + (shuffle ? "?shuffle=1" : "");
}

bool queuePathShuffles(const QString &path) { return isQueuePath(path) && path.endsWith("?shuffle=1"); }

bool parseReference(const QString &text, QString *kind, QString *id, const QString &defaultKind) {
    static const QStringList kinds{"track", "album", "artist", "playlist"};
    static const QRegularExpression idPattern("^[A-Za-z0-9]{10,40}$");
    const QString t = text.trimmed();
    QString k, i;
    if (t.startsWith("spotify://")) {
        const QStringList parts = t.mid(10).section('?', 0, 0).split('/');
        if (parts.size() == 2)
            k = parts[0], i = parts[1];
    } else if (t.startsWith("spotify:")) {
        const QStringList parts = t.split(':');
        // spotify:track:<id>, or spotify:user:<u>:playlist:<id>.
        if (parts.size() >= 3)
            k = parts.at(parts.size() - 2), i = parts.last();
    } else if (t.contains("open.spotify.com/")) {
        const QUrl url(t);
        QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
        // /intl-de/track/<id> keeps a language first.
        if (!parts.isEmpty() && parts.first().startsWith("intl-"))
            parts.removeFirst();
        if (parts.size() >= 2)
            k = parts.at(0), i = parts.at(1);
    } else if (!defaultKind.isEmpty()) {
        k = defaultKind, i = t;
    }
    if (!kinds.contains(k) || !idPattern.match(i).hasMatch())
        return false;
    if (kind)
        *kind = k;
    if (id)
        *id = i;
    return true;
}

// --- spotify_player's JSON ----------------------------------------------------

QByteArray repair(const QByteArray &cliOutput) {
    QByteArray b = cliOutput.trimmed();
    b.replace("\r", "\\r");
    b.replace("\n", "\\n");
    return b;
}

Item parseTrack(const QJsonObject &o) {
    Item t;
    t.kind = "track";
    t.id = bareId(o.value("id"));
    t.name = o.value("name").toString();
    addArtists(o.value("artists").toArray(), &t);
    const QJsonObject album = o.value("album").toObject();
    t.album = album.value("name").toString();
    t.albumId = bareId(album.value("id"));
    t.year = yearOf(album.value("release_date").toString());
    // std::time::Duration: {"secs": 215, "nanos": 600000000}.
    const QJsonValue d = o.value("duration");
    if (d.isObject())
        t.durationMs = qint64(d.toObject().value("secs").toDouble()) * 1000 +
                       qint64(d.toObject().value("nanos").toDouble()) / 1000000;
    else if (o.contains("duration_ms"))
        t.durationMs = qint64(o.value("duration_ms").toDouble());
    t.explicitContent = o.value("explicit").toBool();
    return t;
}

Item parseAlbum(const QJsonObject &o) {
    Item a;
    a.kind = "album";
    a.id = bareId(o.value("id"));
    a.name = o.value("name").toString();
    addArtists(o.value("artists").toArray(), &a);
    a.year = yearOf(o.value("release_date").toString());
    a.album = a.name;
    a.albumId = a.id;
    return a;
}

Item parseArtist(const QJsonObject &o) {
    Item a;
    a.kind = "artist";
    a.id = bareId(o.value("id"));
    a.name = o.value("name").toString();
    return a;
}

Item parsePlaylist(const QJsonObject &o) {
    Item p;
    p.kind = "playlist";
    p.id = bareId(o.value("id"));
    p.name = o.value("name").toString();
    // A tuple: ["display name", "user id"].
    const QJsonValue owner = o.value("owner");
    p.owner = owner.isArray() ? owner.toArray().at(0).toString() : owner.toObject().value("display_name").toString();
    p.ownerId = owner.isArray() ? owner.toArray().at(1).toString() : owner.toObject().value("id").toString();
    return p;
}

QList<Item> parseItems(const QJsonArray &a, const QString &kind) {
    QList<Item> out;
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        Item i = kind == "track"   ? parseTrack(o)
                 : kind == "album" ? parseAlbum(o)
                 : kind == "artist" ? parseArtist(o)
                                    : parsePlaylist(o);
        if (i.isValid())
            out << i;
    }
    return out;
}

Item parseFullTrack(const QJsonObject &o) {
    Item t;
    t.kind = "track";
    t.id = bareId(o.value("id"));
    t.name = o.value("name").toString();
    addArtists(o.value("artists").toArray(), &t);
    const QJsonObject album = o.value("album").toObject();
    t.album = album.value("name").toString();
    t.albumId = bareId(album.value("id"));
    t.year = yearOf(album.value("release_date").toString());
    t.imageUrl = largestImage(album.value("images").toArray());
    t.durationMs = qint64(o.value("duration_ms").toDouble());
    t.explicitContent = o.value("explicit").toBool();
    return t;
}

Playback parsePlayback(const QJsonValue &v) {
    Playback p;
    if (!v.isObject())
        return p;
    const QJsonObject o = v.toObject();
    p.valid = true;
    p.playing = o.value("is_playing").toBool();
    p.progressMs = qint64(o.value("progress_ms").toDouble());
    const QJsonObject device = o.value("device").toObject();
    p.deviceId = device.value("id").toString();
    p.deviceName = device.value("name").toString();
    p.deviceType = device.value("type").toString();
    p.volume = device.value("volume_percent").isDouble() ? device.value("volume_percent").toInt() : -1;
    p.contextUri = o.value("context").toObject().value("uri").toString();
    const QJsonObject item = o.value("item").toObject();
    // An episode or something rspotify didn't know is shown by name only.
    if (!item.isEmpty())
        p.track = parseFullTrack(item);
    return p;
}

QList<Device> parseDevices(const QJsonArray &a) {
    QList<Device> out;
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        Device d;
        d.id = o.value("id").toString();
        d.name = o.value("name").toString();
        d.type = o.value("type").toString();
        d.active = o.value("is_active").toBool(o.value("active").toBool());
        d.volume = o.value("volume_percent").isDouble() ? o.value("volume_percent").toInt() : -1;
        if (!d.name.isEmpty())
            out << d;
    }
    return out;
}

SearchResults parseSearch(const QJsonObject &o) {
    SearchResults r;
    r.tracks = parseItems(o.value("tracks").toArray(), "track");
    r.albums = parseItems(o.value("albums").toArray(), "album");
    r.artists = parseItems(o.value("artists").toArray(), "artist");
    r.playlists = parseItems(o.value("playlists").toArray(), "playlist");
    return r;
}

QList<Item> parseContextTracks(const QJsonObject &o) {
    if (o.contains("top_tracks"))
        return parseItems(o.value("top_tracks").toArray(), "track");
    QList<Item> tracks = parseItems(o.value("tracks").toArray(), "track");
    // An album's tracks may leave out the album; it is the context's.
    if (o.contains("album")) {
        const Item album = parseAlbum(o.value("album").toObject());
        for (Item &t : tracks)
            if (t.albumId.isEmpty()) {
                t.album = album.name;
                t.albumId = album.id;
                t.year = album.year;
            }
    }
    return tracks;
}

// --- Settings -------------------------------------------------------------------

QString Settings::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omamp/spotify.conf";
}

Settings Settings::load(const QString &path) {
    const QSettings s(path.isEmpty() ? defaultPath() : path, QSettings::IniFormat);
    Settings out;
    out.enabled = s.value("spotify/enabled", false).toBool();
    out.backend = s.value("spotify/backend", "spotify_player").toString();
    if (!Backend::ids().contains(out.backend))
        out.backend = "spotify_player";
    out.blendSearch = s.value("spotify/blend_search", true).toBool();
    out.blendPalette = s.value("spotify/blend_palette", true).toBool();
    out.startDaemon = s.value("spotify/start_daemon", true).toBool();
    out.sessionReads = s.value("spotify/session_reads", true).toBool();
    return out;
}

bool Settings::save(const QString &path) const {
    const QString file = path.isEmpty() ? defaultPath() : path;
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSettings s(file, QSettings::IniFormat);
    s.setValue("spotify/enabled", enabled);
    s.setValue("spotify/backend", backend);
    s.setValue("spotify/blend_search", blendSearch);
    s.setValue("spotify/blend_palette", blendPalette);
    s.setValue("spotify/start_daemon", startDaemon);
    s.setValue("spotify/session_reads", sessionReads);
    s.sync();
    return s.status() == QSettings::NoError;
}

// --- Backend --------------------------------------------------------------------

int Backend::timeoutMs() {
    const int env = qEnvironmentVariableIntValue("OMAMP_SPOTIFY_TIMEOUT_MS");
    return env > 0 ? env : 20000;
}

int Backend::longTimeoutMs() {
    const int env = qEnvironmentVariableIntValue("OMAMP_SPOTIFY_LONG_TIMEOUT_MS");
    return env > 0 ? env : 60000;
}

QString Backend::unsupported(const QString &what) { return tr("%1 can't do that: %2").arg(what); }

QList<Item> Backend::library(const QString &, QString *error) {
    if (error)
        *error = tr("%1 has no library to read. Choose spotify_player in Settings → Spotify for your playlists and search.").arg(name());
    return {};
}

QList<Item> Backend::tracksOf(const Item &, QString *error) { return library({}, error); }

QList<Item> Backend::artistPage(const Item &, QStringList *, QString *error) { return library({}, error); }

Catalogue *Backend::catalogue() {
    if (!m_catalogue)
        m_catalogue = std::make_unique<WebApiCatalogue>(this);
    return m_catalogue.get();
}

QList<Item> WebApiCatalogue::artistPage(const Item &artist, QStringList *sections, QString *error) {
    return m_backend->artistPage(artist, sections, error);
}

QList<Item> WebApiCatalogue::playlistTracks(const Item &playlist, QString *error) {
    return m_backend->tracksOf(playlist, error);
}

bool Backend::playContext(const Item &context, bool, QString *error) { return playItem(context, error); }

SearchResults Backend::search(const QString &, QString *error) {
    if (error)
        *error = tr("%1 can't search. Choose spotify_player in Settings → Spotify to search Spotify.").arg(name());
    return {};
}

bool Backend::addToPlaylist(const QString &, const Item &, QString *error) {
    if (error)
        *error = tr("%1 can't change playlists.").arg(name());
    return false;
}

bool Backend::createPlaylist(const QString &, QString *error) {
    if (error)
        *error = tr("%1 can't make playlists.").arg(name());
    return false;
}

Item Backend::track(const QString &id, QString *) {
    Item t;
    t.kind = "track";
    t.id = id;
    return t;
}

QString Backend::account(QString *) { return {}; }

std::unique_ptr<Backend> Backend::create(const QString &id) {
    if (id == "spotifast")
        return std::make_unique<SpotifastBackend>();
    return std::make_unique<SpotifyPlayerBackend>();
}

bool Backend::runProgram(const QString &program, const QStringList &arguments, QByteArray *out, QString *error,
                         int timeout) {
    if (program.isEmpty()) {
        if (error)
            *error = tr("not installed");
        return false;
    }
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(program, arguments);
    if (!p.waitForStarted(5000)) {
        if (error)
            *error = tr("could not start %1").arg(QFileInfo(program).fileName());
        return false;
    }
    // In short steps, so a cancel from the window stops it soon.
    const int limit = timeout < 0 ? timeoutMs() : timeout;
    QElapsedTimer clock;
    clock.start();
    while (!p.waitForFinished(100) && p.state() != QProcess::NotRunning) {
        if (CancelScope::cancelled() || clock.elapsed() >= limit) {
            p.kill();
            p.waitForFinished(1000);
            if (error)
                *error = CancelScope::cancelled() ? tr("cancelled")
                                                  : tr("%1 did not answer in time").arg(QFileInfo(program).fileName());
            return false;
        }
    }
    const QByteArray stdoutText = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) {
            QString message = firstLine(p.readAllStandardError());
            if (message.isEmpty())
                message = firstLine(stdoutText);
            if (message.startsWith("Bad request: "))
                message = message.mid(13);
            *error = message.isEmpty() ? tr("%1 failed").arg(QFileInfo(program).fileName()) : message;
        }
        return false;
    }
    if (out)
        *out = stdoutText;
    return true;
}

// --- spotify_player -------------------------------------------------------------

SpotifyPlayerBackend::SpotifyPlayerBackend(const QString &program)
    : m_program(findProgram(program, "OMAMP_SPOTIFY_PLAYER", "spotify_player")) {}

QString SpotifyPlayerBackend::configFolder() {
    const QString env = qEnvironmentVariable("OMAMP_SPOTIFY_PLAYER_CONFIG");
    return env.isEmpty() ? QDir::homePath() + "/.config/spotify-player" : env;
}

QString SpotifyPlayerBackend::cacheFolder() {
    const QString env = qEnvironmentVariable("OMAMP_SPOTIFY_PLAYER_CACHE");
    return env.isEmpty() ? QDir::homePath() + "/.cache/spotify-player" : env;
}

bool SpotifyPlayerBackend::signedIn() const {
    // librespot's credentials.json and a Web API <client id>_token.json.
    // Only their names are looked at; Omamp never opens them.
    // With a client_id in app.toml, its own token must be there: a new ID
    // needs `spotify_player authenticate` again.
    const QDir cache(cacheFolder());
    if (!cache.exists("credentials.json"))
        return false;
    const QString id = configuredClientId();
    if (!id.isEmpty())
        return cache.exists(id + "_token.json");
    return !cache.entryList({"*_token.json"}, QDir::Files).isEmpty();
}

bool SpotifyPlayerBackend::stopInstances(QString *error) {
    if (m_program.isEmpty())
        return true;
    QList<qint64> pids;
    for (const Proc &p : processes())
        if (isInstance(p.exe, p.arguments, m_program))
            pids << p.pid;
    for (qint64 pid : pids)
        ::kill(pid_t(pid), SIGTERM);
    QElapsedTimer t;
    t.start();
    while (running() && t.elapsed() < 5000)
        QThread::msleep(100);
    if (running()) {
        for (qint64 pid : pids)
            ::kill(pid_t(pid), SIGKILL);
        QThread::msleep(300);
    }
    if (running()) {
        if (error)
            *error = tr("spotify_player is still running and wouldn't stop.");
        return false;
    }
    return true;
}

namespace {

// app.toml's lines, marking the key lines of the top level (before any
// [table], outside a multi-line string), which is where client_id lives.
struct TomlLine {
    QByteArray text;
    bool topLevelKey = false;
    bool tableHeader = false;
};

QList<TomlLine> tomlLines(const QByteArray &data) {
    static const QByteArray quotes[] = {QByteArray(R"(""")"), QByteArray("'''")};
    QList<TomlLine> out;
    bool inTable = false;
    QByteArray openQuote; // the quote of a multi-line string we are inside
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &text : lines) {
        TomlLine l;
        l.text = text;
        const QByteArray t = text.trimmed();
        if (openQuote.isEmpty()) {
            if (t.startsWith('[')) {
                inTable = true;
                l.tableHeader = true;
            } else if (!inTable && !t.isEmpty() && !t.startsWith('#')) {
                l.topLevelKey = true;
            }
            for (const QByteArray &q : quotes)
                if (text.count(q) % 2 == 1) {
                    openQuote = q;
                    break;
                }
        } else if (text.count(openQuote) % 2 == 1) {
            openQuote.clear();
        }
        out << l;
    }
    return out;
}

// A top-level key's value, unquoted; " " for a key with an empty value,
// empty when the key isn't there.
QString topLevelValue(const QString &configFolder, const QString &key) {
    QFile f((configFolder.isEmpty() ? SpotifyPlayerBackend::configFolder() : configFolder) + "/app.toml");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QRegularExpression re(R"(^\s*)" + QRegularExpression::escape(key) + R"(\s*=\s*(.*)$)");
    for (const TomlLine &l : tomlLines(f.readAll())) {
        if (!l.topLevelKey)
            continue;
        const auto m = re.match(QString::fromUtf8(l.text).trimmed());
        if (!m.hasMatch())
            continue;
        QString v = m.captured(1).trimmed();
        if (v.startsWith('"') || v.startsWith('\'')) {
            const int end = v.indexOf(v.at(0), 1);
            v = end > 0 ? v.mid(1, end - 1) : v.mid(1);
        } else if (v.contains('#')) {
            v = v.left(v.indexOf('#'));
        }
        v = v.trimmed();
        return v.isEmpty() ? QString(" ") : v;
    }
    return {};
}

} // namespace

QString SpotifyPlayerBackend::configuredClientId(const QString &folder) {
    return topLevelValue(folder, "client_id").trimmed();
}

bool SpotifyPlayerBackend::hasClientIdCommand(const QString &folder) {
    return !topLevelValue(folder, "client_id_command").isEmpty();
}

QString SpotifyPlayerBackend::effectiveClientId(const QString &folder) {
    const QString id = configuredClientId(folder);
    return id.isEmpty() ? ncspotClientId() : id;
}

bool SpotifyPlayerBackend::isValidClientId(const QString &id) {
    static const QRegularExpression re("^[0-9a-fA-F]{32}$");
    return re.match(id.trimmed()).hasMatch();
}

bool SpotifyPlayerBackend::writeClientId(const QString &folder, const QString &givenId, QString *backup, QString *error) {
    const QString id = givenId.trimmed().toLower();
    const QString dir = folder.isEmpty() ? configFolder() : folder;
    const QString path = dir + "/app.toml";
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!isValidClientId(id))
        return fail(tr("That isn't a Spotify client ID. A client ID is 32 letters and digits, shown on your app's "
                       "page at developer.spotify.com."));
    if (hasClientIdCommand(dir))
        return fail(tr("app.toml sets client_id_command, which spotify_player uses instead of client_id. Remove "
                       "that line from %1 first.")
                        .arg(path));
    QByteArray data;
    QFile in(path);
    if (in.exists()) {
        if (!in.open(QIODevice::ReadOnly))
            return fail(tr("Couldn't read %1.").arg(path));
        data = in.readAll();
        in.close();
        const QString stem = path + ".omamp-backup-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        QString copy = stem;
        for (int n = 2; QFileInfo::exists(copy); ++n)
            copy = stem + '-' + QString::number(n);
        if (!QFile::copy(path, copy))
            return fail(tr("Couldn't back up %1, so it was left alone.").arg(path));
        if (backup)
            *backup = copy;
    } else if (!QDir().mkpath(dir)) {
        return fail(tr("Couldn't make %1.").arg(dir));
    }

    // spotify_player 0.25 sends playlist reads through ncspot's shared
    // client ID whatever client_id says (ncspot_only_get_endpoints), and that
    // ID is rate-limited across everyone who uses it. An empty list sends
    // them through this ID too; spotify_player still falls back to ncspot's
    // when this one is refused (Spotify's own playlists, for new apps).
    const QByteArray line = "client_id = \"" + id.toLatin1() + "\"\nncspot_only_get_endpoints = []";
    QList<TomlLine> lines = tomlLines(data);
    static const QRegularExpression key(R"(^\s*client_id\s*=)");
    static const QRegularExpression endpoints(R"(^\s*ncspot_only_get_endpoints\s*=)");
    // The old endpoint list goes; the line above brings the new one.
    for (int i = lines.size() - 1; i >= 0; --i)
        if (lines[i].topLevelKey && endpoints.match(QString::fromUtf8(lines[i].text)).hasMatch())
            lines.removeAt(i);
    bool done = false;
    for (TomlLine &l : lines)
        if (l.topLevelKey && key.match(QString::fromUtf8(l.text)).hasMatch()) {
            l.text = line + (l.text.endsWith('\r') ? "\r" : "");
            done = true;
            break;
        }
    QList<QByteArray> out;
    for (const TomlLine &l : lines) {
        if (!done && l.tableHeader) {
            out << line;
            done = true;
        }
        out << l.text;
    }
    if (!done) {
        // No table to go above: at the end, before the final newline.
        if (data.isEmpty())
            out = {line, QByteArray()};
        else if (out.last().isEmpty())
            out.insert(out.size() - 1, line);
        else
            out << line << QByteArray();
    }
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly) || save.write(out.join('\n')) < 0 || !save.commit())
        return fail(tr("Couldn't write %1.").arg(path));
    return true;
}

bool SpotifyPlayerBackend::isInstance(const QString &exe, const QStringList &arguments, const QString &program) {
    QStringList rest;
    if (program.isEmpty() || !programArguments(exe, arguments, program, &rest))
        return false;
    // The app or the daemon takes options only; a CLI command names one.
    static const QStringList commands{"get",    "playback", "connect", "like",     "authenticate", "playlist",
                                      "generate", "search", "features", "lyrics", "help"};
    for (const QString &a : rest)
        if (commands.contains(a))
            return false;
    return true;
}

bool SpotifyPlayerBackend::running() const {
    if (m_program.isEmpty())
        return false;
    for (const Proc &p : processes())
        if (isInstance(p.exe, p.arguments, m_program))
            return true;
    return false;
}

bool SpotifyPlayerBackend::start(QString *error) {
    if (m_program.isEmpty()) {
        if (error)
            *error = tr("spotify_player isn't installed. Install it with: omarchy-pkg-add spotify-player");
        return false;
    }
    if (running())
        return true;
    if (!signedIn()) {
        if (error)
            *error = tr("Sign in to spotify_player first.");
        return false;
    }
    // The daemon detaches itself; it has nothing to read.
    QProcess p;
    p.setProgram(m_program);
    p.setArguments({"--daemon"});
    p.setStandardInputFile(QProcess::nullDevice());
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    if (!p.startDetached()) {
        if (error)
            *error = tr("could not start spotify_player");
        return false;
    }
    for (int i = 0; i < 40; ++i) {
        if (running())
            return true;
        QThread::msleep(100);
    }
    if (error)
        *error = tr("spotify_player started but isn't running. Its log is in %1.").arg(cacheFolder());
    return false;
}

QStringList SpotifyPlayerBackend::signInCommand() const {
    return {m_program.isEmpty() ? QString("spotify_player") : m_program, "authenticate"};
}

QString SpotifyPlayerBackend::deviceName() const {
    // [device] name in app.toml (not a secret); spotify-player otherwise.
    QFile f(configFolder() + "/app.toml");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool inDevice = false;
        static const QRegularExpression nameLine("^\\s*name\\s*=\\s*\"([^\"]*)\"");
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith('['))
                inDevice = line == "[device]";
            else if (inDevice) {
                const auto m = nameLine.match(line);
                if (m.hasMatch())
                    return m.captured(1);
            }
        }
    }
    return "spotify-player";
}

bool SpotifyPlayerBackend::command(const QStringList &arguments, QByteArray *out, QString *error, int timeout) const {
    if (m_program.isEmpty()) {
        if (error)
            *error = tr("spotify_player isn't installed.");
        return false;
    }
    if (!signedIn()) {
        if (error)
            *error = tr("spotify_player isn't signed in to Spotify. Settings → Spotify → Sign in.");
        return false;
    }
    if (!running()) {
        if (error)
            *error = tr("spotify_player isn't running. Settings → Spotify → Start.");
        return false;
    }
    QString why;
    if (runProgram(m_program, arguments, out, &why, timeout))
        return true;
    if (error) {
        if (why.contains("PREMIUM", Qt::CaseInsensitive))
            *error = tr("Spotify says this account needs Premium to play here. You can still browse and search.");
        else
            *error = why;
    }
    return false;
}

bool SpotifyPlayerBackend::json(const QStringList &arguments, QJsonValue *value, QString *error, int timeout) const {
    QByteArray out;
    if (!command(arguments, &out, error, timeout))
        return false;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson("[" + repair(out) + "]", &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error)
            *error = tr("spotify_player answered something Omamp can't read: %1").arg(parseError.errorString());
        return false;
    }
    *value = doc.array().at(0);
    return true;
}

Playback SpotifyPlayerBackend::playback(QString *error) {
    QJsonValue v;
    if (!json({"get", "key", "playback"}, &v, error))
        return {};
    return parsePlayback(v);
}

bool SpotifyPlayerBackend::playTrack(const QString &id, QString *error) {
    return command({"playback", "start", "track", "--id", id}, nullptr, error);
}

bool SpotifyPlayerBackend::playItem(const Item &item, QString *error) {
    if (item.kind == "track")
        return playTrack(item.id, error);
    return command({"playback", "start", "context", "--id", item.id, item.kind}, nullptr, error);
}

bool SpotifyPlayerBackend::resume(QString *error) { return command({"playback", "play"}, nullptr, error); }
bool SpotifyPlayerBackend::pause(QString *error) { return command({"playback", "pause"}, nullptr, error); }

bool SpotifyPlayerBackend::seekTo(qint64 ms, qint64 currentMs, QString *error) {
    // spotify_player seeks by an offset from where it is.
    const qint64 offset = ms - currentMs;
    if (offset == 0)
        return true;
    return command({"playback", "seek", "--", QString::number(offset)}, nullptr, error);
}

bool SpotifyPlayerBackend::like(bool on, QString *error) {
    return command(on ? QStringList{"like"} : QStringList{"like", "--unlike"}, nullptr, error);
}

QList<Device> SpotifyPlayerBackend::devices(QString *error) {
    QJsonValue v;
    if (!json({"get", "key", "devices"}, &v, error))
        return {};
    return parseDevices(v.toArray());
}

bool SpotifyPlayerBackend::connectDevice(const QString &id, QString *error) {
    return command({"connect", "--id", id}, nullptr, error);
}

QList<Item> SpotifyPlayerBackend::library(const QString &section, QString *error) {
    static const QHash<QString, QPair<QString, QString>> keys{
        {"playlists", {"user-playlists", "playlist"}},
        {"liked", {"user-liked-tracks", "track"}},
        {"albums", {"user-saved-albums", "album"}},
        {"artists", {"user-followed-artists", "artist"}},
        {"top", {"user-top-tracks", "track"}},
    };
    const auto key = keys.value(section);
    if (key.first.isEmpty()) {
        if (error)
            *error = tr("unknown section %1").arg(section);
        return {};
    }
    QJsonValue v;
    if (!json({"get", "key", key.first}, &v, error, longTimeoutMs()))
        return {};
    return parseItems(v.toArray(), key.second);
}

QList<Item> SpotifyPlayerBackend::tracksOf(const Item &container, QString *error) {
    if (!QStringList{"playlist", "album", "artist"}.contains(container.kind)) {
        if (error)
            *error = tr("only a playlist, an album or an artist has tracks");
        return {};
    }
    // Never `get item … artist`: it reads the artist's top tracks and
    // related artists, which Spotify refuses to new apps, and then waits on
    // ncspot's shared, rate-limited client ID.
    if (container.kind == "artist") {
        QList<Item> tracks;
        for (const Item &i : artistPage(container, nullptr, error))
            if (i.kind == "track")
                tracks << i;
        return tracks;
    }
    QJsonValue v;
    if (!json({"get", "item", "--id", container.id, container.kind}, &v, error, longTimeoutMs()))
        return {};
    return parseContextTracks(v.toObject());
}

QList<Item> SpotifyPlayerBackend::artistPage(const Item &artist, QStringList *sections, QString *error) {
    QString name = artist.name;
    // Known by id only (a link): your followed artists may name it, and
    // Spotify lets every app read those quickly.
    if (name.trimmed().isEmpty()) {
        QString ignored;
        for (const Item &a : library("artists", &ignored))
            if (a.id == artist.id)
                name = a.name;
    }
    name.remove('"');
    name = name.trimmed();
    if (name.isEmpty()) {
        if (error)
            *error = tr("Omamp doesn't know this artist's name, so it can't look the artist up.");
        return {};
    }
    // Two searches: the plain name ranks the best-known tracks first, and
    // artist:"name" finds more of the artist's own albums. Spotify answers
    // at most ten of each kind.
    QString firstError;
    const SearchResults plain = search(name, &firstError);
    QString secondError;
    const SearchResults byArtist = search("artist:\"" + name + "\"", &secondError);
    if (!firstError.isEmpty() && !secondError.isEmpty()) {
        if (error)
            *error = firstError;
        return {};
    }
    auto theirs = [&artist](const Item &i) { return i.artistIds.contains(artist.id); };
    QList<Item> tracks, albums;
    QSet<QString> seen;
    // A track and its namesake single share a name; tracks are kept by id
    // and by name, so the list doesn't repeat itself.
    QSet<QString> names;
    for (const QList<Item> *list : {&plain.tracks, &byArtist.tracks})
        for (const Item &t : *list)
            if (theirs(t) && !seen.contains(t.id) && !names.contains(t.name.toLower())) {
                seen.insert(t.id);
                names.insert(t.name.toLower());
                tracks << t;
            }
    names.clear();
    for (const QList<Item> *list : {&byArtist.albums, &plain.albums})
        for (const Item &a : *list)
            if (theirs(a) && !seen.contains(a.id) && !names.contains(a.name.toLower() + '|' + a.year)) {
                seen.insert(a.id);
                names.insert(a.name.toLower() + '|' + a.year);
                albums << a;
            }
    // Newest album first, as the artist's own page does.
    std::stable_sort(albums.begin(), albums.end(), [](const Item &a, const Item &b) { return a.year > b.year; });
    if (sections) {
        sections->clear();
        for (int i = 0; i < tracks.size(); ++i)
            *sections << tr("Popular tracks");
        for (int i = 0; i < albums.size(); ++i)
            *sections << tr("Albums");
    }
    return tracks + albums;
}

bool SpotifyPlayerBackend::playContext(const Item &context, bool shuffle, QString *error) {
    if (context.kind == "track")
        return playTrack(context.id, error);
    // With an id, spotify_player looks nothing up: it asks Spotify's player
    // (PUT me/player/play with the context) and, when shuffling, sets
    // shuffle (PUT me/player/shuffle). Neither reads the tracks.
    QStringList arguments{"playback", "start", "context", "--id", context.id, context.kind};
    if (shuffle)
        arguments << "--shuffle";
    return command(arguments, nullptr, error);
}

SearchResults SpotifyPlayerBackend::search(const QString &text, QString *error) {
    QJsonValue v;
    if (text.trimmed().isEmpty() || !json({"search", "--", text.trimmed()}, &v, error, longTimeoutMs()))
        return {};
    return parseSearch(v.toObject());
}

bool SpotifyPlayerBackend::addToPlaylist(const QString &playlistId, const Item &item, QString *error) {
    if (item.kind != "track" && item.kind != "album") {
        if (error)
            *error = tr("only a track or an album can go in a playlist");
        return false;
    }
    return command({"playlist", "edit", item.kind == "track" ? "--track-id" : "--album-id", item.id, "add", playlistId},
                   nullptr, error);
}

// `playlist new <name>` makes a private, non-collaborative playlist. "--"
// keeps a name that starts with a dash from reading as an option.
bool SpotifyPlayerBackend::createPlaylist(const QString &name, QString *error) {
    return command({"playlist", "new", "--", name}, nullptr, error);
}

Item SpotifyPlayerBackend::track(const QString &id, QString *error) {
    QJsonValue v;
    if (!json({"get", "item", "--id", id, "track"}, &v, error, longTimeoutMs()))
        return {};
    return parseTrack(v.toObject());
}

QString SpotifyPlayerBackend::account(QString *error) {
    // spotify_player's CLI has no "who am I". The account owns most of its
    // own playlists, so the commonest owner (Spotify's own aside) is it.
    const QList<Item> playlists = library("playlists", error);
    QHash<QString, int> owners;
    for (const Item &p : playlists)
        if (!p.owner.isEmpty() && p.owner != "Spotify")
            ++owners[p.owner];
    QString best;
    int most = 0;
    for (auto it = owners.cbegin(); it != owners.cend(); ++it)
        if (it.value() > most || (it.value() == most && it.key() < best))
            best = it.key(), most = it.value();
    return best;
}

// --- Spotifast ------------------------------------------------------------------

SpotifastBackend::SpotifastBackend(const QString &program)
    : m_program(findProgram(program, "OMAMP_SPOTIFAST", "spotifast")) {}

bool SpotifastBackend::running() const {
    if (m_program.isEmpty())
        return false;
    static const QStringList verbs{"play-pause", "play",   "pause",   "next",       "previous",   "seek",
                                   "seek-to",    "volume", "volume-up", "volume-down", "mute",    "shuffle",
                                   "repeat",     "like",   "play-uri", "play-search", "devices",  "transfer",
                                   "now-playing", "show",  "reload-themes", "help"};
    for (const Proc &p : processes()) {
        QStringList rest;
        if (!programArguments(p.exe, p.arguments, m_program, &rest))
            continue;
        bool verb = false;
        for (const QString &a : rest)
            verb = verb || verbs.contains(a) || a == "--help" || a == "-h" || a == "--version" || a == "-V";
        if (!verb)
            return true;
    }
    return false;
}

bool SpotifastBackend::start(QString *error) {
    if (m_program.isEmpty()) {
        if (error)
            *error = tr("Spotifast isn't installed.");
        return false;
    }
    if (running())
        return true;
    // Spotifast is a window; it opens when asked for.
    if (!QProcess::startDetached(m_program, {})) {
        if (error)
            *error = tr("could not start Spotifast");
        return false;
    }
    return true;
}

Playback SpotifastBackend::parseNowPlaying(const QByteArray &line, bool *saved) {
    Playback p;
    const QStringList f = QString::fromUtf8(line).trimmed().split('\t');
    if (f.size() < 6 || f.at(1).isEmpty())
        return p;
    p.valid = true;
    p.playing = f.at(0).compare("playing", Qt::CaseInsensitive) == 0;
    p.track.kind = "track";
    p.track.name = f.at(1);
    p.track.artists = f.at(2).split(", ", Qt::SkipEmptyParts);
    p.track.album = f.at(3);
    p.progressMs = f.at(4).toLongLong();
    p.track.durationMs = f.at(5).toLongLong();
    p.volume = f.size() > 6 && !f.at(6).isEmpty() ? f.at(6).toInt() : -1;
    p.track.imageUrl = f.value(9);
    if (saved)
        *saved = f.value(10) == "yes";
    p.deviceName = f.value(11);
    return p;
}

Playback SpotifastBackend::playback(QString *error) {
    QByteArray out;
    if (!runProgram(m_program, {"now-playing", "--raw"}, &out, error))
        return {};
    return parseNowPlaying(out);
}

bool SpotifastBackend::playTrack(const QString &id, QString *error) {
    return runProgram(m_program, {"play-uri", "spotify:track:" + id}, nullptr, error);
}

bool SpotifastBackend::playItem(const Item &item, QString *error) {
    return runProgram(m_program, {"play-uri", item.uri()}, nullptr, error);
}

bool SpotifastBackend::resume(QString *error) { return runProgram(m_program, {"play"}, nullptr, error); }
bool SpotifastBackend::pause(QString *error) { return runProgram(m_program, {"pause"}, nullptr, error); }

bool SpotifastBackend::seekTo(qint64 ms, qint64, QString *error) {
    return runProgram(m_program, {"seek-to", QString::number(ms / 1000.0, 'f', 1)}, nullptr, error);
}

bool SpotifastBackend::like(bool on, QString *error) {
    // `like` toggles, so look first.
    QByteArray out;
    if (!runProgram(m_program, {"now-playing", "--raw"}, &out, error))
        return false;
    bool saved = false;
    if (!parseNowPlaying(out, &saved).valid) {
        if (error)
            *error = tr("Nothing is playing in Spotifast.");
        return false;
    }
    if (saved == on)
        return true;
    return runProgram(m_program, {"like"}, nullptr, error);
}

QList<Device> SpotifastBackend::devices(QString *error) {
    QByteArray out;
    if (!runProgram(m_program, {"devices", "--raw"}, &out, error))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(out);
    QJsonArray a = doc.isArray() ? doc.array() : doc.object().value("devices").toArray();
    return parseDevices(a);
}

bool SpotifastBackend::connectDevice(const QString &id, QString *error) {
    return runProgram(m_program, {"transfer", id}, nullptr, error);
}

} // namespace Spotify
