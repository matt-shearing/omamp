#include "devicesync.h"

#include "job.h"
#include "library.h"
#include "mask.h"
#include "playlists.h"
#include "query.h"
#include "scanner.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QXmlStreamReader>

#include <atomic>
#include <thread>

namespace DeviceSync {

const char *const defaultMask = "<Album Artist>/<Album>/$If(<Disc#>,<Disc#>-)<Track#:2> <Title>";
const char *const manifestName = ".thrum-sync.json";
// The manifest's name before the rename (Thrum was Omamp until 0.4.0): read
// when a device has no new one, and removed once the new one is written.
const char *const legacyManifestName = ".omamp-sync.json";

namespace {

const char *const settingsKey = "sync.profiles";

QString g_ffmpeg = "ffmpeg";
std::function<bool(const QString &, QString *)> g_mountCheck;
QStringList g_mediaRoots, g_syncthingConfigs;
bool g_rootsSet = false;

const QStringList losslessFormats = {"flac", "wav", "aiff", "aif", "alac", "ape", "wv", "dsf", "dff", "tta"};

QString lower(const QString &s) { return s.trimmed().toLower(); }

// The codec, encoder and container options ffmpeg needs for each target.
struct Codec {
    QString encoder;
    int defaultBitrate;
    bool lossless;
    QStringList extra;
    bool cover; // keep the embedded picture
};

Codec codecFor(const QString &codec) {
    if (codec == "opus")
        return {"libopus", 128, false, {"-vbr", "on"}, false};
    if (codec == "mp3")
        return {"libmp3lame", 256, false, {"-id3v2_version", "3"}, true};
    if (codec == "aac")
        return {"aac", 256, false, {"-movflags", "+faststart"}, true};
    if (codec == "vorbis")
        return {"libvorbis", 192, false, {}, false};
    if (codec == "flac")
        return {"flac", 0, true, {}, true};
    return {};
}

int effectiveBitrate(const FormatRule &rule) {
    const Codec c = codecFor(rule.to);
    return rule.bitrate > 0 ? rule.bitrate : c.defaultBitrate;
}

bool ruleMatches(const FormatRule &rule, const QString &format, const QString &extension) {
    for (const QString &f : rule.from) {
        if (f == "*" || f == format || f == extension)
            return true;
        if (f == "lossless" && (losslessFormats.contains(format) || losslessFormats.contains(extension)))
            return true;
    }
    return false;
}

QStringList splitList(const QString &text) {
    QStringList out;
    for (const QString &part : text.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts))
        out << lower(part);
    return out;
}

// --- tracks -----------------------------------------------------------------

struct TrackRow {
    qint64 id = 0;
    QString path, format, availability, volumeKind;
    bool online = true;
    qint64 size = 0, mtimeNs = 0, durationMs = 0;
};

QHash<qint64, TrackRow> loadTracks(Library &library, const QList<qint64> &ids) {
    QHash<qint64, TrackRow> rows;
    for (qsizetype from = 0; from < ids.size(); from += 500) {
        QStringList chunk;
        for (qsizetype k = from; k < std::min<qsizetype>(from + 500, ids.size()); ++k)
            chunk << QString::number(ids[k]);
        QSqlQuery q(library.db());
        // The ids are integers from the database, so they go in as text.
        if (!q.exec("SELECT t.id, v.root_path, t.rel_path, lower(coalesce(t.format, '')), t.availability, v.online, "
                    "t.size, t.mtime_ns, coalesce(t.duration_ms, 0), v.kind FROM tracks t "
                    "JOIN volumes v ON v.id = t.volume_id WHERE t.id IN (" + chunk.join(',') + ")"))
            continue;
        while (q.next()) {
            TrackRow r;
            r.id = q.value(0).toLongLong();
            r.path = QDir(q.value(1).toString()).filePath(q.value(2).toString());
            r.format = q.value(3).toString();
            r.availability = q.value(4).toString();
            r.online = q.value(5).toInt() != 0;
            r.size = q.value(6).toLongLong();
            r.mtimeNs = q.value(7).toLongLong();
            r.durationMs = q.value(8).toLongLong();
            r.volumeKind = q.value(9).toString();
            rows.insert(r.id, r);
        }
    }
    return rows;
}

// The tracks a source names, in order.
QList<qint64> sourceIds(Library &library, const Source &s, QString *playlistName, QString *error) {
    QString why;
    if (s.kind == "playlist") {
        const qint64 id = Playlists::resolve(library, s.ref, &why);
        if (!id) {
            *error = why;
            return {};
        }
        Playlists::Playlist p;
        Playlists::get(library, id, &p);
        if (p.kind == "folder") {
            // A folder syncs every playlist inside it.
            QList<qint64> all;
            for (const Playlists::Playlist &child : Playlists::children(library, id))
                all += Playlists::trackIds(library, child.id, &why);
            return all;
        }
        *playlistName = p.name;
        QList<qint64> ids = Playlists::trackIds(library, id, &why);
        if (!why.isEmpty())
            *error = why;
        return ids;
    }
    if (s.kind == "collection") {
        const qint64 id = Collections::resolve(library, s.ref, &why);
        if (!id) {
            *error = why;
            return {};
        }
        QList<qint64> ids = Collections::trackIds(library, id, {}, &why);
        if (!why.isEmpty())
            *error = why;
        return ids;
    }
    if (s.kind == "query" || s.kind == "library") {
        QList<qint64> ids = Query::ids(library, s.kind == "library" ? QString() : s.ref, {}, &why);
        if (!why.isEmpty())
            *error = why;
        return ids;
    }
    *error = QString("Unknown source kind %1").arg(s.kind);
    return {};
}

// --- the manifest -----------------------------------------------------------

struct ManifestEntry {
    QString source;
    qint64 size = 0, mtimeNs = 0;
    QString rule;
};

struct Manifest {
    QHash<QString, ManifestEntry> files; // by relative target path
    QStringList playlists;
};

Manifest readManifest(const QString &target) {
    Manifest m;
    QFile f(QDir(target).filePath(manifestName));
    if (!f.open(QIODevice::ReadOnly)) {
        f.setFileName(QDir(target).filePath(legacyManifestName));
        if (!f.open(QIODevice::ReadOnly))
            return m;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject files = o.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject e = it.value().toObject();
        // Nanosecond times don't fit a JSON number (a double) exactly, so
        // they are written as text.
        auto number = [&](const char *key) {
            const QJsonValue v = e.value(key);
            return v.isString() ? v.toString().toLongLong() : qint64(v.toDouble());
        };
        m.files.insert(it.key(), {e.value("source").toString(), number("size"), number("mtime_ns"), e.value("rule").toString()});
    }
    for (const QJsonValue &v : o.value("playlists").toArray())
        m.playlists << v.toString();
    return m;
}

bool writeManifest(const QString &target, const QString &profile, const Manifest &m, QString *error) {
    QJsonObject files;
    for (auto it = m.files.begin(); it != m.files.end(); ++it)
        files.insert(it.key(), QJsonObject{{"source", it->source},
                                           {"size", QString::number(it->size)},
                                           {"mtime_ns", QString::number(it->mtimeNs)},
                                           {"rule", it->rule}});
    const QJsonObject o{{"thrum", 1},
                        {"profile", profile},
                        {"written", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                        {"files", files},
                        {"playlists", QJsonArray::fromStringList(m.playlists)}};
    QSaveFile f(QDir(target).filePath(manifestName));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QFile::remove(QDir(target).filePath(legacyManifestName));
    return true;
}

bool hiddenPart(const QString &relative) {
    for (const QString &part : relative.split('/'))
        if (part.startsWith('.'))
            return true;
    return false;
}

QString existingAncestor(QString path) {
    path = QDir::cleanPath(path);
    while (!path.isEmpty() && path != "/" && !QFileInfo::exists(path))
        path = QFileInfo(path).absolutePath();
    return path.isEmpty() ? QString("/") : path;
}

// A drive is "mounted" when the folder sits on a filesystem other than the
// system's root and the home folder's: a USB stick under /run/media.
bool defaultMountCheck(const QString &path, QString *mountPoint) {
    const QStorageInfo here(existingAncestor(path));
    if (!here.isValid())
        return false;
    const QString root = here.rootPath();
    if (mountPoint)
        *mountPoint = root;
    if (root == "/" || root == QStorageInfo(QDir::homePath()).rootPath())
        return false;
    return true;
}

QString safeName(const QString &name) {
    Mask::Sanitise s;
    return Mask::sanitiseComponent(name, s);
}

// Writes an .m3u8 of the files beside it, relative to the target.
bool writePlaylistFile(const QString &file, const QStringList &relativePaths, QString *error) {
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    QByteArray body = "#EXTM3U\n";
    for (const QString &p : relativePaths)
        body += p.toUtf8() + '\n';
    f.write(body);
    if (!f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace

// --- sources and rules ------------------------------------------------------

QString Source::toString() const { return kind == "library" ? kind : kind + ':' + ref; }

Source Source::fromString(const QString &text, QString *error) {
    Source s;
    const QString t = text.trimmed();
    if (t == "library" || t == "all") {
        s.kind = "library";
        return s;
    }
    const int colon = t.indexOf(':');
    const QString kind = lower(t.left(colon));
    if (colon <= 0 || !QStringList{"playlist", "autoplaylist", "collection", "query"}.contains(kind)) {
        if (error)
            *error = QString("A source is playlist:<name>, collection:<name>, query:<text> or library, not \"%1\"").arg(text);
        return {};
    }
    s.kind = kind == "autoplaylist" ? QString("playlist") : kind;
    s.ref = t.mid(colon + 1).trimmed();
    if (s.ref.isEmpty() && error)
        *error = QString("%1: needs a name or text after the colon").arg(kind);
    return s;
}

QString FormatRule::toString() const {
    QString right = action == "convert" ? to + (bitrate > 0 ? QString(":%1").arg(bitrate) : QString()) : action;
    return from.join(',') + "->" + right;
}

FormatRule FormatRule::fromString(const QString &text, QString *error) {
    FormatRule r;
    const int arrow = text.indexOf("->");
    if (arrow <= 0) {
        if (error)
            *error = QString("A format rule looks like flac,wav->opus:128 or wma->skip, not \"%1\"").arg(text);
        return {};
    }
    r.from = splitList(text.left(arrow));
    const QString right = lower(text.mid(arrow + 2));
    const QString what = right.section(':', 0, 0);
    if (what == "copy" || what == "skip") {
        r.action = what;
    } else {
        r.action = "convert";
        r.to = what == "ogg" ? QString("vorbis") : what == "m4a" ? QString("aac") : what;
        bool ok = true;
        if (right.contains(':'))
            r.bitrate = right.section(':', 1).remove('k').toInt(&ok);
        if (codecFor(r.to).encoder.isEmpty()) {
            if (error)
                *error = QString("Thrum converts to opus, mp3, aac, vorbis or flac, not \"%1\"").arg(what);
            return {};
        }
        if (!ok || r.bitrate < 0 || r.bitrate > 512) {
            if (error)
                *error = QString("The bitrate in \"%1\" should be kbit/s, like opus:128").arg(text);
            return {};
        }
    }
    if (r.from.isEmpty() && error)
        *error = QString("\"%1\" names no formats before ->").arg(text);
    return r;
}

QString FormatRule::signature() const {
    if (action != "convert")
        return action;
    const int b = effectiveBitrate(*this);
    return b > 0 ? QString("%1:%2").arg(to).arg(b) : to;
}

QString extensionFor(const QString &codec) {
    if (codec == "aac")
        return "m4a";
    if (codec == "vorbis")
        return "ogg";
    return codec;
}

// --- profiles ---------------------------------------------------------------

QJsonObject Profile::toJson() const {
    QJsonArray s, r;
    for (const Source &x : sources)
        s << x.toString();
    for (const FormatRule &x : rules)
        r << x.toString();
    return {{"id", id},
            {"name", name},
            {"kind", kind},
            {"target", target},
            {"sources", s},
            {"mask", mask},
            {"rules", r},
            {"delete_others", deleteOthers},
            {"write_playlists", writePlaylists},
            {"last_sync", lastSync},
            {"last_result", lastResult}};
}

Profile Profile::fromJson(const QJsonObject &o) {
    Profile p;
    p.id = o.value("id").toInteger();
    p.name = o.value("name").toString();
    p.kind = o.value("kind").toString("folder");
    p.target = o.value("target").toString();
    for (const QJsonValue &v : o.value("sources").toArray())
        p.sources << Source::fromString(v.toString());
    p.mask = o.value("mask").toString();
    for (const QJsonValue &v : o.value("rules").toArray())
        p.rules << FormatRule::fromString(v.toString());
    p.deleteOthers = o.value("delete_others").toBool();
    p.writePlaylists = o.value("write_playlists").toBool(true);
    p.lastSync = o.value("last_sync").toInteger();
    p.lastResult = o.value("last_result").toObject();
    return p;
}

namespace {

QList<Profile> readAll(Library &library) {
    QList<Profile> out;
    const QJsonArray all = QJsonDocument::fromJson(library.setting(settingsKey).toString().toUtf8()).array();
    for (const QJsonValue &v : all)
        out << Profile::fromJson(v.toObject());
    return out;
}

void writeAll(Library &library, const QList<Profile> &profiles) {
    QJsonArray all;
    for (const Profile &p : profiles)
        all << p.toJson();
    library.setSetting(settingsKey, QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact)));
}

} // namespace

QList<Profile> list(Library &library) { return readAll(library); }

Profile find(Library &library, const QString &idOrName, QString *error) {
    const QList<Profile> all = readAll(library);
    bool isNumber = false;
    const qint64 id = idOrName.toLongLong(&isNumber);
    for (const Profile &p : all)
        if ((isNumber && p.id == id) || p.name.compare(idOrName.trimmed(), Qt::CaseInsensitive) == 0)
            return p;
    if (error)
        *error = all.isEmpty() ? QString("There are no sync profiles yet; make one with: thrum sync create")
                               : QString("No sync profile called \"%1\" (see thrum sync list)").arg(idOrName);
    return {};
}

qint64 save(Library &library, Profile profile, QString *error) {
    auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return qint64(0);
    };
    profile.name = profile.name.trimmed();
    if (profile.name.isEmpty())
        return refuse("A sync profile needs a name");
    if (!QStringList{"folder", "usb", "syncthing"}.contains(profile.kind))
        return refuse(QString("A target is a folder, usb or syncthing, not \"%1\"").arg(profile.kind));
    if (profile.target.trimmed().isEmpty())
        return refuse("A sync profile needs a target folder");
    profile.target = QDir::cleanPath(QDir(profile.target.trimmed()).absolutePath());
    if (profile.target == "/" || profile.target == QDir::cleanPath(QDir::homePath()))
        return refuse("Pick a folder of its own to sync into, not " + profile.target);
    if (profile.sources.isEmpty())
        return refuse("A sync profile needs at least one playlist, collection or query to sync");
    for (const Source &s : profile.sources)
        if (s.kind.isEmpty() || (s.kind != "library" && s.ref.isEmpty()))
            return refuse("One of the sources is empty");
    for (const FormatRule &r : profile.rules)
        if (r.from.isEmpty() || r.action.isEmpty() || (r.action == "convert" && r.to.isEmpty()))
            return refuse("One of the format rules is incomplete");
    if (!profile.mask.isEmpty()) {
        const Mask::Parsed m = Mask::parse(profile.mask);
        if (!m.ok())
            return refuse("The file name mask: " + m.error);
    }
    QList<Profile> all = readAll(library);
    qint64 next = 1;
    for (const Profile &p : all) {
        next = std::max(next, p.id + 1);
        if (p.id != profile.id && p.name.compare(profile.name, Qt::CaseInsensitive) == 0)
            return refuse(QString("There is already a sync profile called \"%1\"").arg(p.name));
    }
    if (profile.id == 0) {
        profile.id = next;
        all << profile;
    } else {
        bool found = false;
        for (Profile &p : all)
            if (p.id == profile.id) {
                profile.lastSync = p.lastSync;
                profile.lastResult = p.lastResult;
                p = profile;
                found = true;
            }
        if (!found)
            return refuse(QString("No sync profile %1").arg(profile.id));
    }
    writeAll(library, all);
    return profile.id;
}

bool remove(Library &library, qint64 id, QString *error) {
    QList<Profile> all = readAll(library);
    const qsizetype before = all.size();
    all.removeIf([id](const Profile &p) { return p.id == id; });
    if (all.size() == before) {
        if (error)
            *error = QString("No sync profile %1").arg(id);
        return false;
    }
    writeAll(library, all);
    return true;
}

void recordRun(Library &library, qint64 id, const QJsonObject &result) {
    QList<Profile> all = readAll(library);
    for (Profile &p : all)
        if (p.id == id) {
            p.lastSync = QDateTime::currentSecsSinceEpoch();
            p.lastResult = result;
        }
    writeAll(library, all);
}

// --- targets and devices ----------------------------------------------------

QJsonObject TargetState::toJson() const {
    return {{"ready", ready},
            {"state", state},
            {"message", message},
            {"free_bytes", freeBytes},
            {"total_bytes", totalBytes},
            {"mount_point", mountPoint}};
}

TargetState checkTarget(Library &library, const Profile &profile) {
    TargetState t;
    const QString target = profile.target;
    const QStorageInfo storage(existingAncestor(target));
    if (storage.isValid()) {
        t.freeBytes = storage.bytesAvailable();
        t.totalBytes = storage.bytesTotal();
        t.mountPoint = storage.rootPath();
    }
    // Tests make a device look nearly full.
    if (qEnvironmentVariableIsSet("THRUM_SYNC_FREE_BYTES"))
        t.freeBytes = qEnvironmentVariable("THRUM_SYNC_FREE_BYTES").toLongLong();
    for (const QString &root : Scanner::roots(library)) {
        if (root.isEmpty())
            continue;
        const QString r = QDir::cleanPath(root);
        if (target == r || target.startsWith(r + '/') || r.startsWith(target + '/')) {
            t.state = "inside-library";
            t.message = QString("%1 overlaps the library folder %2; the copies would be scanned as new tracks").arg(target, r);
            return t;
        }
    }
    if (profile.kind == "usb") {
        QString mount;
        const bool mounted = g_mountCheck ? g_mountCheck(target, &mount) : defaultMountCheck(target, &mount);
        if (!mounted) {
            t.state = "not-connected";
            t.message = QString("The drive for %1 isn't plugged in (or isn't mounted)").arg(target);
            return t;
        }
        if (!mount.isEmpty())
            t.mountPoint = mount;
    }
    if (profile.kind == "syncthing" && !QFileInfo(QDir(target).filePath(".stfolder")).exists()) {
        t.state = "not-syncthing";
        t.message = QString("%1 isn't a Syncthing folder (it has no .stfolder); add it in Syncthing first").arg(target);
        return t;
    }
    if (!QFileInfo::exists(target)) {
        if (profile.kind != "folder") {
            t.state = "not-connected";
            t.message = QString("%1 doesn't exist").arg(target);
            return t;
        }
        t.state = "missing";
        t.message = QString("%1 will be made").arg(target);
        t.ready = true;
        return t;
    }
    if (!QFileInfo(target).isDir() || !QFileInfo(target).isWritable()) {
        t.state = "read-only";
        t.message = QString("%1 can't be written to").arg(target);
        return t;
    }
    t.state = "ready";
    t.ready = true;
    return t;
}

QJsonObject Device::toJson() const {
    return {{"kind", kind}, {"path", path}, {"label", label}, {"free_bytes", freeBytes}, {"total_bytes", totalBytes}};
}

void setDeviceRoots(const QStringList &mediaRoots, const QStringList &syncthingConfigs) {
    g_mediaRoots = mediaRoots;
    g_syncthingConfigs = syncthingConfigs;
    g_rootsSet = true;
}

QList<Device> detectDevices() {
    QStringList media = g_mediaRoots, configs = g_syncthingConfigs;
    // THRUM_MEDIA_ROOTS (colon-separated) stands in for /run/media/$USER in
    // screenshots and tests.
    const QString mediaEnv = qEnvironmentVariable("THRUM_MEDIA_ROOTS");
    if (!g_rootsSet) {
        const QString user = qEnvironmentVariable("USER");
        media = {"/run/media/" + user, "/media/" + user, "/media"};
        const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state");
        const QString config = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
        configs = {state + "/syncthing/config.xml", config + "/syncthing/config.xml"};
        if (!mediaEnv.isEmpty())
            media = mediaEnv.split(':', Qt::SkipEmptyParts);
    }
    const bool anyFolder = g_rootsSet || !mediaEnv.isEmpty();
    QList<Device> out;
    QSet<QString> seen;
    for (const QString &root : media) {
        const QFileInfoList drives = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &d : drives) {
            const QString path = d.absoluteFilePath();
            if (seen.contains(path) || (!anyFolder && QStorageInfo(path).rootPath() != path))
                continue;
            seen.insert(path);
            const QStorageInfo s(path);
            out << Device{"usb", path, d.fileName(), s.bytesAvailable(), s.bytesTotal()};
        }
    }
    for (const QString &file : configs) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QXmlStreamReader xml(&f);
        while (!xml.atEnd()) {
            if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() != QLatin1String("folder"))
                continue;
            QString path = xml.attributes().value("path").toString();
            if (path.startsWith("~/"))
                path = QDir::homePath() + path.mid(1);
            path = QDir::cleanPath(path);
            if (path.isEmpty() || seen.contains(path))
                continue;
            seen.insert(path);
            const QString label = xml.attributes().value("label").toString();
            const QStorageInfo s(existingAncestor(path));
            out << Device{"syncthing", path, label.isEmpty() ? QFileInfo(path).fileName() : label, s.bytesAvailable(),
                          s.bytesTotal()};
        }
        break; // the first config found is Syncthing's
    }
    return out;
}

void setFfmpegProgram(const QString &program) { g_ffmpeg = program.isEmpty() ? QString("ffmpeg") : program; }
QString ffmpegProgram() { return g_ffmpeg; }
void setMountCheck(std::function<bool(const QString &, QString *)> check) { g_mountCheck = std::move(check); }

// --- the plan ---------------------------------------------------------------

QJsonObject Action::toJson() const {
    QJsonObject o{{"op", op}, {"target", target}, {"reason", reason}, {"bytes", bytes}};
    if (trackId)
        o["track_id"] = trackId;
    if (!source.isEmpty())
        o["source"] = source;
    if (!rule.isEmpty())
        o["rule"] = rule;
    return o;
}

QJsonObject Plan::toJson(int limit, bool withKept) const {
    QJsonArray list;
    int shown = 0;
    for (const Action &a : actions) {
        if (a.op == "keep" && !withKept)
            continue;
        if (limit > 0 && shown >= limit)
            break;
        list << a.toJson();
        ++shown;
    }
    return {{"ok", ok()},
            {"error", error},
            {"profile", profile.name},
            {"target", profile.target},
            {"target_state", target.toJson()},
            {"copy", copies},
            {"convert", conversions},
            {"keep", kept},
            {"delete", deletions},
            {"skip", skipped},
            {"bytes_to_write", bytesToWrite},
            {"bytes_to_free", bytesToFree},
            {"fits", fits},
            {"warnings", QJsonArray::fromStringList(warnings)},
            {"playlists", QJsonArray::fromStringList(playlists)},
            {"actions", list}};
}

QList<qint64> trackIds(Library &library, const Profile &profile, QString *error) {
    QList<qint64> order;
    QSet<qint64> seen;
    for (const Source &s : profile.sources) {
        QString name, why;
        const QList<qint64> ids = sourceIds(library, s, &name, &why);
        if (!why.isEmpty() && error)
            *error = s.toString() + ": " + why;
        for (qint64 id : ids)
            if (!seen.contains(id)) {
                seen.insert(id);
                order << id;
            }
    }
    return order;
}

Plan plan(Library &library, const Profile &profile, Job *job) {
    Plan plan;
    plan.profile = profile;
    plan.target = checkTarget(library, profile);
    if (!plan.target.ready) {
        plan.error = plan.target.message;
        return plan;
    }
    if (job)
        job->setProgress("Reading the list", 0, 0);

    // The tracks, in the order the sources give them, each once.
    QList<qint64> order;
    QSet<qint64> seenIds;
    QList<QPair<QString, QList<qint64>>> playlistSources;
    for (const Source &s : profile.sources) {
        QString name, why;
        const QList<qint64> ids = sourceIds(library, s, &name, &why);
        if (!why.isEmpty()) {
            plan.error = s.toString() + ": " + why;
            return plan;
        }
        if (!name.isEmpty())
            playlistSources.append({name, ids});
        for (qint64 id : ids)
            if (!seenIds.contains(id)) {
                seenIds.insert(id);
                order << id;
            }
    }
    const QHash<qint64, TrackRow> rows = loadTracks(library, order);
    const Manifest manifest = readManifest(profile.target);

    const Mask::Parsed mask = Mask::parse(profile.mask.isEmpty() ? QString(defaultMask) : profile.mask);
    if (!mask.ok()) {
        plan.error = "The file name mask: " + mask.error;
        return plan;
    }
    Mask::Context mc;
    mc.sanitise.maxPathChars = 0;

    QHash<qint64, QString> targetOf; // for the playlist files
    QSet<QString> planned;           // relative paths, lower-cased (FAT is case-insensitive)
    const FormatRule copyRule{{"*"}, "copy", {}, 0};
    int n = 0;
    for (qint64 id : order) {
        if (job && job->isCancelled()) {
            plan.error = "Cancelled";
            return plan;
        }
        if (job && ++n % 200 == 0)
            job->setProgress("Comparing with the device", n, order.size());
        const TrackRow r = rows.value(id);
        Action a;
        a.trackId = id;
        a.source = r.path;
        if (!r.id) {
            continue;
        }
        if (r.volumeKind == "subsonic") {
            a.op = "skip";
            a.reason = "on a server";
            plan.actions << a;
            ++plan.skipped;
            continue;
        }
        if (!r.online || r.availability != "online") {
            a.op = "skip";
            a.reason = r.availability == "missing" ? QString("missing") : QString("offline");
            plan.actions << a;
            ++plan.skipped;
            continue;
        }
        const QString extension = QFileInfo(r.path).suffix().toLower();
        FormatRule rule = copyRule;
        for (const FormatRule &candidate : profile.rules)
            if (ruleMatches(candidate, r.format, extension)) {
                rule = candidate;
                break;
            }
        // Converting to the format a file is already in only loses quality.
        if (rule.action == "convert" && extensionFor(rule.to) == extension && rule.to != "flac")
            rule = copyRule;
        a.rule = rule.signature();
        if (rule.action == "skip") {
            a.op = "skip";
            a.reason = "rule";
            plan.actions << a;
            ++plan.skipped;
            continue;
        }

        QString relative = Mask::evaluate(mask, Mask::fromTrack(library, id), mc);
        if (relative.isEmpty())
            relative = QFileInfo(r.path).completeBaseName();
        relative += '.' + (rule.action == "convert" ? extensionFor(rule.to) : extension);
        // Two tracks that would land on one name get (2), (3)…
        if (planned.contains(relative.toLower())) {
            const QString stem = relative.left(relative.lastIndexOf('.'));
            const QString ext = relative.mid(relative.lastIndexOf('.'));
            int k = 2;
            while (planned.contains(QString("%1 (%2)%3").arg(stem).arg(k).arg(ext).toLower()))
                ++k;
            relative = QString("%1 (%2)%3").arg(stem).arg(k).arg(ext);
        }
        planned.insert(relative.toLower());
        a.target = relative;
        targetOf.insert(id, relative);

        const QFileInfo existing(QDir(profile.target).filePath(relative));
        const ManifestEntry m = manifest.files.value(relative);
        const bool same = existing.exists() && !m.source.isEmpty() && m.source == r.path && m.size == r.size &&
                          m.mtimeNs == r.mtimeNs && m.rule == a.rule;
        // A copy made before the manifest (or by hand) counts when its size
        // matches and it is no older than the source.
        const bool sameCopy = existing.exists() && m.source.isEmpty() && rule.action == "copy" &&
                              existing.size() == r.size &&
                              existing.lastModified().toMSecsSinceEpoch() * 1000000 >= r.mtimeNs;
        if (same || sameCopy) {
            a.op = "keep";
            a.reason = "unchanged";
            a.bytes = existing.size();
            plan.actions << a;
            ++plan.kept;
            continue;
        }
        a.op = rule.action == "convert" ? QString("convert") : QString("copy");
        a.reason = existing.exists() ? QString("changed") : QString("new");
        if (rule.action == "convert") {
            const int kbps = effectiveBitrate(rule);
            // FLAC: about 60% of the PCM it holds, so the source size will do.
            a.bytes = kbps > 0 ? r.durationMs * kbps / 8 : r.size;
            ++plan.conversions;
        } else {
            a.bytes = r.size;
            ++plan.copies;
        }
        plan.bytesToWrite += std::max<qint64>(0, a.bytes - (existing.exists() ? existing.size() : 0));
        plan.actions << a;
    }

    // Playlist files.
    QSet<QString> playlistFiles;
    if (profile.writePlaylists) {
        for (const auto &p : std::as_const(playlistSources)) {
            const QString file = safeName(p.first) + ".m3u8";
            if (!playlistFiles.contains(file.toLower())) {
                playlistFiles.insert(file.toLower());
                plan.playlists << file;
            }
        }
    }

    // What is on the device and no longer wanted.
    if (job)
        job->setProgress("Looking at the device", 0, 0);
    const QDir root(profile.target);
    const QString base = root.absolutePath() + '/';
    QDirIterator it(profile.target, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = path.mid(base.size());
        if (hiddenPart(relative) || planned.contains(relative.toLower()) || playlistFiles.contains(relative.toLower()))
            continue;
        const bool ours = manifest.files.contains(relative) || manifest.playlists.contains(relative);
        if (!ours && !profile.deleteOthers)
            continue;
        if (QFileInfo(path).isSymLink())
            continue;
        Action a;
        a.op = "delete";
        a.target = relative;
        a.bytes = QFileInfo(path).size();
        a.reason = ours ? QString("no longer in the list") : QString("not from this sync");
        plan.bytesToFree += a.bytes;
        plan.actions << a;
        ++plan.deletions;
    }

    const qint64 need = plan.bytesToWrite - plan.bytesToFree;
    plan.fits = plan.target.freeBytes < 0 || need <= plan.target.freeBytes;
    if (!plan.fits)
        plan.warnings << QString("This needs %1 MB more than the %2 MB free on the device")
                             .arg((need - plan.target.freeBytes) / 1048576 + 1)
                             .arg(plan.target.freeBytes / 1048576);
    if (plan.skipped)
        plan.warnings << QString("%1 tracks are left off (offline, missing, on a server, or skipped by a rule)").arg(plan.skipped);
    if (plan.conversions && QStandardPaths::findExecutable(g_ffmpeg).isEmpty() && !QFileInfo(g_ffmpeg).isExecutable()) {
        plan.error = "Converting needs the ffmpeg command, which isn't installed (sudo pacman -S ffmpeg)";
        return plan;
    }

    for (const auto &p : std::as_const(playlistSources)) {
        QStringList &paths = plan.playlistEntries[safeName(p.first) + ".m3u8"];
        for (qint64 id : p.second)
            if (targetOf.contains(id))
                paths << targetOf.value(id);
    }
    if (job)
        job->setProgress("Ready", order.size(), order.size());
    return plan;
}

// --- applying ---------------------------------------------------------------

QJsonObject Result::toJson() const {
    return {{"ok", ok},
            {"cancelled", cancelled},
            {"error", error},
            {"copied", copied},
            {"converted", converted},
            {"kept", kept},
            {"deleted", deleted},
            {"skipped", skipped},
            {"failed", failed},
            {"failures", QJsonArray::fromStringList(failures.mid(0, 50))},
            {"playlists", QJsonArray::fromStringList(playlists)},
            {"bytes_written", bytesWritten},
            {"elapsed_ms", elapsedMs}};
}

namespace {

// Runs ffmpeg, cancelling it when the job is cancelled.
bool runFfmpeg(const QStringList &args, Job *job, QString *error) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(g_ffmpeg, args);
    if (!p.waitForStarted(10000)) {
        *error = "ffmpeg didn't start: " + p.errorString();
        return false;
    }
    while (!p.waitForFinished(200)) {
        if (p.state() == QProcess::NotRunning)
            break;
        if (job && job->isCancelled()) {
            p.kill();
            p.waitForFinished(2000);
            *error = "cancelled";
            return false;
        }
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const QString output = QString::fromUtf8(p.readAll()).trimmed();
        *error = "ffmpeg: " + (output.isEmpty() ? QString("exit code %1").arg(p.exitCode()) : output.section('\n', -1));
        return false;
    }
    return true;
}

bool convertFile(const QString &source, const QString &target, const QString &signature, Job *job, QString *error) {
    const QString codecName = signature.section(':', 0, 0);
    const int bitrate = signature.section(':', 1).toInt();
    const Codec codec = codecFor(codecName);
    const QString temp = QFileInfo(target).absolutePath() + "/.thrum-sync-" +
                         QString::number(quintptr(QThread::currentThreadId()), 16) + '.' + extensionFor(codecName);
    auto args = [&](bool cover) {
        QStringList a = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-map", "0:a:0"};
        if (cover)
            a << "-map" << "0:v?" << "-c:v" << "copy" << "-disposition:v" << "attached_pic";
        a << "-map_metadata" << "0" << "-c:a" << codec.encoder;
        if (!codec.lossless && bitrate > 0)
            a << "-b:a" << QString("%1k").arg(bitrate);
        a << codec.extra << temp;
        return a;
    };
    bool ok = runFfmpeg(args(codec.cover), job, error);
    // Some pictures won't go into some containers; the music matters more.
    if (!ok && codec.cover && *error != "cancelled")
        ok = runFfmpeg(args(false), job, error);
    if (!ok) {
        QFile::remove(temp);
        return false;
    }
    QFile::remove(target);
    if (!QFile::rename(temp, target)) {
        *error = "couldn't move the converted file into place";
        QFile::remove(temp);
        return false;
    }
    return true;
}

bool copyFile(const QString &source, const QString &target, QString *error) {
    const QString temp = QFileInfo(target).absolutePath() + "/.thrum-sync-" +
                         QString::number(quintptr(QThread::currentThreadId()), 16);
    QFile::remove(temp);
    QFile in(source);
    if (!in.copy(temp)) {
        *error = in.errorString();
        return false;
    }
    QFile::remove(target);
    if (!QFile::rename(temp, target)) {
        *error = "couldn't move the copy into place";
        QFile::remove(temp);
        return false;
    }
    return true;
}

void removeEmptyFolders(const QString &root, QString folder) {
    const QString top = QDir::cleanPath(root);
    folder = QDir::cleanPath(folder);
    while (folder.startsWith(top + '/') && QDir(folder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!QDir().rmdir(folder))
            break;
        folder = QFileInfo(folder).absolutePath();
    }
}

} // namespace

Result apply(Library &library, const Plan &plan, Job *job, int workers) {
    Result result;
    QElapsedTimer timer;
    timer.start();
    if (!plan.ok()) {
        result.error = plan.error;
        return result;
    }
    if (!plan.fits) {
        result.error = plan.warnings.value(0, "It doesn't fit on the device");
        return result;
    }
    const QString target = plan.profile.target;
    if (!QDir().mkpath(target)) {
        result.error = "Couldn't make " + target;
        return result;
    }

    Manifest manifest = readManifest(target);
    QMutex lock;
    QList<const Action *> work;
    for (const Action &a : plan.actions) {
        if (a.op == "keep") {
            ++result.kept;
        } else if (a.op == "skip") {
            ++result.skipped;
        } else if (a.op == "copy" || a.op == "convert") {
            work << &a;
        }
    }

    // Deletes first, so the space is there for what comes.
    for (const Action &a : plan.actions) {
        if (a.op != "delete")
            continue;
        if (job && job->isCancelled())
            break;
        const QString path = QDir(target).filePath(a.target);
        if (QFile::remove(path) || !QFileInfo::exists(path)) {
            ++result.deleted;
            manifest.files.remove(a.target);
            manifest.playlists.removeAll(a.target);
            removeEmptyFolders(target, QFileInfo(path).absolutePath());
        } else {
            ++result.failed;
            result.failures << a.target + ": couldn't delete it";
        }
    }

    // The manifest records the library's size and time for each source (not
    // the file's own), so the next plan compares like with like. Read on
    // this thread: the workers never touch the database.
    QList<qint64> ids;
    for (const Action *a : std::as_const(work))
        ids << a->trackId;
    const QHash<qint64, TrackRow> sources = loadTracks(library, ids);
    std::atomic<int> next = 0, done = 0;
    const int total = int(work.size());
    if (job)
        job->setProgress("Syncing", 0, total);
    auto worker = [&]() {
        for (;;) {
            const int k = next++;
            if (k >= total || (job && job->isCancelled()))
                return;
            const Action &a = *work[k];
            const QString to = QDir(target).filePath(a.target);
            QDir().mkpath(QFileInfo(to).absolutePath());
            const QFileInfo from(a.source);
            const qint64 size = from.size();
            const qint64 mtimeNs = from.lastModified().toMSecsSinceEpoch() * 1000000;
            QString why;
            const bool ok = a.op == "convert" ? convertFile(a.source, to, a.rule, job, &why) : copyFile(a.source, to, &why);
            QMutexLocker hold(&lock);
            if (ok) {
                (a.op == "convert" ? result.converted : result.copied)++;
                result.bytesWritten += QFileInfo(to).size();
                // The library's size and time, so the next plan compares like with like.
                const TrackRow r = sources.value(a.trackId);
                manifest.files.insert(a.target, {a.source, r.id ? r.size : size, r.id ? r.mtimeNs : mtimeNs, a.rule});
            } else if (why != "cancelled") {
                ++result.failed;
                result.failures << a.target + ": " + why;
            }
            const int d = ++done;
            if (job)
                job->setProgress(a.op == "convert" ? "Converting" : "Copying", d, total);
        }
    };
    int threads = workers > 0 ? workers : std::clamp(QThread::idealThreadCount() / 2, 1, 4);
    threads = std::min(threads, std::max(total, 1));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(worker);
    for (std::thread &t : pool)
        t.join();

    result.cancelled = job && job->isCancelled();
    if (!result.cancelled) {
        for (const QString &file : plan.playlists) {
            QStringList paths;
            for (const QString &p : plan.playlistEntries.value(file)) {
                if (manifest.files.contains(p) && QFileInfo::exists(QDir(target).filePath(p)))
                    paths << p;
            }
            QString why;
            if (writePlaylistFile(QDir(target).filePath(file), paths, &why)) {
                result.playlists << file;
                if (!manifest.playlists.contains(file))
                    manifest.playlists << file;
            } else {
                ++result.failed;
                result.failures << file + ": " + why;
            }
        }
    }
    QString why;
    if (!writeManifest(target, plan.profile.name, manifest, &why)) {
        result.failures << QString(manifestName) + ": " + why;
        ++result.failed;
    }
    result.ok = !result.cancelled && result.failed == 0;
    if (result.cancelled)
        result.error = "Cancelled";
    else if (result.failed)
        result.error = QString("%1 files failed").arg(result.failed);
    result.elapsedMs = timer.elapsed();
    if (job)
        job->setProgress("Done", total, total);
    return result;
}

}
