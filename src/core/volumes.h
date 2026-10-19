#pragma once

#include <QList>
#include <QString>

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
// - A network share is known by a marker file, .omamp-volume-id, at the
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

    static constexpr const char *markerName = ".omamp-volume-id";

    QList<Mount> mounts() const;
    // The mount holding an absolute, canonical path (the longest mount point
    // that contains it; later mounts shadow earlier ones).
    Mount mountFor(const QString &path) const;
    QString uuidForSource(const QString &source) const;

    // Identifies the volume holding path. adding is true when the user is
    // adding path as a library root, the only time a marker may be written.
    VolumeInfo identify(const QString &path, bool adding, QString *error = nullptr) const;

    // Where the volume with this key is mounted now, or empty when offline.
    // lastRoot and mountSubpath come from the volumes table.
    QString locate(const QString &key, const QString &lastRoot, const QString &mountSubpath) const;

    static QString unescape(const QString &field);

private:
    static QString readMarker(const QString &dir);
    static QString writeMarker(const QString &dir);

    QString m_mountInfoPath, m_byUuidDir;
};
