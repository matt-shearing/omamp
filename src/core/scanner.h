#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "volumes.h"

class Job;
class Library;
struct TrackTags;

struct ScanResult {
    bool ok = true;
    QString error;           // why the whole scan failed (a root that doesn't exist)
    QStringList roots;       // canonical library roots (or folders) scanned
    qint64 files = 0;        // audio files found on disk
    qint64 unchanged = 0;    // skipped by size and modification time, never opened
    qint64 added = 0, updated = 0;
    qint64 moved = 0;        // new files recognised as missing tracks that moved; their rows kept
    qint64 missing = 0;      // newly marked missing (their volume is online)
    qint64 failed = 0;       // files whose tags could not be read (still added, untagged)
    qint64 skippedMounts = 0; // other filesystems mounted inside a root, not crossed
    qint64 foldersListed = 0; // folders whose files were looked at
    qint64 foldersSkipped = 0; // folders not listed because their modification time had not changed
    QStringList unreadable;  // folders that could not be listed; nothing under them is marked missing
    QStringList failures;    // paths of files whose tags failed
    QStringList movedPaths;  // "old -> new" for each moved track (the first 100)
    qint64 elapsedMs = 0;
    bool cancelled = false;

    // True when the scan wrote anything the window should reload.
    bool changedLibrary() const { return added || updated || moved || missing; }
    QJsonObject toJson() const;
};

// One folder for an incremental rescan, as the folder watcher reports them.
// recursive covers everything below it (a folder that appeared, vanished or
// moved); otherwise only the files directly inside are looked at.
struct FolderScan {
    QString path;
    bool recursive = false;
};

struct LibraryRoot {
    QString path;       // absolute, on the volume's current mount; empty when offline
    QString kind;       // "local" or "network"
    QString fsType;
    qint64 volumeId = 0;
    bool online = false;
};

struct LocateResult {
    bool ok = true;
    QString error;
    qint64 searched = 0;      // audio files under the folder that are not in the library already
    qint64 missingBefore = 0; // missing tracks before the search
    qint64 missingAfter = 0;
    QStringList located;      // "old -> new" for each track found
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};

// Fills the library from folders on disk.
//
// 1. Walk each root and stat every audio file. Files whose size and
//    modification time (to the nanosecond) match the database are skipped
//    without being opened, which makes a rescan of an unchanged library cost
//    one stat per file. In ChangedFolders mode (for network shares, where
//    nothing watches for changes) a folder whose own modification time is
//    unchanged isn't even listed: a rescan then costs one stat per folder.
// 2. Mark files that have gone as missing. Only files under a folder that was
//    actually listed are touched, and a root that isn't there fails the scan
//    instead, so an unplugged drive never turns a library into dead links.
// 3. Read new and changed files with TagLib on a pool of 4 to 8 threads, fed
//    a few thousand at a time so memory doesn't grow with the library.
// 4. Write the results in batches, one transaction per batch. A new file
//    that matches a missing track (same size, length and tags, or the same
//    bytes) takes over that track's row, so its rating, play count, history
//    and playlists follow it: MediaMonkey's "Locate Moved/Missing Files",
//    done on every scan.
//
// The scanner runs on the Library's thread and blocks it; the window runs it
// on a worker thread with its own Library connection. It never crosses into
// another filesystem mounted inside a root: add that as its own root.
class Scanner {
public:
    enum class Mode {
        Full,           // list every folder, stat every audio file
        ChangedFolders, // skip folders whose modification time matches the database
    };

    Scanner(Library &library, const VolumeResolver &resolver, Job *job = nullptr);

    void setWorkers(int count) { m_workers = count; }
    static int defaultWorkers();
    // How many files may be waiting to be read or written at once. Bounds
    // the scan's memory; tests set it small to exercise the feeding.
    void setInFlightLimit(int count) { m_inFlight = qMax(1, count); }

    // Adds each folder as a library root (when it isn't one yet) and scans it.
    ScanResult scan(const QStringList &roots, Mode mode = Mode::Full);
    // Rescans every library root whose volume is online.
    ScanResult rescanAll(Mode mode = Mode::Full);
    // Rescans only these folders, which must be inside library roots. A
    // folder that no longer exists has its tracks marked missing (its library
    // root must still be there, or nothing is touched).
    ScanResult rescanFolders(const QList<FolderScan> &folders);

    // Searches a folder (inside the library or not) for files that are
    // missing tracks, by size, length, tags and content, and points those
    // tracks at them. Nothing else is added.
    LocateResult locate(const QString &folder);

    // The library roots, as absolute paths on their current mounts (empty
    // root_path when offline).
    static QStringList roots(Library &library);
    static QList<LibraryRoot> libraryRoots(Library &library);

    // Rewrites one track's row from tags just read from its file, exactly as
    // a scan would (artists, genres, album, search index), keeping its id,
    // rating, play count and history. For the tag writer, which has just
    // written the file and must not wait for a rescan. Runs inside the
    // caller's transaction, if any.
    static bool updateTrackRow(Library &library, qint64 trackId, const TrackTags &tags, qint64 size,
                               qint64 mtimeNs, QString *error = nullptr);

private:
    struct Scope;
    struct Run;
    bool walkScope(const Scope &scope, Mode mode, Run &run, ScanResult &result);
    bool readAndWrite(Run &run, ScanResult &result);
    qint64 startJob(const char *kind, const QStringList &args);
    void finishJob(qint64 jobId, const ScanResult &result);

    Library &m_library;
    VolumeResolver m_resolver; // a copy: it is two paths, and a temporary must not dangle
    Job *m_job;
    int m_workers;
    int m_inFlight = 2000;
};
