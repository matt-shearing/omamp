#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

#include "volumes.h"

struct stat;
class QSocketNotifier;
class QThread;
class QTimer;

// What a look at a path on a mount found. Anything but Online means the
// volume is treated as offline: its tracks show as offline, never missing.
enum class ProbeState {
    Online,    // stat answered and the path is there
    Absent,    // ENOENT or ENOTDIR: nothing there (an unmounted mount point, a renamed share)
    Stale,     // ESTALE: an NFS handle the server no longer knows
    IoError,   // EIO, EHOSTDOWN, ENOTCONN and friends: the server stopped answering properly
    Denied,    // EACCES or EPERM: mounted, but this user can't read it
    TimedOut,  // no answer within the timeout: a hung mount
    StillHung, // an earlier probe of the same path has not come back yet
};

struct ProbeResult {
    ProbeState state = ProbeState::Absent;
    int error = 0;        // errno, when there was one
    bool isDir = false;
    QString marker;       // the .omamp-volume-id inside, when asked for and present
    qint64 elapsedMs = 0;
    bool online() const { return state == ProbeState::Online; }
    // "online", "absent", "stale", "io-error", "denied", "timed-out", "still-hung".
    QString name() const;
    // A sentence for a person: "did not answer within 1.5 s".
    QString describe() const;
};

// Looks at a path without ever blocking the caller for long.
//
// A hard NFS mount whose server has gone, or an SMB share whose server is
// asleep, can make stat() sleep for minutes in the kernel, where no signal
// reaches it. So each probe runs stat() (and, when asked, reads the volume
// marker) on a short-lived thread of its own and waits at most timeoutMs.
// A thread that doesn't come back is left behind, and the path is
// remembered as hung: later probes of it return StillHung at once instead of
// stranding another thread, until the first one finally returns.
namespace PathProbe {

// Default 1500 ms: long enough for a share that is waking a disk to answer a
// stat (the stat itself is one round trip), short enough not to be noticed.
int defaultTimeoutMs();
void setDefaultTimeoutMs(int ms);

ProbeResult probe(const QString &path, int timeoutMs = -1, bool readMarker = false);

// How many probe threads are still stuck in the kernel.
int stuckThreads();

// Tests replace stat() to fake errors and hangs. The hook returns 0 or an
// errno and fills st. An empty function restores the real stat().
using StatHook = std::function<int(const QByteArray &path, struct stat *st)>;
void setStatHook(const StatHook &hook);

ProbeState stateForErrno(int error);

}

// Refuses a library database on a network mount. SQLite's locking relies on
// POSIX advisory locks, which SMB and NFS implement loosely or not at all; a
// database there can be corrupted by a second machine or a dropped
// connection. The check follows symlinks: the folder that will hold the file
// is resolved first.
bool checkDatabaseIsLocal(const QString &dbPath, const VolumeResolver &resolver, QString *error);

// Notices shares coming and going, and tells the library.
//
// - The mount table: /proc/self/mountinfo raises POLLPRI when anything is
//   mounted or unmounted, which a QSocketNotifier picks up at no cost. Any
//   other mount table (tests use files) is polled.
// - A hung mount leaves the mount table alone, so every volume is also
//   probed every pollInterval (default 30 s).
//
// Each check runs on a worker thread with its own Library connection, so the
// window never waits on a share. It refreshes the volumes table (online,
// root_path) and the tracks' availability, then reports what changed. A
// network volume that comes back is reported in volumesCameOnline, so the
// Watcher can quick-rescan just that volume.
class MountMonitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)

public:
    explicit MountMonitor(const QString &dbPath = {}, const VolumeResolver &resolver = VolumeResolver(),
                          QObject *parent = nullptr);
    ~MountMonitor() override;

    void setPollInterval(int ms);
    int pollInterval() const;

    bool start();
    void stop();
    bool isBusy() const { return m_busy; }

    // Checks every volume now, on the worker thread. A check asked for while
    // one runs happens once it finishes.
    Q_INVOKABLE void checkNow();

signals:
    // The mount table itself changed.
    void mountsChanged();
    // After every check: [{id, key, kind, label, online, root_path, state}].
    void volumesChecked(const QJsonArray &volumes);
    void volumesCameOnline(const QList<qint64> &ids, const QStringList &roots);
    void volumesWentOffline(const QList<qint64> &ids);
    void busyChanged();

private:
    void mountTableEvent();
    void poll();
    void finished(const QJsonArray &volumes, const QList<qint64> &up, const QStringList &roots,
                  const QList<qint64> &down);

    QString m_dbPath;
    VolumeResolver m_resolver;
    QSocketNotifier *m_notifier = nullptr;
    int m_fd = -1;
    QTimer *m_timer;
    QThread *m_thread = nullptr;
    QByteArray m_lastTable;
    bool m_busy = false, m_again = false, m_running = false;
};
