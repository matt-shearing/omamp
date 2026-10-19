#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

class Job;

// What stat() says about one path, reduced to what the scanner uses.
struct FsStat {
    bool isDir = false, isRegular = false, isLink = false;
    quint64 device = 0;
    qint64 size = 0, mtimeNs = 0;
};

// One entry of a folder listing. stat is filled only for the entries the
// walker asked about; followed only for a symbolic link that was followed.
struct FsEntry {
    QByteArray name;
    unsigned char type = 0; // DT_DIR, DT_REG, DT_LNK, DT_UNKNOWN (from dirent.h)
    bool statted = false;
    int statError = 0;
    FsStat stat;
    bool followed = false;
    int followError = 0;
    FsStat target;
};

// The seam between the scanner and the filesystem. The real one is POSIX
// (opendir, fstat, fstatat); tests and the benchmark put a latency layer in
// front of it, or replace it with a tree held in memory, to measure what a
// rescan costs over a network without a network.
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // Lists a folder. self gets the folder's own stat. For each entry the
    // predicate accepts (by name and type), the entry is stat'ed without
    // following links; links the predicate accepted are then followed too.
    // Returns 0 or an errno (ENOENT for a folder that vanished).
    using Wanted = std::function<bool(const QByteArray &name, unsigned char type)>;
    virtual int list(const QByteArray &path, FsStat *self, QList<FsEntry> *entries, const Wanted &wanted) = 0;

    static std::shared_ptr<FileSystem> posix();
};

// Adds a fixed delay to every request, standing in for a network round trip:
// one per folder listed (the listing itself; on SMB2+ the folder's own
// attributes come back with it) and one per entry stat'ed. Counts both.
class LatencyFileSystem : public FileSystem {
public:
    LatencyFileSystem(std::shared_ptr<FileSystem> inner, int latencyUs);
    int list(const QByteArray &path, FsStat *self, QList<FsEntry> *entries, const Wanted &wanted) override;

    qint64 listings() const { return m_listings; }
    qint64 stats() const { return m_stats; }
    int latencyUs() const { return m_latencyUs; }

private:
    std::shared_ptr<FileSystem> m_inner;
    int m_latencyUs;
    std::atomic<qint64> m_listings{0}, m_stats{0};
};

// A folder tree held in memory, for measuring the walk over 100,000 files
// without making 100,000 files. Paths are absolute and use '/'.
class MemoryFileSystem : public FileSystem {
public:
    MemoryFileSystem();
    void addFolder(const QByteArray &path, qint64 mtimeNs = 1);
    void addFile(const QByteArray &path, qint64 size, qint64 mtimeNs);
    void touchFolder(const QByteArray &path, qint64 mtimeNs);
    int list(const QByteArray &path, FsStat *self, QList<FsEntry> *entries, const Wanted &wanted) override;

private:
    struct Node {
        bool dir = false;
        qint64 size = 0, mtimeNs = 1;
        QList<QByteArray> children; // names, in insertion order
    };
    Node &ensureFolder(const QByteArray &path);
    QHash<QByteArray, Node> m_nodes;
};

struct WalkedFile {
    QString rel;    // relative to the volume root
    QString folder; // its folder, relative to the volume root
    qint64 size = 0, mtimeNs = 0;
};

struct WalkOutput {
    QList<WalkedFile> files;        // sorted by rel, whatever order the workers found them in
    QHash<QString, qint64> folders; // listed folders holding audio: rel path -> mtime_ns
    QSet<QString> skipped;          // folders not listed, their modification time unchanged
    QStringList unreadable;         // rel paths that could not be listed
    QStringList vanished;           // rel paths that no longer exist
    qint64 skippedMounts = 0, listed = 0;
};

struct WalkOptions {
    std::shared_ptr<FileSystem> fs;               // null means POSIX
    int workers = 1;                              // folders listed at once
    const QHash<QString, qint64> *known = nullptr; // stored folder times, for ChangedFolders mode
    Job *job = nullptr;
    std::function<bool(const QString &fileName)> isAudio;
};

// Walks a folder tree the way the scanner needs: hidden entries skipped,
// only audio files stat'ed, linked files followed and linked folders not,
// other filesystems not crossed, and (with known) a folder whose
// modification time matches the database not listed for files, only for
// subfolders.
//
// With more than one worker, folders are listed in parallel: on a network
// share each listing and stat is a round trip, and keeping several in
// flight hides the latency. On a local disk a few help with a cold cache.
namespace Walker {

WalkOutput walk(const QByteArray &absolute, const QString &rel, quint64 rootDevice, bool recursive,
                const WalkOptions &options);

}
