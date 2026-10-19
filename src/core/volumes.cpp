#include "volumes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

bool Mount::isNetwork() const {
    static const QStringList network = {"cifs", "smb3", "smbfs", "nfs", "nfs4", "9p", "afs", "ceph",
                                        "glusterfs", "davfs", "fuse.sshfs", "fuse.rclone", "fuse.davfs2",
                                        "fuse.glusterfs", "fuse.s3fs"};
    return network.contains(fsType);
}

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
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString id = QString::fromUtf8(file.readLine(200)).trimmed();
    return QUuid::fromString(id).isNull() ? QString() : id;
}

QString VolumeResolver::writeMarker(const QString &dir) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile file(dir + '/' + markerName);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(id.toUtf8() + "\n# Omamp uses this file to recognise this music share wherever it is mounted.\n");
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

QString VolumeResolver::locate(const QString &key, const QString &lastRoot, const QString &mountSubpath) const {
    const QList<Mount> all = mounts();
    auto join = [](const QString &a, const QString &b) {
        return b.isEmpty() ? a : (a == "/" ? '/' + b : a + '/' + b);
    };

    if (key.startsWith("marker:")) {
        const QString id = key.mid(7);
        // The last place counts only while a share is still mounted there: an
        // unmounted mount point can hold a stale copy of the folder.
        if (!lastRoot.isEmpty() && mountFor(lastRoot).isNetwork() && readMarker(lastRoot) == id)
            return lastRoot;
        // The share may be mounted somewhere else now: try the same folder
        // under every network mount.
        for (const Mount &m : all) {
            if (!m.isNetwork())
                continue;
            const QString candidate = join(m.mountPoint, mountSubpath);
            if (readMarker(candidate) == id)
                return candidate;
        }
        return {};
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
        return found;
    }

    if (key.startsWith("net:")) {
        for (const Mount &m : all) {
            if (m.isNetwork() && "net:" + m.source + (m.root == "/" ? QString() : m.root) == key)
                return join(m.mountPoint, mountSubpath);
        }
        return {};
    }

    if (key.startsWith("mount:")) {
        for (const Mount &m : all) {
            if (QString("mount:%1:%2:%3@%4").arg(m.fsType, m.source, m.root, m.mountPoint) == key)
                return join(m.mountPoint, mountSubpath);
        }
    }
    return {};
}
