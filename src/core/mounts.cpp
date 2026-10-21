#include "mounts.h"

#include "library.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::atomic<int> g_timeoutMs{1500};
std::atomic<int> g_stuck{0};
QMutex g_mutex;               // guards g_hung and g_hook
QSet<QString> g_hung;         // paths with a probe thread still inside the kernel
PathProbe::StatHook g_hook;

int realStat(const QByteArray &path, struct stat *st) { return ::stat(path.constData(), st) == 0 ? 0 : errno; }

// Reads the marker the way VolumeResolver does, but on the probe thread.
QString readMarkerAt(const QByteArray &dir) {
    const QByteArray file = dir + '/' + VolumeResolver::markerName;
    const int fd = ::open(file.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    char buffer[200];
    const ssize_t n = ::read(fd, buffer, sizeof buffer);
    ::close(fd);
    if (n <= 0)
        return {};
    const QString id = QString::fromUtf8(QByteArray(buffer, int(n)).split('\n').value(0)).trimmed();
    return QUuid::fromString(id).isNull() ? QString() : id;
}

struct Shared {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    ProbeResult result;
};

} // namespace

QString ProbeResult::name() const {
    switch (state) {
    case ProbeState::Online: return "online";
    case ProbeState::Absent: return "absent";
    case ProbeState::Stale: return "stale";
    case ProbeState::IoError: return "io-error";
    case ProbeState::Denied: return "denied";
    case ProbeState::TimedOut: return "timed-out";
    case ProbeState::StillHung: return "still-hung";
    }
    return "absent";
}

QString ProbeResult::describe() const {
    switch (state) {
    case ProbeState::Online: return "online";
    case ProbeState::Absent: return "not there (not mounted, or moved)";
    case ProbeState::Stale: return "the server no longer recognises this share (stale NFS handle); remount it";
    case ProbeState::IoError: return QString("the server stopped answering (%1)").arg(QString::fromLocal8Bit(strerror(error)));
    case ProbeState::Denied: return "mounted, but this user may not read it";
    case ProbeState::TimedOut:
        return QString("did not answer within %1 s (a hung mount)").arg(double(PathProbe::defaultTimeoutMs()) / 1000.0, 0, 'g', 2);
    case ProbeState::StillHung: return "still not answering (an earlier check is waiting on it)";
    }
    return {};
}

int PathProbe::defaultTimeoutMs() { return g_timeoutMs; }
void PathProbe::setDefaultTimeoutMs(int ms) { g_timeoutMs = qMax(10, ms); }
int PathProbe::stuckThreads() { return g_stuck; }

void PathProbe::setStatHook(const StatHook &hook) {
    QMutexLocker lock(&g_mutex);
    g_hook = hook;
}

ProbeState PathProbe::stateForErrno(int error) {
    switch (error) {
    case 0: return ProbeState::Online;
    case ENOENT:
    case ENOTDIR: return ProbeState::Absent;
    case ESTALE: return ProbeState::Stale;
    case EACCES:
    case EPERM: return ProbeState::Denied;
    case ETIMEDOUT: return ProbeState::TimedOut;
    default: return ProbeState::IoError; // EIO, EHOSTDOWN, EHOSTUNREACH, ENOTCONN, ECONNABORTED, ...
    }
}

ProbeResult PathProbe::probe(const QString &path, int timeoutMs, bool readMarker) {
    if (timeoutMs < 0)
        timeoutMs = g_timeoutMs;
    QElapsedTimer timer;
    timer.start();
    StatHook hook;
    {
        QMutexLocker lock(&g_mutex);
        if (g_hung.contains(path)) {
            ProbeResult r;
            r.state = ProbeState::StillHung;
            return r;
        }
        hook = g_hook;
    }

    auto shared = std::make_shared<Shared>();
    const QByteArray encoded = QFile::encodeName(path);
    // Detached on purpose: a thread stuck in the kernel can't be joined or
    // cancelled. It holds only its shared state and copies of its inputs.
    std::thread([shared, encoded, path, hook, readMarker] {
        struct stat st {};
        const int error = hook ? hook(encoded, &st) : realStat(encoded, &st);
        ProbeResult r;
        r.error = error;
        r.state = PathProbe::stateForErrno(error);
        r.isDir = error == 0 && S_ISDIR(st.st_mode);
        if (r.online() && readMarker && r.isDir)
            r.marker = readMarkerAt(encoded);
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->result = r;
            shared->finished = true;
        }
        shared->done.notify_all();
        // If the caller gave up on this thread, the path is free again now.
        QMutexLocker lock(&g_mutex);
        if (g_hung.remove(path))
            --g_stuck;
    }).detach();

    std::unique_lock<std::mutex> lock(shared->mutex);
    const bool answered = shared->done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return shared->finished; });
    if (answered) {
        ProbeResult r = shared->result;
        r.elapsedMs = timer.elapsed();
        return r;
    }
    lock.unlock();
    ProbeResult r;
    r.state = ProbeState::TimedOut;
    r.elapsedMs = timer.elapsed();
    QMutexLocker hungLock(&g_mutex);
    // The thread may have finished between the wait and here; then it has
    // already tried to remove the path, so only mark it hung if it hasn't.
    std::lock_guard<std::mutex> again(shared->mutex);
    if (shared->finished) {
        ProbeResult late = shared->result;
        late.elapsedMs = timer.elapsed();
        return late;
    }
    if (!g_hung.contains(path)) {
        g_hung.insert(path);
        ++g_stuck;
    }
    return r;
}

bool checkDatabaseIsLocal(const QString &dbPath, const VolumeResolver &resolver, QString *error) {
    // The file may not exist yet: resolve the nearest folder that does.
    QString dir = QFileInfo(dbPath).absolutePath();
    QString canonical;
    while (!dir.isEmpty()) {
        canonical = QFileInfo(dir).canonicalFilePath();
        if (!canonical.isEmpty())
            break;
        const QString up = QFileInfo(dir).path();
        if (up == dir)
            break;
        dir = up;
    }
    if (QFileInfo(dbPath).exists())
        canonical = QFileInfo(QFileInfo(dbPath).canonicalFilePath()).path();
    if (canonical.isEmpty())
        return true;
    const Mount mount = resolver.mountFor(canonical);
    if (!mount.isNetwork())
        return true;
    if (error)
        *error = QString("The library file %1 is on a network share (%2, %3). SQLite can corrupt a database "
                         "there; keep the library on a local disk (the default is ~/.local/share/thrum) and add "
                         "the share's music folder to it instead.")
                     .arg(dbPath, mount.fsType, mount.source);
    return false;
}

// --- MountMonitor -------------------------------------------------------

MountMonitor::MountMonitor(const QString &dbPath, const VolumeResolver &resolver, QObject *parent)
    : QObject(parent), m_dbPath(dbPath.isEmpty() ? Library::defaultPath() : dbPath), m_resolver(resolver),
      m_timer(new QTimer(this)) {
    m_timer->setInterval(30000);
    connect(m_timer, &QTimer::timeout, this, &MountMonitor::poll);
}

MountMonitor::~MountMonitor() {
    stop();
    // A check holds a pointer to this monitor. Its probes all time out, so
    // the wait is bounded.
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void MountMonitor::setPollInterval(int ms) { m_timer->setInterval(qMax(10, ms)); }
int MountMonitor::pollInterval() const { return m_timer->interval(); }

bool MountMonitor::start() {
    stop();
    m_running = true;
    QFile table(m_resolver.mountInfoPath());
    if (table.open(QIODevice::ReadOnly))
        m_lastTable = table.readAll();
    // /proc/self/mountinfo raises POLLPRI on every mount and unmount; a
    // QSocketNotifier of type Exception sees it. Files (tests) are polled.
    if (m_resolver.mountInfoPath().startsWith("/proc/")) {
        m_fd = ::open(QFile::encodeName(m_resolver.mountInfoPath()).constData(), O_RDONLY | O_CLOEXEC);
        if (m_fd >= 0) {
            m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Exception, this);
            connect(m_notifier, &QSocketNotifier::activated, this, &MountMonitor::mountTableEvent);
        }
    }
    m_timer->start();
    checkNow();
    return true;
}

void MountMonitor::stop() {
    m_running = false;
    m_timer->stop();
    delete m_notifier;
    m_notifier = nullptr;
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void MountMonitor::mountTableEvent() {
    // Reading the file to the end re-arms POLLPRI.
    if (m_fd >= 0) {
        char buffer[4096];
        ::lseek(m_fd, 0, SEEK_SET);
        while (::read(m_fd, buffer, sizeof buffer) > 0) {
        }
    }
    QFile table(m_resolver.mountInfoPath());
    if (table.open(QIODevice::ReadOnly)) {
        const QByteArray now = table.readAll();
        if (now == m_lastTable)
            return;
        m_lastTable = now;
    }
    emit mountsChanged();
    checkNow();
}

void MountMonitor::poll() {
    // A polled mount table (anything outside /proc) is compared here.
    if (!m_notifier) {
        QFile table(m_resolver.mountInfoPath());
        if (table.open(QIODevice::ReadOnly)) {
            const QByteArray now = table.readAll();
            if (now != m_lastTable) {
                m_lastTable = now;
                emit mountsChanged();
            }
        }
    }
    checkNow();
}

void MountMonitor::checkNow() {
    if (m_busy) {
        m_again = true;
        return;
    }
    m_busy = true;
    emit busyChanged();
    const QString db = m_dbPath;
    const VolumeResolver resolver = m_resolver;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    MountMonitor *self = this;
    // Every probe inside times out, so this thread always ends, and the
    // destructor waits for it.
    m_thread = QThread::create([db, resolver, self] {
        QJsonArray volumes;
        QList<qint64> up, down;
        QStringList roots;
        {
            Library library(db);
            QString error;
            if (library.open(&error)) {
                for (const VolumeCheck &c : library.refreshVolumes(resolver)) {
                    volumes.append(QJsonObject{{"id", c.id}, {"key", c.key}, {"kind", c.kind}, {"label", c.label},
                                               {"online", c.online}, {"root_path", c.root}, {"state", c.state}});
                    if (c.online && !c.wasOnline) {
                        up << c.id;
                        roots << c.libraryRoots;
                    } else if (!c.online && c.wasOnline) {
                        down << c.id;
                    }
                }
            }
        }
        // Posted to the monitor's thread; Qt drops it if the monitor is gone by then.
        QMetaObject::invokeMethod(self, [self, volumes, up, roots, down] {
            self->finished(volumes, up, roots, down);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
}

void MountMonitor::finished(const QJsonArray &volumes, const QList<qint64> &up, const QStringList &roots,
                            const QList<qint64> &down) {
    m_busy = false;
    emit volumesChecked(volumes);
    if (!down.isEmpty())
        emit volumesWentOffline(down);
    if (!up.isEmpty())
        emit volumesCameOnline(up, roots);
    emit busyChanged();
    if (m_again && m_running) {
        m_again = false;
        checkNow();
    }
}
