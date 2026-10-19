#include "walker.h"

#include "job.h"

#include <QFile>
#include <QMutex>
#include <QWaitCondition>
#include <algorithm>
#include <deque>
#include <thread>
#include <vector>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

qint64 mtimeOf(const struct stat &st) { return qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec; }

FsStat fromStat(const struct stat &st) {
    FsStat s;
    s.isDir = S_ISDIR(st.st_mode);
    s.isRegular = S_ISREG(st.st_mode);
    s.isLink = S_ISLNK(st.st_mode);
    s.device = quint64(st.st_dev);
    s.size = qint64(st.st_size);
    s.mtimeNs = mtimeOf(st);
    return s;
}

class PosixFileSystem : public FileSystem {
public:
    int list(const QByteArray &path, FsStat *self, QList<FsEntry> *entries, const Wanted &wanted) override {
        DIR *dir = opendir(path.constData());
        if (!dir)
            return errno ? errno : EIO;
        const int fd = dirfd(dir);
        struct stat st {};
        if (fstat(fd, &st) == 0)
            *self = fromStat(st);
        while (dirent *entry = readdir(dir)) {
            FsEntry e;
            e.name = QByteArray(entry->d_name);
            e.type = entry->d_type;
            if (!wanted(e.name, e.type))
                continue;
            struct stat est {};
            e.statted = true;
            if (fstatat(fd, entry->d_name, &est, AT_SYMLINK_NOFOLLOW) != 0) {
                e.statError = errno;
            } else {
                e.stat = fromStat(est);
                if (e.stat.isLink) {
                    struct stat tst {};
                    e.followed = true;
                    if (fstatat(fd, entry->d_name, &tst, 0) != 0)
                        e.followError = errno;
                    else
                        e.target = fromStat(tst);
                }
            }
            entries->append(e);
        }
        closedir(dir);
        return 0;
    }
};

void sleepUs(int us) {
    if (us > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(us));
}

} // namespace

std::shared_ptr<FileSystem> FileSystem::posix() {
    static std::shared_ptr<FileSystem> instance = std::make_shared<PosixFileSystem>();
    return instance;
}

LatencyFileSystem::LatencyFileSystem(std::shared_ptr<FileSystem> inner, int latencyUs)
    : m_inner(std::move(inner)), m_latencyUs(latencyUs) {}

int LatencyFileSystem::list(const QByteArray &path, FsStat *self, QList<FsEntry> *entries, const Wanted &wanted) {
    ++m_listings;
    sleepUs(m_latencyUs);
    const int error = m_inner->list(path, self, entries, wanted);
    for (const FsEntry &e : std::as_const(*entries)) {
        if (e.statted) {
            ++m_stats;
            sleepUs(m_latencyUs);
        }
        if (e.followed) {
            ++m_stats;
            sleepUs(m_latencyUs);
        }
    }
    return error;
}

MemoryFileSystem::MemoryFileSystem() { m_nodes.insert("/", Node{true, 0, 1, {}}); }

MemoryFileSystem::Node &MemoryFileSystem::ensureFolder(const QByteArray &path) {
    auto it = m_nodes.find(path);
    if (it != m_nodes.end())
        return it.value();
    const int slash = path.lastIndexOf('/');
    const QByteArray parent = slash <= 0 ? QByteArray("/") : path.left(slash);
    ensureFolder(parent).children.append(path.mid(slash + 1));
    return m_nodes.insert(path, Node{true, 0, 1, {}}).value();
}

void MemoryFileSystem::addFolder(const QByteArray &path, qint64 mtimeNs) { ensureFolder(path).mtimeNs = mtimeNs; }

void MemoryFileSystem::touchFolder(const QByteArray &path, qint64 mtimeNs) { ensureFolder(path).mtimeNs = mtimeNs; }

void MemoryFileSystem::addFile(const QByteArray &path, qint64 size, qint64 mtimeNs) {
    const int slash = path.lastIndexOf('/');
    Node &parent = ensureFolder(slash <= 0 ? QByteArray("/") : path.left(slash));
    if (!m_nodes.contains(path))
        parent.children.append(path.mid(slash + 1));
    m_nodes.insert(path, Node{false, size, mtimeNs, {}});
}

int MemoryFileSystem::list(const QByteArray &path, FsStat *self, QList<FsEntry> *entries, const Wanted &wanted) {
    // Read-only after setup, so several walkers may list at once.
    const auto it = m_nodes.constFind(path);
    if (it == m_nodes.constEnd())
        return ENOENT;
    if (!it->dir)
        return ENOTDIR;
    self->isDir = true;
    self->device = 1;
    self->mtimeNs = it->mtimeNs;
    for (const QByteArray &name : it->children) {
        const Node &child = m_nodes.value(path == "/" ? '/' + name : path + '/' + name);
        FsEntry e;
        e.name = name;
        e.type = child.dir ? DT_DIR : DT_REG;
        if (!wanted(e.name, e.type))
            continue;
        e.statted = true;
        e.stat.isDir = child.dir;
        e.stat.isRegular = !child.dir;
        e.stat.device = 1;
        e.stat.size = child.size;
        e.stat.mtimeNs = child.mtimeNs;
        entries->append(e);
    }
    return 0;
}

namespace {

QString joinRel(const QString &dir, const QString &name) { return dir.isEmpty() ? name : dir + '/' + name; }

struct Task {
    QByteArray absolute;
    QString rel;
    bool recursive;
};

// Everything one folder's listing found, merged into the output under the lock.
struct FolderResult {
    QList<WalkedFile> files;
    QList<Task> subfolders;
    bool listed = false, skipped = false, hasAudio = false;
    int error = 0;
    qint64 folderTime = 0, skippedMounts = 0;
};

FolderResult listFolder(const Task &task, quint64 rootDevice, const WalkOptions &o, FileSystem &fs) {
    FolderResult r;
    FsStat self;
    bool skipFiles = false;
    bool selfKnown = false;
    QList<FsEntry> entries;
    // The decision to skip a folder's files needs its own modification
    // time, which list() fills before it asks about any entry.
    const FileSystem::Wanted wanted = [&](const QByteArray &name, unsigned char type) {
        if (!selfKnown) {
            selfKnown = true;
            if (o.known) {
                const auto it = o.known->constFind(task.rel);
                skipFiles = it != o.known->constEnd() && it.value() == self.mtimeNs;
            }
        }
        // Hidden entries are skipped: dot folders, the volume marker, and
        // the ._ files macOS leaves on shares.
        if (name.isEmpty() || name[0] == '.')
            return false;
        const bool maybeDir = task.recursive && (type == DT_DIR || type == DT_UNKNOWN);
        const bool maybeFile = !skipFiles && (type == DT_REG || type == DT_LNK || type == DT_UNKNOWN);
        return maybeDir || (maybeFile && o.isAudio(QFile::decodeName(name)));
    };
    r.error = fs.list(task.absolute, &self, &entries, wanted);
    if (r.error)
        return r;
    if (!selfKnown && o.known) {
        // An empty folder: nothing asked, so decide now.
        const auto it = o.known->constFind(task.rel);
        skipFiles = it != o.known->constEnd() && it.value() == self.mtimeNs;
    }
    r.folderTime = self.mtimeNs;
    r.skipped = skipFiles;
    r.listed = !skipFiles;

    for (const FsEntry &e : std::as_const(entries)) {
        if (!e.statted || e.statError)
            continue;
        FsStat st = e.stat;
        const QString fileName = QFile::decodeName(e.name);
        const bool audioName = !skipFiles && o.isAudio(fileName);
        if (st.isLink) {
            // Linked files are followed; linked folders are not, so a link
            // back up the tree can't loop the walk.
            if (!audioName || !e.followed || e.followError || !e.target.isRegular)
                continue;
            st = e.target;
        }
        if (st.isDir) {
            if (!task.recursive)
                continue;
            if (st.device != rootDevice) {
                ++r.skippedMounts;
                continue;
            }
            r.subfolders.append({task.absolute + '/' + e.name, joinRel(task.rel, fileName), true});
        } else if (st.isRegular && audioName) {
            r.files.append({joinRel(task.rel, fileName), task.rel, st.size, st.mtimeNs});
            r.hasAudio = true;
        }
    }
    return r;
}

} // namespace

WalkOutput Walker::walk(const QByteArray &absolute, const QString &rel, quint64 rootDevice, bool recursive,
                        const WalkOptions &options) {
    WalkOutput out;
    std::shared_ptr<FileSystem> fs = options.fs ? options.fs : FileSystem::posix();
    const int workers = qBound(1, options.workers, 64);

    QMutex mutex;
    QWaitCondition wake;
    std::deque<Task> queue{{absolute, rel, recursive}};
    int active = 0;

    auto merge = [&](const Task &task, FolderResult &r) {
        if (r.error) {
            if (r.error == ENOENT || r.error == ENOTDIR)
                out.vanished << task.rel;
            else
                out.unreadable << task.rel;
            return;
        }
        if (r.skipped)
            out.skipped.insert(task.rel);
        else
            ++out.listed;
        out.skippedMounts += r.skippedMounts;
        out.files += r.files;
        if (r.hasAudio)
            out.folders.insert(task.rel, r.folderTime);
        if (options.job && (out.listed + out.skipped.size()) % 64 == 0)
            options.job->setProgress("Looking for music", out.files.size(), 0);
        for (Task &sub : r.subfolders)
            queue.push_back(std::move(sub));
    };

    auto cancelled = [&] { return options.job && options.job->isCancelled(); };

    if (workers == 1) {
        // Depth first, like a plain recursive walk.
        while (!queue.empty() && !cancelled()) {
            const Task task = std::move(queue.back());
            queue.pop_back();
            FolderResult r = listFolder(task, rootDevice, options, *fs);
            std::reverse(r.subfolders.begin(), r.subfolders.end());
            merge(task, r);
        }
    } else {
        auto work = [&] {
            QMutexLocker lock(&mutex);
            for (;;) {
                while (queue.empty() && active > 0)
                    wake.wait(&mutex);
                if (queue.empty() || cancelled()) {
                    wake.wakeAll();
                    return;
                }
                const Task task = std::move(queue.front());
                queue.pop_front();
                ++active;
                lock.unlock();
                FolderResult r = listFolder(task, rootDevice, options, *fs);
                lock.relock();
                --active;
                merge(task, r);
                wake.wakeAll();
            }
        };
        std::vector<std::thread> threads;
        for (int i = 1; i < workers; ++i)
            threads.emplace_back(work);
        work();
        for (std::thread &t : threads)
            t.join();
    }

    std::sort(out.files.begin(), out.files.end(), [](const WalkedFile &a, const WalkedFile &b) { return a.rel < b.rel; });
    return out;
}
