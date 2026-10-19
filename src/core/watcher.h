#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>

#include "scanner.h"
#include "volumes.h"

class Job;
class Library;
class MountMonitor;
class QSocketNotifier;
class QThread;
class QTimer;

// Keeps the library in step with the disk while Omamp runs.
//
// - Library roots on local filesystems are watched with inotify, one watch
//   per folder. Changes are collected for a moment (the debounce), then just
//   the folders that changed are rescanned. A file moved between two watched
//   folders keeps its track, because both folders are rescanned together.
// - Roots on network filesystems (cifs, nfs, sshfs, ...) are never watched:
//   inotify only sees changes made by this machine. They are rescanned on a
//   schedule instead, in ChangedFolders mode, so a folder whose modification
//   time hasn't changed isn't even listed.
// - If the system's inotify limit (fs.inotify.max_user_watches) or the
//   watcher's own cap runs out, the roots that couldn't be watched in full
//   join the scheduled rescans, and degraded() says so. Nothing fails.
//
// Rescans run on a worker thread with their own Library connection, so the
// window never blocks. The Watcher itself lives on the thread that made it.
class Watcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(bool degraded READ isDegraded NOTIFY statusChanged)
    Q_PROPERTY(int watchedFolders READ watchedFolders NOTIFY statusChanged)
    Q_PROPERTY(QStringList localRoots READ localRoots NOTIFY statusChanged)
    Q_PROPERTY(QStringList networkRoots READ networkRoots NOTIFY statusChanged)
    Q_PROPERTY(QStringList scheduledRoots READ scheduledRoots NOTIFY statusChanged)

public:
    // dbPath empty means the default library. The resolver is copied.
    explicit Watcher(const QString &dbPath = {}, const VolumeResolver &resolver = VolumeResolver(),
                     QObject *parent = nullptr);
    ~Watcher() override;

    // Quiet time after the last change before a rescan (default 1500 ms),
    // and the longest a busy folder can hold a rescan back (default 10 s).
    void setDebounce(int ms) { m_debounceMs = ms; }
    void setMaxDelay(int ms) { m_maxDelayMs = ms; }
    // How often network roots (and roots that couldn't be watched) are
    // rescanned, in seconds. Default 900 (15 minutes); 0 turns it off.
    void setNetworkInterval(int seconds);
    // The most folders this watcher will watch. Default: half of
    // fs.inotify.max_user_watches, leaving the rest for other programs.
    void setMaxWatches(int count) { m_maxWatches = count; }
    // false: report changes (foldersChanged) without rescanning. For tests
    // and `omamp watch --dry-run`.
    void setAutoRescan(bool on) { m_autoRescan = on; }
    void setWorkers(int count) { m_workers = count; }

    // Reads the library roots and starts watching. Call again (or reload())
    // after roots are added or removed.
    bool start(QString *error = nullptr);
    Q_INVOKABLE void reload() { start(); }
    // Stops watching, cancels a running rescan and waits for it.
    void stop();

    bool isRunning() const { return m_fd >= 0; }
    bool isBusy() const { return m_busy; }
    bool isDegraded() const { return m_degraded; }
    int watchedFolders() const { return m_wdPath.size(); }
    QStringList localRoots() const { return m_localRoots; }
    QStringList networkRoots() const { return m_networkRoots; }
    // Network roots plus local roots that couldn't be watched in full.
    QStringList scheduledRoots() const;
    static int systemWatchLimit();

    // Rescans the scheduled roots now (for a "refresh" button, or when a
    // share comes back).
    Q_INVOKABLE void rescanScheduledNow();
    // Queues folders for an incremental rescan, as if they had changed.
    Q_INVOKABLE void rescanFolders(const QStringList &folders, bool recursive = false);
    // Quick-rescans whole library roots (ChangedFolders mode), as a
    // "remount" request: for a share that has just come back.
    Q_INVOKABLE void rescanRoots(const QStringList &roots);
    // Follows a MountMonitor: when a volume comes back online, the roots are
    // re-read (a network root that was offline isn't scheduled yet) and the
    // volume's library folders are quick-rescanned, and only those.
    void followMounts(MountMonitor *monitor);

signals:
    // A debounced batch of changed folders, about to be rescanned.
    void foldersChanged(const QStringList &folders);
    // kind is "changes" (from inotify), "scheduled", "overflow" (the kernel
    // dropped events, so every watched root is rescanned) or "remount" (a
    // share came back).
    void rescanStarted(const QString &kind, const QStringList &folders);
    // result is ScanResult::toJson() plus "kind".
    void rescanFinished(const QJsonObject &result);
    // A rescan added, changed, moved or lost tracks: reload what's on screen.
    void libraryChanged();
    // The watch limit ran out; some folders are rescanned on a schedule.
    void watchLimitReached(int watched, int limit);
    void statusChanged();
    void busyChanged();

private:
    void readEvents();
    void addTree(const QString &path, const QString &root);
    void removeTree(const QString &path);
    void queue(const QString &path, bool recursive);
    void flush();
    void runNext();
    void finished(const QJsonObject &result, bool changed);

    QString m_dbPath;
    VolumeResolver m_resolver;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QHash<int, QString> m_wdPath;
    QHash<QString, int> m_pathWd;
    QHash<QString, QString> m_pathRoot; // watched folder -> its library root
    QStringList m_localRoots, m_networkRoots;
    QSet<QString> m_partialRoots;       // local roots not watched in full
    bool m_degraded = false;
    int m_maxWatches = 0;
    int m_debounceMs = 1500, m_maxDelayMs = 10000, m_workers = 0;
    bool m_autoRescan = true;

    QTimer *m_debounce, *m_schedule;
    qint64 m_firstPendingMs = 0;
    QHash<QString, bool> m_pending;     // folder -> recursive, collecting
    QString m_pendingKind = "changes";

    struct Request {
        QString kind;
        QList<FolderScan> folders; // for "changes"
        QStringList roots;         // for "scheduled" and "overflow"
    };
    QList<Request> m_queue;
    bool m_busy = false;
    int m_scheduledRuns = 0;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
};
