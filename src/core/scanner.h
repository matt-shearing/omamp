#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "volumes.h"

class Job;
class Library;

struct ScanResult {
    bool ok = true;
    QString error;           // why the whole scan failed (a root that doesn't exist)
    QStringList roots;       // canonical library roots scanned
    qint64 files = 0;        // audio files found on disk
    qint64 unchanged = 0;    // skipped by size and modification time, never opened
    qint64 added = 0, updated = 0;
    qint64 missing = 0;      // newly marked missing (their volume is online)
    qint64 failed = 0;       // files whose tags could not be read (still added, untagged)
    qint64 skippedMounts = 0; // other filesystems mounted inside a root, not crossed
    QStringList unreadable;  // folders that could not be listed; nothing under them is marked missing
    QStringList failures;    // paths of files whose tags failed
    qint64 elapsedMs = 0;
    bool cancelled = false;

    QJsonObject toJson() const;
};

// Fills the library from folders on disk.
//
// 1. Walk each root and stat every audio file. Files whose size and
//    modification time (to the nanosecond) match the database are skipped
//    without being opened, which makes a rescan of an unchanged library cost
//    one stat per file.
// 2. Read new and changed files with TagLib on a pool of 4 to 8 threads.
// 3. Write the results in batches, one transaction per batch.
// 4. Mark files that have gone as missing. Only files under a folder that was
//    actually listed are touched, and a root that isn't there fails the scan
//    instead, so an unplugged drive never turns a library into dead links.
//
// The scanner runs on the Library's thread and blocks it; the window runs it
// on a worker thread with its own Library connection. It never crosses into
// another filesystem mounted inside a root: add that as its own root.
class Scanner {
public:
    Scanner(Library &library, const VolumeResolver &resolver, Job *job = nullptr);

    void setWorkers(int count) { m_workers = count; }
    static int defaultWorkers();

    // Adds each folder as a library root (when it isn't one yet) and scans it.
    ScanResult scan(const QStringList &roots);
    // Rescans every library root whose volume is online.
    ScanResult rescanAll();

    // The library roots, as absolute paths on their current mounts (empty
    // root_path when offline).
    static QStringList roots(Library &library);

private:
    bool scanRoot(const QString &root, ScanResult &result);

    Library &m_library;
    VolumeResolver m_resolver; // a copy: it is two paths, and a temporary must not dangle
    Job *m_job;
    int m_workers;
};
