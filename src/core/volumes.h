#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

class Library;
struct ProbeResult;

// One line of /proc/self/mountinfo.
struct Mount {
    QString mountPoint; // where it is mounted, e.g. /mnt/nas
    QString root;       // which directory of the filesystem is mounted there (btrfs subvolumes, bind mounts)
    QString fsType;     // ext4, btrfs, cifs, nfs4, fuse.sshfs, ...
    QString source;     // /dev/nvme0n1p2, //nas/music, nas:/export/music
    bool isNetwork() const;
};

// Which volume a path lives on, and where that volume is mounted now.
struct VolumeInfo {
    QString key;       // stable identity: uuid:..., marker:..., net:..., mount:...
    QString kind;      // "local" or "network"
    QString rootPath;  // absolute path of the volume root on this machine, right now
    QString mountSubpath; // rootPath relative to its mount point ("" when they are the same)
    QString fsType, source, label;
    bool markerWritten = false;
    bool isValid() const { return !key.isEmpty(); }
};

// Gives every library root a volume identity that survives a remount at a
// different path or a new machine, so track paths can be stored relative to
// the volume and a missing drive reads as offline rather than deleted.
//
// - A local filesystem is known by its UUID (from /dev/disk/by-uuid). A btrfs
//   subvolume or bind mount adds the mounted directory, since one UUID can be
//   mounted several times with different roots.
// - A network share is known by a marker file, .thrum-volume-id, at the
//   library root. It is written only when the user adds that root, and only
//   if the share is writable. Otherwise the server and share name stand in.
// - Anything else (tmpfs, overlay) is known by its mount, which is the best
//   that can be done for a filesystem without a UUID.
//
// The mountinfo file and the by-uuid folder are injectable so tests can fake
// any mount layout.
class VolumeResolver {
public:
    explicit VolumeResolver(const QString &mountInfoPath = "/proc/self/mountinfo",
                            const QString &byUuidDir = "/dev/disk/by-uuid");

    static constexpr const char *markerName = ".thrum-volume-id";

    QList<Mount> mounts() const;
    // The mount holding an absolute, canonical path (the longest mount point
    // that contains it; later mounts shadow earlier ones).
    Mount mountFor(const QString &path) const;
    QString uuidForSource(const QString &source) const;

    // Identifies the volume holding path. adding is true when the user is
    // adding path as a library root, the only time a marker may be written.
    VolumeInfo identify(const QString &path, bool adding, QString *error = nullptr) const;

    // Where the volume with this key is mounted now, or empty when offline.
    // lastRoot and mountSubpath come from the volumes table. Network
    // volumes are looked at through PathProbe, so a hung share makes this
    // wait at most one probe timeout, and an ESTALE or EIO share reads as
    // offline. why, when given, says what the last look found.
    QString locate(const QString &key, const QString &lastRoot, const QString &mountSubpath,
                   ProbeResult *why = nullptr) const;

    QString mountInfoPath() const { return m_mountInfoPath; }
    // The filesystem types counted as network shares (Mount::isNetwork).
    static const QStringList &networkTypes();

    static QString unescape(const QString &field);

private:
    static QString readMarker(const QString &dir);
    static QString writeMarker(const QString &dir);

    QString m_mountInfoPath, m_byUuidDir;
};

// One row of the volumes table, for `thrum volume` and the window's volume panel.
struct VolumeRow {
    qint64 id = 0;
    QString key, kind, label, fsType, source, rootPath, mountSubpath;
    bool online = false;
    qint64 lastSeen = 0, added = 0;
    int scanWorkers = 0;      // 0: the default for its kind
    qint64 tracks = 0, offline = 0, missing = 0, roots = 0;
    QStringList libraryRoots; // relative to the volume root
    qint64 remoteSourceId = 0;
    int effectiveWorkers() const;
    QJsonObject toJson() const;
};

// Looking after volumes by hand: MediaMonkey's "re-assign drive", and
// forgetting a volume that is gone for good.
namespace Volumes {

// Folder walkers per volume when nothing is set: a share is limited by round
// trips, so more requests in flight help; a local disk is limited by the
// disk, and more threads only add seeking.
constexpr int defaultNetworkWorkers = 8;
constexpr int defaultLocalWorkers = 4;

QList<VolumeRow> list(Library &library);
// By id ("3"), key ("marker:..."), label, or a path on its current mount.
VolumeRow find(Library &library, const QString &which, QString *error);

// Points a volume at a new root: the same music, now somewhere the volume's
// key no longer finds (a share whose server was renamed, a disk copied to
// a new one, a share with no marker that is now mounted from another
// address). newRoot must be the folder that corresponds to the volume's
// root, so every track's rel_path still holds; at least one of the
// volume's library folders must exist under it. The volume takes the new
// root's identity (writing the old marker id onto a writable share that has
// none, so the identity carries over), and its tracks come back online.
// Refuses when another volume already has that identity and holds tracks.
bool reassign(Library &library, const VolumeResolver &resolver, qint64 volumeId, const QString &newRoot,
              QString *error, QJsonObject *report = nullptr);

// Removes a volume, its folders and its tracks from the library. Playlist
// entries keep their saved title and path; play history rows lose their
// track. Refused while the volume is online unless force is set, since an
// online volume is better rescanned.
bool forget(Library &library, qint64 volumeId, bool force, QString *error, qint64 *tracksRemoved = nullptr);

// 0 restores the default.
bool setScanWorkers(Library &library, qint64 volumeId, int workers, QString *error);

}
