#include "volumes.h"

#include "library.h"
#include "mounts.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

const QStringList &VolumeResolver::networkTypes() {
    // fuse.sshfs and fuse.rclone are how sshfs and rclone mount; davfs2
    // mounts as plain "davfs" through mount.davfs, or as fuse.davfs2.
    static const QStringList network = {"cifs", "smb3", "smbfs", "nfs", "nfs4", "9p", "afs", "ceph",
                                        "glusterfs", "davfs", "fuse.sshfs", "fuse.rclone", "fuse.davfs2",
                                        "fuse.glusterfs", "fuse.s3fs", "fuse.smbnetfs"};
    return network;
}

bool Mount::isNetwork() const { return VolumeResolver::networkTypes().contains(fsType); }

VolumeResolver::VolumeResolver(const QString &mountInfoPath, const QString &byUuidDir)
    : m_mountInfoPath(mountInfoPath), m_byUuidDir(byUuidDir) {}

// mountinfo escapes space, tab, newline and backslash as \ooo octal.
QString VolumeResolver::unescape(const QString &field) {
    if (!field.contains('\\'))
        return field;
    QByteArray in = field.toUtf8(), out;
    for (int i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size()) {
            bool ok = false;
            const int code = in.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out += char(code);
                i += 3;
                continue;
            }
        }
        out += in[i];
    }
    return QString::fromUtf8(out);
}

QList<Mount> VolumeResolver::mounts() const {
    QList<Mount> result;
    QFile file(m_mountInfoPath);
    if (!file.open(QIODevice::ReadOnly))
        return result;
    // Read whole: /proc files report size 0, so readLine on a stream is safest.
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        // id parent major:minor root mountpoint options [optional...] - fstype source superoptions
        const QList<QByteArray> fields = line.split(' ');
        const int dash = fields.indexOf("-");
        if (fields.size() < 10 || dash < 6 || dash + 2 >= fields.size())
            continue;
        Mount m;
        m.root = unescape(QString::fromUtf8(fields[3]));
        m.mountPoint = unescape(QString::fromUtf8(fields[4]));
        m.fsType = QString::fromUtf8(fields[dash + 1]);
        m.source = unescape(QString::fromUtf8(fields[dash + 2]));
        result << m;
    }
    return result;
}

static bool contains(const QString &mountPoint, const QString &path) {
    if (mountPoint == "/")
        return path.startsWith('/');
    return path == mountPoint || path.startsWith(mountPoint + '/');
}

Mount VolumeResolver::mountFor(const QString &path) const {
    Mount best;
    for (const Mount &m : mounts()) {
        // >= so a later mount on the same point (which hides the earlier one) wins.
        if (contains(m.mountPoint, path) && m.mountPoint.size() >= best.mountPoint.size())
            best = m;
    }
    return best;
}

QString VolumeResolver::uuidForSource(const QString &source) const {
    if (!source.startsWith('/'))
        return {};
    const QString device = QFileInfo(source).canonicalFilePath();
    if (device.isEmpty())
        return {};
    const QFileInfoList links = QDir(m_byUuidDir).entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &link : links) {
        if (link.canonicalFilePath() == device)
            return link.fileName();
    }
    return {};
}

QString VolumeResolver::readMarker(const QString &dir) {
    QFile file(dir + '/' + markerName);
    if (!file.open(QIODevice::ReadOnly)) {
        file.setFileName(dir + '/' + legacyMarkerName);
        if (!file.open(QIODevice::ReadOnly))
            return {};
    }
    const QString id = QString::fromUtf8(file.readLine(200)).trimmed();
    return QUuid::fromString(id).isNull() ? QString() : id;
}

QString VolumeResolver::writeMarker(const QString &dir) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile file(dir + '/' + markerName);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(id.toUtf8() + "\n# Thrum uses this file to recognise this music share wherever it is mounted.\n");
    return file.commit() ? id : QString();
}

// Never a null string: Qt SQL binds a null QString as NULL.
static QString relativeTo(const QString &base, const QString &path) {
    if (path == base)
        return QString("");
    return base == "/" ? path.mid(1) : path.mid(base.size() + 1);
}

VolumeInfo VolumeResolver::identify(const QString &path, bool adding, QString *error) const {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) {
        if (error)
            *error = QString("%1 does not exist").arg(path);
        return {};
    }
    const Mount mount = mountFor(canonical);
    if (mount.mountPoint.isEmpty()) {
        if (error)
            *error = QString("No mount found for %1").arg(canonical);
        return {};
    }

    VolumeInfo info;
    info.fsType = mount.fsType;
    info.source = mount.source;
    info.rootPath = mount.mountPoint;

    if (mount.isNetwork()) {
        info.kind = "network";
        // An existing marker anywhere from the path up to the mount point wins,
        // so adding a subfolder of a known share reuses the share's volume.
        for (QString dir = canonical; contains(mount.mountPoint, dir);) {
            const QString id = readMarker(dir);
            if (!id.isEmpty()) {
                info.key = "marker:" + id;
                info.rootPath = dir;
                break;
            }
            if (dir == mount.mountPoint)
                break;
            dir = QFileInfo(dir).path();
        }
        if (info.key.isEmpty() && adding && QFileInfo(canonical).isDir()) {
            const QString id = writeMarker(canonical);
            if (!id.isEmpty()) {
                info.key = "marker:" + id;
                info.rootPath = canonical;
                info.markerWritten = true;
            }
        }
        // A read-only share: fall back to the server and share name.
        if (info.key.isEmpty())
            info.key = "net:" + mount.source + (mount.root == "/" ? QString() : mount.root);
        info.label = mount.source;
    } else {
        info.kind = "local";
        const QString uuid = uuidForSource(mount.source);
        if (!uuid.isEmpty()) {
            info.key = "uuid:" + uuid + (mount.root == "/" ? QString() : ':' + mount.root);
            info.label = uuid;
        } else {
            info.key = QString("mount:%1:%2:%3@%4").arg(mount.fsType, mount.source, mount.root, mount.mountPoint);
            info.label = mount.mountPoint;
        }
    }
    info.mountSubpath = relativeTo(mount.mountPoint, info.rootPath);
    return info;
}

QString VolumeResolver::locate(const QString &key, const QString &lastRoot, const QString &mountSubpath,
                              ProbeResult *why) const {
    const QList<Mount> all = mounts();
    auto join = [](const QString &a, const QString &b) {
        return b.isEmpty() ? a : (a == "/" ? '/' + b : a + '/' + b);
    };
    ProbeResult last;
    auto report = [&](const QString &found) {
        if (why) {
            *why = last;
            if (!found.isEmpty())
                why->state = ProbeState::Online;
        }
        return found;
    };
    auto mountAt = [&](const QString &path) {
        Mount best;
        for (const Mount &m : all) {
            const bool inside = m.mountPoint == "/" ? path.startsWith('/')
                                                    : (path == m.mountPoint || path.startsWith(m.mountPoint + '/'));
            if (inside && m.mountPoint.size() >= best.mountPoint.size())
                best = m;
        }
        return best;
    };

    if (key.startsWith("marker:")) {
        const QString id = key.mid(7);
        // Each look happens on a probe thread: a share that has stopped
        // answering must not hang the caller.
        auto markerAt = [&](const QString &dir) {
            const ProbeResult r = PathProbe::probe(dir, -1, true);
            if (!r.online())
                last = r;
            return r.online() ? r.marker : QString();
        };
        // The last place counts only while a share is still mounted there: an
        // unmounted mount point can hold a stale copy of the folder.
        if (!lastRoot.isEmpty() && mountAt(lastRoot).isNetwork() && markerAt(lastRoot) == id)
            return report(lastRoot);
        // The share may be mounted somewhere else now: try the same folder
        // under every network mount.
        for (const Mount &m : all) {
            if (!m.isNetwork())
                continue;
            const QString candidate = join(m.mountPoint, mountSubpath);
            if (candidate == lastRoot)
                continue;
            if (markerAt(candidate) == id)
                return report(candidate);
        }
        return report({});
    }

    if (key.startsWith("uuid:")) {
        const QString rest = key.mid(5);
        const int colon = rest.indexOf(':');
        const QString uuid = colon < 0 ? rest : rest.left(colon);
        const QString root = colon < 0 ? "/" : rest.mid(colon + 1);
        QString found;
        for (const Mount &m : all) {
            if (m.root == root && !m.isNetwork() && uuidForSource(m.source) == uuid) {
                found = join(m.mountPoint, mountSubpath);
                // Prefer the mount we used last time when there are several.
                if (found == lastRoot)
                    break;
            }
        }
        return report(found);
    }

    if (key.startsWith("net:")) {
        for (const Mount &m : all) {
            if (m.isNetwork() && "net:" + m.source + (m.root == "/" ? QString() : m.root) == key) {
                const QString candidate = join(m.mountPoint, mountSubpath);
                last = PathProbe::probe(candidate);
                if (last.online())
                    return report(candidate);
            }
        }
        return report({});
    }

    if (key.startsWith("mount:")) {
        for (const Mount &m : all) {
            if (QString("mount:%1:%2:%3@%4").arg(m.fsType, m.source, m.root, m.mountPoint) == key)
                return report(join(m.mountPoint, mountSubpath));
        }
    }
    return report({});
}

// --- Volumes ------------------------------------------------------------

int VolumeRow::effectiveWorkers() const {
    if (scanWorkers > 0)
        return scanWorkers;
    return kind == "network" ? Volumes::defaultNetworkWorkers : Volumes::defaultLocalWorkers;
}

QJsonObject VolumeRow::toJson() const {
    QJsonObject o{{"id", id},
                  {"key", key},
                  {"kind", kind},
                  {"label", label},
                  {"fs_type", fsType},
                  {"source", source},
                  {"root_path", rootPath},
                  {"mount_subpath", mountSubpath},
                  {"online", online},
                  {"last_seen", lastSeen},
                  {"added", added},
                  {"scan_workers", effectiveWorkers()},
                  {"scan_workers_set", scanWorkers > 0},
                  {"tracks", tracks},
                  {"offline", offline},
                  {"missing", missing},
                  {"library_folders", QJsonArray::fromStringList(libraryRoots)}};
    if (remoteSourceId)
        o["remote_source_id"] = remoteSourceId;
    return o;
}

namespace Volumes {

static const char *selectRows =
    "SELECT v.id, v.key, v.kind, coalesce(v.label, ''), coalesce(v.fs_type, ''), coalesce(v.source, ''), v.root_path, "
    "v.mount_subpath, v.online, coalesce(v.last_seen, 0), v.added, coalesce(v.scan_workers, 0), "
    "coalesce(v.remote_source_id, 0), "
    "(SELECT count(*) FROM tracks t WHERE t.volume_id = v.id), "
    "(SELECT count(*) FROM tracks t WHERE t.volume_id = v.id AND t.availability = 'offline'), "
    "(SELECT count(*) FROM tracks t WHERE t.volume_id = v.id AND t.availability = 'missing') "
    "FROM volumes v ";

static VolumeRow rowFrom(QSqlQuery &q, Library &library) {
    VolumeRow r;
    r.id = q.value(0).toLongLong();
    r.key = q.value(1).toString();
    r.kind = q.value(2).toString();
    r.label = q.value(3).toString();
    r.fsType = q.value(4).toString();
    r.source = q.value(5).toString();
    r.rootPath = q.value(6).toString();
    r.mountSubpath = q.value(7).toString();
    r.online = q.value(8).toBool();
    r.lastSeen = q.value(9).toLongLong();
    r.added = q.value(10).toLongLong();
    r.scanWorkers = q.value(11).toInt();
    r.remoteSourceId = q.value(12).toLongLong();
    r.tracks = q.value(13).toLongLong();
    r.offline = q.value(14).toLongLong();
    r.missing = q.value(15).toLongLong();
    QSqlQuery f(library.db());
    f.prepare("SELECT rel_path FROM folders WHERE volume_id = ? AND is_root ORDER BY rel_path");
    f.addBindValue(r.id);
    f.exec();
    while (f.next())
        r.libraryRoots << f.value(0).toString();
    r.roots = r.libraryRoots.size();
    return r;
}

QList<VolumeRow> list(Library &library) {
    QList<VolumeRow> out;
    QSqlQuery q(library.db());
    q.exec(QString(selectRows) + "ORDER BY v.id");
    while (q.next())
        out << rowFrom(q, library);
    return out;
}

VolumeRow find(Library &library, const QString &which, QString *error) {
    const QList<VolumeRow> all = list(library);
    bool isNumber = false;
    const qint64 id = which.toLongLong(&isNumber);
    for (const VolumeRow &r : all)
        if ((isNumber && r.id == id) || r.key == which)
            return r;
    QList<VolumeRow> byLabel;
    for (const VolumeRow &r : all)
        if (!r.label.isEmpty() && r.label == which)
            byLabel << r;
    if (byLabel.size() == 1)
        return byLabel.first();
    // A path: the volume whose root holds it (the deepest one).
    const QString path = QDir::cleanPath(which);
    VolumeRow best;
    for (const VolumeRow &r : all) {
        if (r.kind == "subsonic")
            continue;
        if ((path == r.rootPath || path.startsWith(r.rootPath == "/" ? "/" : r.rootPath + '/')) &&
            r.rootPath.size() > best.rootPath.size())
            best = r;
    }
    if (best.id)
        return best;
    if (error)
        *error = byLabel.size() > 1 ? QString("Several volumes are labelled %1; use the id").arg(which)
                                    : QString("No volume matches %1 (see: thrum volume list)").arg(which);
    return {};
}

static QString joinPath(const QString &a, const QString &b) {
    return b.isEmpty() ? a : (a == "/" ? '/' + b : a + '/' + b);
}

bool reassign(Library &library, const VolumeResolver &resolver, qint64 volumeId, const QString &newRoot,
              QString *error, QJsonObject *report) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QString why;
    const VolumeRow old = find(library, QString::number(volumeId), &why);
    if (!old.id)
        return fail(why);
    if (old.kind == "subsonic")
        return fail("A Subsonic source is changed with: thrum remote add (a new address), not volume reassign");

    const ProbeResult probe = PathProbe::probe(newRoot);
    if (!probe.online() || !probe.isDir)
        return fail(QString("%1 is not a folder that can be read: %2").arg(newRoot, probe.describe()));
    const QString root = QFileInfo(newRoot).canonicalFilePath();

    // The library folders must be where they should be, or the tracks'
    // relative paths would point at nothing.
    QStringList found;
    for (const QString &rel : old.libraryRoots)
        if (QFileInfo(joinPath(root, rel)).isDir())
            found << rel;
    if (!old.libraryRoots.isEmpty() && found.isEmpty())
        return fail(QString("None of this volume's library folders (%1) are under %2. Give the folder that matches "
                            "the volume's old root, %3.")
                        .arg(old.libraryRoots.join(", "), root, old.rootPath));
    // And some of its tracks: a library folder that is the volume root
    // exists under any folder at all.
    {
        QSqlQuery sample(library.db());
        sample.prepare("SELECT rel_path FROM tracks WHERE volume_id = ? AND availability != 'missing' ORDER BY id LIMIT 50");
        sample.addBindValue(old.id);
        sample.exec();
        QString example;
        bool any = false, some = false;
        while (sample.next() && !some) {
            any = true;
            const QString rel = sample.value(0).toString();
            if (example.isEmpty())
                example = rel;
            some = QFileInfo::exists(joinPath(root, rel));
        }
        if (any && !some)
            return fail(QString("None of this volume's tracks (such as %1) are under %2. Give the folder that matches "
                                "the volume's old root, %3.")
                            .arg(example, root, old.rootPath));
    }

    const Mount mount = resolver.mountFor(root);
    if (mount.mountPoint.isEmpty())
        return fail(QString("No mount found for %1").arg(root));

    VolumeInfo info;
    bool wroteMarker = false;
    if (mount.isNetwork() && old.key.startsWith("marker:")) {
        // Carry the old identity over: write the old marker id onto the new
        // root when it has none, so the next lookup finds it by marker.
        const QString id = old.key.mid(7);
        QString existing;
        for (const char *name : {VolumeResolver::markerName, VolumeResolver::legacyMarkerName}) {
            QFile marker(root + '/' + name);
            if (existing.isEmpty() && marker.open(QIODevice::ReadOnly))
                existing = QString::fromUtf8(marker.readLine(200)).trimmed();
        }
        if (existing.isEmpty()) {
            QSaveFile out(root + '/' + VolumeResolver::markerName);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(id.toUtf8() + "\n# Thrum uses this file to recognise this music share wherever it is mounted.\n");
                wroteMarker = out.commit();
            }
            existing = wroteMarker ? id : QString();
        }
        if (!existing.isEmpty()) {
            info.key = "marker:" + existing;
            info.kind = "network";
            info.rootPath = root;
            info.fsType = mount.fsType;
            info.source = mount.source;
            info.label = mount.source;
            info.mountSubpath = root == mount.mountPoint ? QString("") : root.mid(mount.mountPoint == "/" ? 1 : mount.mountPoint.size() + 1);
        }
    }
    if (info.key.isEmpty()) {
        info = resolver.identify(root, true, &why);
        if (!info.isValid())
            return fail(why);
        wroteMarker = info.markerWritten;
        // identify() may settle on a marker higher up; the volume root must
        // be the folder given, so relative paths keep their meaning.
        if (info.rootPath != root) {
            info.rootPath = root;
            info.mountSubpath = root == mount.mountPoint ? QString("") : root.mid(mount.mountPoint == "/" ? 1 : mount.mountPoint.size() + 1);
        }
    }

    QSqlDatabase db = library.db();
    // Another volume with the new identity: one with no tracks (an empty
    // scan of the new place) can go; one with tracks would need merging.
    QSqlQuery other(db);
    other.prepare("SELECT v.id, (SELECT count(*) FROM tracks t WHERE t.volume_id = v.id) FROM volumes v "
                  "WHERE v.key = ? AND v.id != ?");
    other.addBindValue(info.key);
    other.addBindValue(old.id);
    other.exec();
    qint64 dropId = 0;
    if (other.next()) {
        if (other.value(1).toLongLong() > 0)
            return fail(QString("Volume %1 already has that identity and holds %2 tracks. Forget one of them first "
                                "(thrum volume forget %1 --force), then reassign.")
                            .arg(other.value(0).toLongLong())
                            .arg(other.value(1).toLongLong()));
        dropId = other.value(0).toLongLong();
    }
    other.finish();

    db.transaction();
    if (dropId) {
        QSqlQuery d(db);
        d.prepare("DELETE FROM volumes WHERE id = ?");
        d.addBindValue(dropId);
        if (!d.exec()) {
            db.rollback();
            return fail(d.lastError().text());
        }
    }
    QSqlQuery u(db);
    u.prepare("UPDATE volumes SET key = ?, kind = ?, label = ?, fs_type = ?, source = ?, mount_subpath = ?, "
              "root_path = ?, online = 1, last_seen = ? WHERE id = ?");
    for (const QVariant &value : {QVariant(info.key), QVariant(info.kind), QVariant(info.label), QVariant(info.fsType),
                                  QVariant(info.source), QVariant(info.mountSubpath), QVariant(info.rootPath),
                                  QVariant(QDateTime::currentSecsSinceEpoch()), QVariant(old.id)})
        u.addBindValue(value);
    if (!u.exec()) {
        db.rollback();
        return fail(u.lastError().text());
    }
    QSqlQuery t(db);
    t.prepare("UPDATE tracks SET availability = 'online' WHERE volume_id = ? AND availability = 'offline'");
    t.addBindValue(old.id);
    t.exec();
    if (!db.commit())
        return fail(db.lastError().text());

    if (report)
        *report = QJsonObject{{"id", old.id},
                              {"old_key", old.key},
                              {"new_key", info.key},
                              {"old_root", old.rootPath},
                              {"new_root", info.rootPath},
                              {"marker_written", wroteMarker},
                              {"library_folders_found", QJsonArray::fromStringList(found)},
                              {"replaced_empty_volume", dropId}};
    return true;
}

bool forget(Library &library, qint64 volumeId, bool force, QString *error, qint64 *tracksRemoved) {
    QString why;
    const VolumeRow row = find(library, QString::number(volumeId), &why);
    if (!row.id) {
        if (error)
            *error = why;
        return false;
    }
    if (row.online && !force) {
        if (error)
            *error = QString("Volume %1 (%2) is online; rescan it instead, or add --force to forget it anyway")
                         .arg(row.id)
                         .arg(row.label.isEmpty() ? row.rootPath : row.label);
        return false;
    }
    QSqlDatabase db = library.db();
    db.transaction();
    QSqlQuery q(db);
    auto exec = [&](const char *sql) {
        q.prepare(sql);
        q.addBindValue(row.id);
        return q.exec();
    };
    // tracks has no ON DELETE on its volume, so they go first; their
    // artists, genres and history follow by their own cascades.
    bool ok = exec("DELETE FROM tracks WHERE volume_id = ?");
    const qint64 removed = q.numRowsAffected();
    ok = ok && exec("UPDATE remote_sources SET volume_id = NULL WHERE volume_id = ?");
    ok = ok && exec("DELETE FROM volumes WHERE id = ?");
    if (!ok) {
        const QString message = q.lastError().text();
        db.rollback();
        if (error)
            *error = message;
        return false;
    }
    QSqlQuery tidy(db);
    tidy.exec("DELETE FROM albums WHERE id NOT IN (SELECT album_id FROM tracks WHERE album_id IS NOT NULL)");
    tidy.exec("DELETE FROM artists WHERE id NOT IN (SELECT artist_id FROM track_artists)");
    tidy.exec("DELETE FROM genres WHERE id NOT IN (SELECT genre_id FROM track_genres)");
    db.commit();
    if (tracksRemoved)
        *tracksRemoved = removed;
    return true;
}

bool setScanWorkers(Library &library, qint64 volumeId, int workers, QString *error) {
    if (workers < 0 || workers > 64) {
        if (error)
            *error = "Workers must be between 1 and 64 (0 for the default)";
        return false;
    }
    QSqlQuery q(library.db());
    q.prepare("UPDATE volumes SET scan_workers = ? WHERE id = ?");
    q.addBindValue(workers ? QVariant(workers) : QVariant(QMetaType(QMetaType::Int)));
    q.addBindValue(volumeId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (error)
            *error = q.lastError().text().isEmpty() ? QString("No volume %1").arg(volumeId) : q.lastError().text();
        return false;
    }
    return true;
}

}
