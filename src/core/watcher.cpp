#include "watcher.h"

#include "job.h"
#include "library.h"
#include "mounts.h"
#include "ownwrites.h"
#include "tagreader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <memory>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Folder events only: a file counts once it is closed after writing (so a
// copy in progress isn't read half-done), moved, deleted, or touched; a
// folder counts when it appears, disappears or moves.
constexpr uint32_t watchMask = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                               IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK;

// Runs rescans on its own thread with its own Library connection.
class RescanWorker : public QObject {
public:
    RescanWorker(const QString &db, const VolumeResolver &resolver, int workers)
        : m_db(db), m_resolver(resolver), m_workers(workers) {}

    QJsonObject run(int id, const QString &kind, const QList<FolderScan> &folders, const QStringList &roots,
                    bool full, bool *changed) {
        *changed = false;
        if (!m_library) {
            m_library = std::make_unique<Library>(m_db);
            QString error;
            if (!m_library->open(&error)) {
                m_library.reset();
                return {{"ok", false}, {"kind", kind}, {"error", error}};
            }
        }
        Job job("rescan");
        {
            QMutexLocker lock(&m_mutex);
            m_job = &job;
            m_jobId = id;
            // Cancelled before it began (the watcher was held meanwhile).
            if (m_cancelId == id)
                job.cancel();
        }
        Scanner scanner(*m_library, m_resolver, &job);
        if (m_workers > 0)
            scanner.setWorkers(m_workers);
        ScanResult r;
        if (kind == "changes") {
            r = scanner.rescanFolders(folders);
        } else {
            // Only roots that are there now: an unmounted share is offline,
            // not a reason to fail the others.
            m_library->refreshVolumes(m_resolver);
            QStringList present;
            for (const LibraryRoot &root : Scanner::libraryRoots(*m_library))
                if (root.online && roots.contains(root.path) && QFileInfo(root.path).isDir())
                    present << root.path;
            if (!present.isEmpty())
                r = scanner.scan(present, full ? Scanner::Mode::Full : Scanner::Mode::ChangedFolders);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_job = nullptr;
        }
        *changed = r.changedLibrary();
        QJsonObject o = r.toJson();
        o["kind"] = kind;
        return o;
    }

    // Cancels the run with this id, whether it is running or was asked for
    // and hasn't begun yet. id 0 means whichever run is going now.
    void cancel(int id = 0) {
        QMutexLocker lock(&m_mutex);
        if (m_job && (id == 0 || id == m_jobId))
            m_job->cancel();
        else if (id != 0)
            m_cancelId = id;
    }

    void close() { m_library.reset(); }

private:
    QString m_db;
    VolumeResolver m_resolver;
    int m_workers;
    std::unique_ptr<Library> m_library;
    QMutex m_mutex;
    Job *m_job = nullptr;
    int m_jobId = 0, m_cancelId = 0;
};

bool isHidden(const QString &name) { return name.startsWith('.'); }

} // namespace

Watcher::Watcher(const QString &dbPath, const VolumeResolver &resolver, QObject *parent)
    : QObject(parent), m_dbPath(dbPath.isEmpty() ? Library::defaultPath() : dbPath), m_resolver(resolver),
      m_debounce(new QTimer(this)), m_schedule(new QTimer(this)) {
    m_maxWatches = qMax(1, systemWatchLimit() / 2);
    m_debounce->setSingleShot(true);
    connect(m_debounce, &QTimer::timeout, this, &Watcher::flush);
    m_schedule->setInterval(900 * 1000);
    connect(m_schedule, &QTimer::timeout, this, &Watcher::rescanScheduledNow);
}

Watcher::~Watcher() { stop(); }

int Watcher::systemWatchLimit() {
    QFile f("/proc/sys/fs/inotify/max_user_watches");
    if (!f.open(QIODevice::ReadOnly))
        return 8192;
    const int n = f.readAll().trimmed().toInt();
    return n > 0 ? n : 8192;
}

void Watcher::setNetworkInterval(int seconds) {
    if (seconds <= 0) {
        m_schedule->stop();
        m_schedule->setInterval(0);
        return;
    }
    m_schedule->setInterval(seconds * 1000);
    if (isRunning() && !scheduledRoots().isEmpty())
        m_schedule->start();
}

QStringList Watcher::scheduledRoots() const {
    QStringList out = m_networkRoots;
    for (const QString &root : m_localRoots)
        if (m_partialRoots.contains(root))
            out << root;
    return out;
}

bool Watcher::start(QString *error) {
    // Starting again re-reads the roots from scratch.
    if (m_fd >= 0) {
        delete m_notifier;
        m_notifier = nullptr;
        ::close(m_fd);
        m_fd = -1;
        m_wdPath.clear();
        m_pathWd.clear();
        m_pathRoot.clear();
    }
    m_localRoots.clear();
    m_networkRoots.clear();
    m_partialRoots.clear();
    m_degraded = false;

    {
        Library library(m_dbPath);
        QString why;
        if (!library.open(&why)) {
            if (error)
                *error = why;
            emit statusChanged();
            return false;
        }
        library.refreshVolumes(m_resolver);
        for (const LibraryRoot &root : Scanner::libraryRoots(library)) {
            if (!root.online || root.path.isEmpty())
                continue;
            if (root.kind == "network" || m_resolver.mountFor(root.path).isNetwork())
                m_networkRoots << root.path;
            else
                m_localRoots << root.path;
        }
    }

    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
        // No inotify at all (or no instances left): everything goes on the schedule.
        if (error)
            *error = QString("inotify unavailable: %1").arg(qt_error_string(errno));
        m_degraded = true;
        for (const QString &root : std::as_const(m_localRoots))
            m_partialRoots.insert(root);
    } else {
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &Watcher::readEvents);
        for (const QString &root : std::as_const(m_localRoots))
            addTree(root, root);
    }

    if (!m_thread) {
        m_thread = new QThread(this);
        m_thread->setObjectName("thrum-rescan");
        auto *worker = new RescanWorker(m_dbPath, m_resolver, m_workers);
        worker->moveToThread(m_thread);
        m_worker = worker;
        m_thread->start(QThread::LowPriority);
    }
    if (m_schedule->interval() > 0 && !scheduledRoots().isEmpty())
        m_schedule->start();
    else
        m_schedule->stop();
    emit statusChanged();
    return m_fd >= 0;
}

void Watcher::stop() {
    m_yielding = false;
    m_debounce->stop();
    m_schedule->stop();
    m_pending.clear();
    m_queue.clear();
    if (m_fd >= 0) {
        delete m_notifier;
        m_notifier = nullptr;
        ::close(m_fd);
        m_fd = -1;
    }
    m_wdPath.clear();
    m_pathWd.clear();
    m_pathRoot.clear();
    if (m_thread) {
        auto *worker = static_cast<RescanWorker *>(m_worker);
        worker->cancel();
        // The worker's Library must close on the thread that opened it.
        QMetaObject::invokeMethod(worker, [worker] { worker->close(); }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
        delete worker;
        m_worker = nullptr;
        delete m_thread;
        m_thread = nullptr;
    }
    if (m_busy) {
        m_busy = false;
        emit busyChanged();
    }
    emit statusChanged();
}

// Adds a watch on every folder below path that the scanner would visit:
// hidden folders, linked folders and other filesystems are left out.
void Watcher::addTree(const QString &path, const QString &root) {
    if (m_fd < 0)
        return;
    struct stat rootStat {};
    if (stat(QFile::encodeName(root).constData(), &rootStat) != 0)
        return;
    QStringList stack{path};
    while (!stack.isEmpty()) {
        const QString dir = stack.takeLast();
        if (m_pathWd.contains(dir))
            continue;
        if (m_wdPath.size() >= m_maxWatches) {
            if (!m_partialRoots.contains(root)) {
                m_partialRoots.insert(root);
                m_degraded = true;
                emit watchLimitReached(m_wdPath.size(), m_maxWatches);
            }
            return;
        }
        const QByteArray encoded = QFile::encodeName(dir);
        const int wd = inotify_add_watch(m_fd, encoded.constData(), watchMask);
        if (wd < 0) {
            if (errno == ENOSPC || errno == ENOMEM) {
                if (!m_partialRoots.contains(root)) {
                    m_partialRoots.insert(root);
                    m_degraded = true;
                    emit watchLimitReached(m_wdPath.size(), systemWatchLimit());
                }
                return;
            }
            continue; // gone already, or not readable: the scanner reports those
        }
        m_wdPath.insert(wd, dir);
        m_pathWd.insert(dir, wd);
        m_pathRoot.insert(dir, root);

        DIR *d = opendir(encoded.constData());
        if (!d)
            continue;
        const int fd = dirfd(d);
        while (dirent *entry = readdir(d)) {
            if (entry->d_name[0] == '.')
                continue;
            if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN)
                continue;
            struct stat st {};
            if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode) ||
                st.st_dev != rootStat.st_dev)
                continue;
            stack << dir + '/' + QFile::decodeName(entry->d_name);
        }
        closedir(d);
    }
}

void Watcher::removeTree(const QString &path) {
    const QString prefix = path + '/';
    for (auto it = m_pathWd.begin(); it != m_pathWd.end();) {
        if (it.key() == path || it.key().startsWith(prefix)) {
            inotify_rm_watch(m_fd, it.value());
            m_wdPath.remove(it.value());
            m_pathRoot.remove(it.key());
            it = m_pathWd.erase(it);
        } else {
            ++it;
        }
    }
}

void Watcher::readEvents() {
    alignas(struct inotify_event) char buffer[64 * 1024];
    bool any = false;
    for (;;) {
        const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
        if (n <= 0)
            break;
        for (char *p = buffer; p < buffer + n;) {
            const auto *e = reinterpret_cast<const struct inotify_event *>(p);
            p += sizeof(struct inotify_event) + e->len;

            if (e->mask & IN_Q_OVERFLOW) {
                // The kernel dropped events: only a full rescan of every
                // watched root can say what happened.
                m_pendingKind = "overflow";
                any = true;
                continue;
            }
            if (e->mask & IN_IGNORED) {
                const QString gone = m_wdPath.take(e->wd);
                m_pathWd.remove(gone);
                m_pathRoot.remove(gone);
                continue;
            }
            const QString dir = m_wdPath.value(e->wd);
            if (dir.isEmpty())
                continue;
            if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                // The folder itself went; its parent's event covers its
                // tracks, unless it is a root, which is rescanned (and, if
                // gone, left alone by the scanner, which refuses a missing root).
                if (m_localRoots.contains(dir)) {
                    queue(dir, true);
                    any = true;
                }
                continue;
            }
            const QString name = e->len ? QFile::decodeName(e->name) : QString();
            if (name.isEmpty() || isHidden(name))
                continue;
            const QString path = dir + '/' + name;
            // Thrum's own writes (tags saved, files organised or trashed)
            // update the library themselves: they are no reason to rescan.
            // A folder we made is still watched, so later changes in it count.
            const bool own = OwnWrites::isOwn(path);
            if (own)
                ++m_ownEvents;
            if (e->mask & IN_ISDIR) {
                if (e->mask & (IN_CREATE | IN_MOVED_TO)) {
                    addTree(path, m_pathRoot.value(dir, dir));
                    if (!own) {
                        queue(path, true);
                        any = true;
                    }
                } else if (e->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    removeTree(path);
                    if (!own) {
                        queue(path, true);
                        any = true;
                    }
                }
                continue;
            }
            if (!own && TagReader::isAudio(name)) {
                queue(dir, false);
                any = true;
            }
        }
    }
    if (!any)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_firstPendingMs)
        m_firstPendingMs = now;
    // Wait for a quiet moment, but never longer than the maximum delay.
    const qint64 left = m_maxDelayMs - (now - m_firstPendingMs);
    m_debounce->start(int(qBound<qint64>(0, qMin<qint64>(m_debounceMs, left), qint64(m_debounceMs))));
}

void Watcher::queue(const QString &path, bool recursive) {
    auto it = m_pending.find(path);
    if (it == m_pending.end())
        m_pending.insert(path, recursive);
    else
        it.value() = it.value() || recursive;
}

void Watcher::rescanFolders(const QStringList &folders, bool recursive) {
    for (const QString &folder : folders)
        queue(QDir::cleanPath(folder), recursive);
    if (!m_firstPendingMs)
        m_firstPendingMs = QDateTime::currentMSecsSinceEpoch();
    m_debounce->start(0);
}

void Watcher::flush() {
    m_firstPendingMs = 0;
    const QString kind = m_pendingKind;
    m_pendingKind = "changes";
    if (kind == "overflow") {
        m_pending.clear();
        emit foldersChanged(m_localRoots);
        if (m_autoRescan)
            m_queue.append({kind, {}, m_localRoots});
    } else {
        if (m_pending.isEmpty())
            return;
        QStringList paths = m_pending.keys();
        paths.sort();
        QList<FolderScan> folders;
        for (const QString &path : std::as_const(paths))
            folders.append({path, m_pending.value(path)});
        m_pending.clear();
        emit foldersChanged(paths);
        if (!m_autoRescan)
            return;
        // Merge into a waiting request rather than queueing a second one.
        if (!m_queue.isEmpty() && m_queue.last().kind == "changes")
            m_queue.last().folders += folders;
        else
            m_queue.append({kind, folders, {}});
    }
    runNext();
}

void Watcher::rescanScheduledNow() {
    const QStringList roots = scheduledRoots();
    if (roots.isEmpty() || !m_thread)
        return;
    for (const Request &r : std::as_const(m_queue))
        if (r.kind == "scheduled")
            return; // one is already waiting
    m_queue.append({"scheduled", {}, roots});
    runNext();
}

void Watcher::rescanRoots(const QStringList &roots) {
    if (roots.isEmpty() || !m_thread)
        return;
    for (Request &r : m_queue)
        if (r.kind == "remount") {
            for (const QString &root : roots)
                if (!r.roots.contains(root))
                    r.roots << root;
            return;
        }
    m_queue.append({"remount", {}, roots});
    runNext();
}

void Watcher::followMounts(MountMonitor *monitor) {
    connect(monitor, &MountMonitor::volumesCameOnline, this, [this](const QList<qint64> &, const QStringList &roots) {
        if (!isRunning())
            return;
        start();
        rescanRoots(roots);
    });
    // A share that went away leaves its roots on the schedule; the rescan
    // finds them offline and skips them, so only the list needs refreshing.
    connect(monitor, &MountMonitor::volumesWentOffline, this, [this](const QList<qint64> &) {
        if (isRunning())
            start();
    });
}

void Watcher::setHeld(bool held) {
    if (held == m_held)
        return;
    m_held = held;
    if (held) {
        // A rescan already running gives way: it stops at the next file
        // and runs again, from the start, once the hold ends.
        if (m_busy && m_worker) {
            m_yielding = true;
            static_cast<RescanWorker *>(m_worker)->cancel(m_runId);
        }
    } else {
        runNext();
    }
}

void Watcher::runNext() {
    if (m_busy || m_held || m_queue.isEmpty() || !m_thread)
        return;
    Request request = m_queue.takeFirst();
    // Every fourth scheduled run lists every folder, to catch files changed
    // in place (which leave their folder's modification time alone). A run
    // that gave way keeps what it was.
    if (request.full < 0)
        request.full = request.kind == "overflow" || (request.kind == "scheduled" && ++m_scheduledRuns % 4 == 0);
    const bool full = request.full > 0;
    // Numbered before anyone hears of it, so a hold that comes from a
    // handler of rescanStarted cancels this run.
    m_current = request;
    ++m_rescansStarted;
    const int id = ++m_runId;
    m_busy = true;
    emit busyChanged();
    QStringList shown = request.roots;
    for (const FolderScan &f : request.folders)
        shown << f.path;
    emit rescanStarted(request.kind, shown);
    if (!m_busy || !m_worker)
        return; // stopped by a handler meanwhile

    auto *worker = static_cast<RescanWorker *>(m_worker);
    QPointer<Watcher> self(this);
    QMetaObject::invokeMethod(worker, [worker, id, request, full, self] {
        bool changed = false;
        const QJsonObject result = worker->run(id, request.kind, request.folders, request.roots, full, &changed);
        if (Watcher *watcher = self.data())
            QMetaObject::invokeMethod(watcher, [self, result, changed] {
                if (self)
                    self->finished(result, changed);
            }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void Watcher::finished(const QJsonObject &result, bool changed) {
    if (!m_busy)
        return; // stopped meanwhile
    m_busy = false;
    const bool yielded = m_yielding && result.value("cancelled").toBool();
    m_yielding = false;
    if (yielded) {
        // It gave way to a job of the window's: it runs again first, once
        // the hold ends.
        m_queue.prepend(m_current);
        QJsonObject r = result;
        r["yielded"] = true;
        emit rescanFinished(r);
        if (changed)
            emit libraryChanged();
        emit busyChanged();
        runNext();
        return;
    }
    emit rescanFinished(result);
    if (changed)
        emit libraryChanged();
    emit busyChanged();
    runNext();
}
