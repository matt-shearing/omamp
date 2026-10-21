// Phase 5: network volumes. Hung, stale and unmounted shares, the mount
// monitor, re-assigning a volume, the parallel folder walker and its latency
// cost model, the waveform cache, the database-on-a-share guard, the mount
// guide and the `thrum volume` commands. Every share here is a temporary
// folder that a fake mount table calls cifs or nfs; nothing is mounted and
// no network is touched.

#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <condition_variable>
#include <mutex>

#include <cerrno>
#include <sys/stat.h>

#include "cli.h"
#include "library.h"
#include "mounts.h"
#include "peaks.h"
#include "scanner.h"
#include "volumes.h"
#include "walker.h"
#include "watcher.h"

namespace {

void write(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

QVariant value(Library &library, const QString &sql) {
    QSqlQuery q(library.db());
    if (!q.exec(sql) || !q.next())
        return {};
    return q.value(0);
}

// A fake machine: a mountinfo file, so a test can mount, unmount and move a
// share by rewriting one file.
struct FakeMounts {
    QTemporaryDir dir;
    QString mountInfo() const { return dir.filePath("mountinfo"); }
    QString byUuid() const { return dir.filePath("by-uuid"); }

    // Each mount is {mount point, root, fs type, source}.
    void setMounts(const QList<QStringList> &mounts) {
        QByteArray text = "1 0 0:1 / / rw - rootfs rootfs rw\n";
        int id = 2;
        for (const QStringList &m : mounts) {
            QString point = m[0];
            point.replace(' ', "\\040");
            text += QString("%1 1 0:%1 %2 %3 rw,relatime shared:1 - %4 %5 rw\n")
                        .arg(id++)
                        .arg(m[1], point, m[2], m[3])
                        .toUtf8();
        }
        write(mountInfo(), text);
    }

    VolumeResolver resolver() const { return VolumeResolver(mountInfo(), byUuid()); }
};

// Audio-named files with no audio in them: the scanner adds them (as
// untagged, failed reads), which is all the volume tests need.
void album(const QString &folder, int files) {
    for (int i = 1; i <= files; ++i)
        write(QString("%1/%2 Song.mp3").arg(folder).arg(i, 2, 10, QChar('0')), QByteArray(1000 + i, 'x'));
}

// A stat() hook that hangs or fails for paths under one prefix.
struct StatTrap {
    std::mutex mutex;
    std::condition_variable released;
    QByteArray prefix;
    int error = 0;          // errno to fail with
    bool hang = false;
    bool open = false;      // set to let hung calls through

    void install() {
        PathProbe::setStatHook([this](const QByteArray &path, struct stat *st) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!prefix.isEmpty() && path.startsWith(prefix)) {
                if (hang) {
                    // Gives up after 10 s so a failing test can't wedge the run.
                    released.wait_for(lock, std::chrono::seconds(10), [&] { return open; });
                } else if (error) {
                    return error;
                }
            }
            lock.unlock();
            return ::stat(path.constData(), st) == 0 ? 0 : errno;
        });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
            hang = false;
        }
        released.notify_all();
    }
    void reset() {
        release();
        std::lock_guard<std::mutex> lock(mutex);
        prefix.clear();
        error = 0;
        open = false;
    }
};

int cli(const QStringList &args, QString *outText, QString *errText = nullptr, const VolumeResolver *resolver = nullptr) {
    QString o, e;
    QTextStream out(&o), err(&e);
    const int code = Cli::run(args, out, err, resolver);
    out.flush();
    err.flush();
    if (outText)
        *outText = o;
    if (errText)
        *errText = e;
    return code;
}

} // namespace

class NasTests : public QObject {
    Q_OBJECT

    StatTrap m_trap;
    QByteArray m_oldCache, m_oldConfig;
    QTemporaryDir m_xdg;

    // A library on a fake cifs share: the share folder, mounted (in the fake
    // table) at itself, scanned once.
    struct Share {
        QTemporaryDir dir;
        FakeMounts mounts;
        QString root;
        std::unique_ptr<Library> library;
    };
    std::unique_ptr<Share> makeShare(int albums = 2, int files = 3) {
        auto s = std::make_unique<Share>();
        s->root = QFileInfo(s->dir.path()).canonicalFilePath() + "/share";
        for (int a = 1; a <= albums; ++a)
            album(QString("%1/Artist/Album %2").arg(s->root).arg(a), files);
        s->mounts.setMounts({{s->root, "/music", "cifs", "//nas/music"}});
        s->library = std::make_unique<Library>(s->dir.filePath("library.db"));
        QString error;
        if (!s->library->open(&error))
            qWarning() << error;
        Scanner scanner(*s->library, s->mounts.resolver());
        const ScanResult r = scanner.scan({s->root});
        if (!r.ok)
            qWarning() << r.error;
        return s;
    }

private slots:
    void initTestCase() {
        m_oldCache = qgetenv("XDG_CACHE_HOME");
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_xdg.filePath("cache")));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_xdg.filePath("config")));
        PathProbe::setDefaultTimeoutMs(300);
        m_trap.install();
    }

    void cleanupTestCase() {
        m_trap.reset();
        PathProbe::setStatHook({});
        PathProbe::setDefaultTimeoutMs(1500);
        qputenv("XDG_CACHE_HOME", m_oldCache);
        qputenv("XDG_CONFIG_HOME", m_oldConfig);
        if (m_oldCache.isEmpty())
            qunsetenv("XDG_CACHE_HOME");
        if (m_oldConfig.isEmpty())
            qunsetenv("XDG_CONFIG_HOME");
    }

    void cleanup() { m_trap.reset(); }

    void migrationNineKeepsVolumesAndFolders() {
        QTemporaryDir dir;
        const QString db = dir.filePath("library.db");
        {
            Library old(db);
            QVERIFY(old.open(nullptr, 7));
            QSqlQuery q(old.db());
            QVERIFY(q.exec("INSERT INTO volumes (key, kind, root_path, added) VALUES ('uuid:a', 'local', '/m', 1)"));
            QVERIFY(q.exec("INSERT INTO folders (volume_id, rel_path, is_root, added) VALUES (1, '', 1, 1)"));
            QVERIFY(q.exec("INSERT INTO tracks (volume_id, folder_id, rel_path, file_name, size, mtime_ns, added, updated) "
                           "VALUES (1, 1, 'a.flac', 'a', 1, 1, 1, 1)"));
        }
        Library library(db);
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QVERIFY(Library::latestVersion() >= 9);
        // The rebuild of volumes must not cascade into folders or orphan tracks.
        QCOMPARE(value(library, "SELECT count(*) FROM folders").toInt(), 1);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks").toInt(), 1);
        QCOMPARE(value(library, "SELECT key FROM volumes WHERE id = 1").toString(), QString("uuid:a"));
        QSqlQuery check(library.db());
        QVERIFY(check.exec("PRAGMA foreign_key_check"));
        QVERIFY(!check.next());
        QCOMPARE(value(library, "PRAGMA foreign_keys").toInt(), 1);
        QSqlQuery q(library.db());
        QVERIFY(q.exec("INSERT INTO volumes (key, kind, root_path, added, scan_workers) "
                       "VALUES ('subsonic:1', 'subsonic', 'subsonic://1', 1, 8)"));
        QVERIFY(!q.exec("INSERT INTO volumes (key, kind, root_path, added) VALUES ('x', 'floppy', '/x', 1)"));
        QVERIFY(q.exec("SELECT count(*) FROM remote_sources"));
        // Deleting a volume still cascades to its folders: the foreign keys
        // point at the rebuilt table.
        QVERIFY(q.exec("DELETE FROM tracks"));
        QVERIFY(q.exec("DELETE FROM volumes WHERE id = 1"));
        QCOMPARE(value(library, "SELECT count(*) FROM folders").toInt(), 0);
    }

    void networkFilesystemsAreRecognised() {
        for (const char *type : {"cifs", "smb3", "nfs", "nfs4", "fuse.sshfs", "fuse.rclone", "davfs", "fuse.davfs2", "9p"}) {
            Mount m;
            m.fsType = type;
            QVERIFY2(m.isNetwork(), type);
        }
        for (const char *type : {"ext4", "btrfs", "xfs", "tmpfs", "overlay", "fuse.portal", "vfat"}) {
            Mount m;
            m.fsType = type;
            QVERIFY2(!m.isNetwork(), type);
        }
    }

    void probeNamesErrorsAndNeverHangs() {
        QTemporaryDir dir;
        QVERIFY(PathProbe::probe(dir.path()).online());
        QCOMPARE(PathProbe::probe(dir.filePath("nothing")).state, ProbeState::Absent);

        m_trap.prefix = QFile::encodeName(dir.path());
        m_trap.error = ESTALE;
        QCOMPARE(PathProbe::probe(dir.path()).state, ProbeState::Stale);
        m_trap.error = EIO;
        QCOMPARE(PathProbe::probe(dir.path()).state, ProbeState::IoError);
        m_trap.error = EHOSTDOWN;
        QCOMPARE(PathProbe::probe(dir.path()).state, ProbeState::IoError);
        m_trap.error = 0;

        // A hung stat: the caller gets TimedOut after the timeout, then
        // StillHung at once while the first thread is stuck.
        m_trap.hang = true;
        QElapsedTimer timer;
        timer.start();
        const ProbeResult first = PathProbe::probe(dir.path(), 200);
        QCOMPARE(first.state, ProbeState::TimedOut);
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(PathProbe::stuckThreads(), 1);
        timer.restart();
        QCOMPARE(PathProbe::probe(dir.path(), 200).state, ProbeState::StillHung);
        QVERIFY(timer.elapsed() < 100);
        // The server answers again: the stuck thread returns, the path is free.
        m_trap.release();
        QTRY_COMPARE(PathProbe::stuckThreads(), 0);
        QVERIFY(PathProbe::probe(dir.path()).online());
    }

    void hungShareIsOfflineNeverMissing() {
        auto share = makeShare();
        Library &library = *share->library;
        const VolumeResolver resolver = share->mounts.resolver();
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 6);
        QCOMPARE(value(library, "SELECT kind FROM volumes").toString(), QString("network"));
        QVERIFY(value(library, "SELECT key FROM volumes").toString().startsWith("marker:"));

        // The server stops answering: refreshing costs one probe timeout, not
        // minutes, and the tracks are offline, not missing.
        m_trap.prefix = QFile::encodeName(share->root);
        m_trap.hang = true;
        QElapsedTimer timer;
        timer.start();
        const QList<VolumeCheck> checks = library.refreshVolumes(resolver);
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(checks.size(), 1);
        QVERIFY(!checks[0].online);
        QCOMPARE(checks[0].state, QString("timed-out"));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'offline'").toInt(), 6);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);
        // Again while it is still hung: no wait at all.
        timer.restart();
        library.refreshVolumes(resolver);
        QVERIFY(timer.elapsed() < 200);
        // A scan of the hung root fails fast and marks nothing missing.
        Scanner scanner(library, resolver);
        timer.restart();
        const ScanResult r = scanner.scan({share->root});
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("offline"), qPrintable(r.error));
        QVERIFY(timer.elapsed() < 2000);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);

        // It answers again: back online, every track with it.
        m_trap.release();
        QTRY_COMPARE(PathProbe::stuckThreads(), 0);
        library.refreshVolumes(resolver);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 6);

        // A stale NFS handle or an I/O error reads as offline too.
        m_trap.prefix = QFile::encodeName(share->root);
        m_trap.error = ESTALE;
        QCOMPARE(library.refreshVolumes(resolver).value(0).state, QString("stale"));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'offline'").toInt(), 6);
        m_trap.error = EIO;
        QCOMPARE(library.refreshVolumes(resolver).value(0).state, QString("io-error"));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);
        m_trap.error = 0;
        library.refreshVolumes(resolver);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 6);
    }

    void unmountedShareStaysOfflineThroughRescans() {
        auto share = makeShare();
        Library &library = *share->library;
        share->mounts.setMounts({});
        const VolumeResolver resolver = share->mounts.resolver();
        library.refreshVolumes(resolver);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'offline'").toInt(), 6);
        // The folder is still on disk (an empty mount point would look the
        // same), but its volume is offline: rescanning everything touches nothing.
        Scanner scanner(library, resolver);
        const ScanResult r = scanner.rescanAll(Scanner::Mode::ChangedFolders);
        QVERIFY(!r.ok);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);
    }

    void mountMonitorReportsSharesComingAndGoing() {
        auto share = makeShare();
        const QString db = share->library->path();
        share->library->close();
        MountMonitor monitor(db, share->mounts.resolver());
        monitor.setPollInterval(50);
        QSignalSpy checked(&monitor, &MountMonitor::volumesChecked);
        QSignalSpy down(&monitor, &MountMonitor::volumesWentOffline);
        QSignalSpy up(&monitor, &MountMonitor::volumesCameOnline);
        QSignalSpy changed(&monitor, &MountMonitor::mountsChanged);
        QVERIFY(monitor.start());
        QTRY_VERIFY(checked.count() >= 1);
        const QJsonArray first = checked.first().first().toJsonArray();
        QCOMPARE(first.size(), 1);
        QCOMPARE(first[0].toObject()["online"].toBool(), true);

        share->mounts.setMounts({});
        QTRY_VERIFY_WITH_TIMEOUT(down.count() == 1, 5000);
        QVERIFY(changed.count() >= 1);
        share->mounts.setMounts({{share->root, "/music", "cifs", "//nas/music"}});
        QTRY_VERIFY_WITH_TIMEOUT(up.count() == 1, 5000);
        QCOMPARE(up.first()[1].toStringList(), QStringList{share->root});

        // A hung share is found by the regular probe, with no mount table change.
        m_trap.prefix = QFile::encodeName(share->root);
        m_trap.hang = true;
        QTRY_VERIFY_WITH_TIMEOUT(down.count() == 2, 5000);
        m_trap.release();
        QTRY_VERIFY_WITH_TIMEOUT(up.count() == 2, 5000);
        monitor.stop();
        QTRY_VERIFY(!monitor.isBusy());
    }

    void watcherQuickRescansAShareThatCameBack() {
        auto share = makeShare(1, 2);
        const QString db = share->library->path();
        // A file added while the share was away, which the remount rescan finds.
        share->mounts.setMounts({});
        share->library->refreshVolumes(share->mounts.resolver());
        share->library->close();
        album(share->root + "/Artist/Album 9", 1);

        MountMonitor monitor(db, share->mounts.resolver());
        monitor.setPollInterval(50);
        Watcher watcher(db, share->mounts.resolver());
        watcher.setNetworkInterval(0);
        watcher.followMounts(&monitor);
        QSignalSpy started(&watcher, &Watcher::rescanStarted);
        QSignalSpy finished(&watcher, &Watcher::rescanFinished);
        watcher.start();
        QVERIFY(watcher.networkRoots().isEmpty());
        monitor.start();
        QTRY_VERIFY(!monitor.isBusy());
        share->mounts.setMounts({{share->root, "/music", "cifs", "//nas/music"}});
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 10000);
        QCOMPARE(started.first()[0].toString(), QString("remount"));
        QCOMPARE(started.first()[1].toStringList(), QStringList{share->root});
        QCOMPARE(watcher.networkRoots(), QStringList{share->root});
        const QJsonObject result = finished.first()[0].toJsonObject();
        QCOMPARE(result["added"].toInt(), 1);
        monitor.stop();
        watcher.stop();
    }

    void reassignMovesAVolumeToItsNewPlace() {
        auto share = makeShare();
        Library &library = *share->library;
        const qint64 volume = value(library, "SELECT id FROM volumes").toLongLong();
        const QString key = value(library, "SELECT key FROM volumes").toString();
        const qint64 rated = value(library, "SELECT min(id) FROM tracks").toLongLong();
        QSqlQuery q(library.db());
        QVERIFY(q.exec(QString("UPDATE tracks SET rating = 80, play_count = 5 WHERE id = %1").arg(rated)));

        // The NAS was replaced: the same music, copied to a new share whose
        // copy has no marker, mounted from a new server.
        QTemporaryDir other;
        const QString newRoot = QFileInfo(other.path()).canonicalFilePath() + "/newshare";
        QDir().mkpath(newRoot);
        QProcess cp;
        cp.start("cp", {"-a", share->root + "/Artist", newRoot + "/"});
        QVERIFY(cp.waitForFinished());
        share->mounts.setMounts({{newRoot, "/music", "cifs", "//newnas/music"}});
        const VolumeResolver resolver = share->mounts.resolver();
        library.refreshVolumes(resolver);
        QCOMPARE(value(library, "SELECT online FROM volumes").toInt(), 0);

        // A folder that doesn't hold the library's folders is refused.
        QString error;
        QVERIFY(!Volumes::reassign(library, resolver, volume, newRoot + "/Artist", &error));
        QVERIFY2(error.contains("old root"), qPrintable(error));

        QJsonObject report;
        QVERIFY2(Volumes::reassign(library, resolver, volume, newRoot, &error, &report), qPrintable(error));
        QCOMPARE(report["new_key"].toString(), key); // the old marker carried over
        QVERIFY(report["marker_written"].toBool());
        QVERIFY(QFile::exists(newRoot + "/.thrum-volume-id"));
        QCOMPARE(value(library, "SELECT root_path FROM volumes").toString(), newRoot);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 6);
        QCOMPARE(value(library, QString("SELECT rating FROM tracks WHERE id = %1").arg(rated)).toInt(), 80);
        // And it stays found: the next refresh finds it by its marker.
        library.refreshVolumes(resolver);
        QCOMPARE(value(library, "SELECT online FROM volumes").toInt(), 1);
        // A rescan sees the same files: nothing added, nothing missing.
        Scanner scanner(library, resolver);
        const ScanResult r = scanner.rescanAll();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.added, 0);
        QCOMPARE(r.missing, 0);
        QCOMPARE(r.unchanged, 6);
    }

    void reassignByCliAndForget() {
        auto share = makeShare(1, 2);
        const QString db = share->library->path();
        share->library->close();
        const VolumeResolver resolver = share->mounts.resolver();
        QString out, err;
        QCOMPARE(cli({"volume", "list", "--json", "--db", db}, &out, &err, &resolver), 0);
        QJsonArray volumes = QJsonDocument::fromJson(out.toUtf8()).object()["volumes"].toArray();
        QCOMPARE(volumes.size(), 1);
        QCOMPARE(volumes[0].toObject()["kind"].toString(), QString("network"));
        QCOMPARE(volumes[0].toObject()["state"].toString(), QString("online"));
        QCOMPARE(volumes[0].toObject()["scan_workers"].toInt(), 8);
        QCOMPARE(volumes[0].toObject()["tracks"].toInt(), 2);

        QCOMPARE(cli({"volume", "workers", "1", "3", "--db", db, "--json"}, &out, &err, &resolver), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object()["scan_workers"].toInt(), 3);
        QCOMPARE(cli({"volume", "show", share->root + "/Artist", "--db", db}, &out, &err, &resolver), 0);
        QVERIFY2(out.contains("scan workers:  3"), qPrintable(out));

        // Forgetting an online volume needs --force.
        QCOMPARE(cli({"volume", "forget", "1", "--db", db}, &out, &err, &resolver), 1);
        QVERIFY(err.contains("--force"));
        share->mounts.setMounts({});
        QCOMPARE(cli({"volume", "list", "--db", db}, &out, &err, &resolver), 0);
        QVERIFY2(out.contains("absent"), qPrintable(out));
        QCOMPARE(cli({"volume", "forget", "1", "--db", db, "--json"}, &out, &err, &resolver), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object()["tracks_removed"].toInt(), 2);
        QCOMPARE(cli({"volume", "list", "--db", db, "--json"}, &out, &err, &resolver), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object()["volumes"].toArray().size(), 0);
        QCOMPARE(cli({"volume", "show", "7", "--db", db}, &out, &err, &resolver), 1);
    }

    void parallelWalkFindsWhatASerialWalkFinds() {
        QTemporaryDir dir;
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        for (int a = 0; a < 12; ++a)
            for (int b = 0; b < 4; ++b)
                album(QString("%1/A%2/B%3").arg(root).arg(a).arg(b), 3);
        write(root + "/A1/.hidden/x.mp3", "x");
        write(root + "/A1/cover.jpg", "x");
        QFile::link(root + "/A0/B0/01 Song.mp3", root + "/A2/linked.mp3");
        QFile::link(root + "/A3", root + "/A4/loop");

        struct stat st {};
        stat(QFile::encodeName(root).constData(), &st);
        WalkOptions serial;
        serial.isAudio = [](const QString &n) { return n.endsWith(".mp3"); };
        WalkOptions parallel = serial;
        parallel.workers = 8;
        const WalkOutput a = Walker::walk(QFile::encodeName(root), "", st.st_dev, true, serial);
        const WalkOutput b = Walker::walk(QFile::encodeName(root), "", st.st_dev, true, parallel);
        QCOMPARE(a.files.size(), 12 * 4 * 3 + 1); // the linked file counts; the linked folder doesn't
        QCOMPARE(b.files.size(), a.files.size());
        for (int i = 0; i < a.files.size(); ++i) {
            QCOMPARE(b.files[i].rel, a.files[i].rel);
            QCOMPARE(b.files[i].size, a.files[i].size);
            QCOMPARE(b.files[i].mtimeNs, a.files[i].mtimeNs);
        }
        QCOMPARE(b.folders, a.folders);
        QCOMPARE(b.listed, a.listed);

        // Known folder times: folders whose time matches are not listed for files.
        WalkOptions quick = parallel;
        quick.known = &a.folders;
        const WalkOutput c = Walker::walk(QFile::encodeName(root), "", st.st_dev, true, quick);
        QCOMPARE(c.files.size(), 0);
        QCOMPARE(c.skipped.size(), a.folders.size());
    }

    // The cost model behind "a NAS rescan takes minutes, not hours": every
    // listing and every stat is one round trip. Measured here on 2,000 files
    // with 0.5 ms per round trip; the 100,000-file numbers come from
    // `thrum_bench nas` and are recorded in docs/plan/progress.md.
    void latencyCostModel() {
        MemoryFileSystem memory;
        int files = 0;
        for (int a = 0; a < 20; ++a)
            for (int b = 0; b < 10; ++b)
                for (int t = 0; t < 10; ++t, ++files)
                    memory.addFile(QString("/m/A%1/B%2/%3.flac").arg(a).arg(b).arg(t).toUtf8(), 1000 + t, 5);
        auto run = [&](int workers, const QHash<QString, qint64> *known, qint64 *listings, qint64 *stats, WalkOutput *out) {
            auto fs = std::make_shared<LatencyFileSystem>(std::shared_ptr<FileSystem>(&memory, [](FileSystem *) {}), 500);
            WalkOptions o;
            o.fs = fs;
            o.workers = workers;
            o.known = known;
            o.isAudio = [](const QString &n) { return n.endsWith(".flac"); };
            QElapsedTimer t;
            t.start();
            *out = Walker::walk("/m", "", 1, true, o);
            *listings = fs->listings();
            *stats = fs->stats();
            return t.elapsed();
        };
        qint64 l1, s1, l8, s8, lq, sq;
        WalkOutput full1, full8, quick;
        const qint64 ms1 = run(1, nullptr, &l1, &s1, &full1);
        const qint64 ms8 = run(8, nullptr, &l8, &s8, &full8);
        QCOMPARE(full1.files.size(), files);
        QCOMPARE(full8.files.size(), files);
        // One listing per folder (1 root + 20 + 200), one stat per file and per subfolder.
        QCOMPARE(l1, 221);
        QCOMPARE(s1, qint64(files + 220));
        QCOMPARE(l8, l1);
        QCOMPARE(s8, s1);
        const qint64 msq = run(8, &full8.folders, &lq, &sq, &quick);
        QCOMPARE(lq, 221);
        QCOMPARE(sq, 220); // only the subfolders: no file is stat'ed
        QCOMPARE(quick.skipped.size(), 200);
        qInfo().noquote() << QString("NAS cost model, 2,000 files in 221 folders at 0.5 ms a round trip: "
                                     "full walk %1 ms with 1 worker, %2 ms with 8; quick walk %3 ms with 8")
                                 .arg(ms1)
                                 .arg(ms8)
                                 .arg(msq);
        QVERIFY2(ms8 * 3 < ms1, qPrintable(QString("%1 vs %2").arg(ms8).arg(ms1)));
        QVERIFY(msq < ms8);
    }

    void unchangedRescanReadsNoFileOverASlowShare() {
        auto share = makeShare(3, 4);
        Library &library = *share->library;
        // Make every file unreadable: a rescan that opened one would fail it.
        QDirIterator it(share->root, {"*.mp3"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            QFile::setPermissions(it.next(), QFileDevice::Permissions());
        auto fs = std::make_shared<LatencyFileSystem>(FileSystem::posix(), 200);
        Scanner scanner(library, share->mounts.resolver());
        scanner.setFileSystem(fs);
        QCOMPARE(scanner.walkWorkers(value(library, "SELECT id FROM volumes").toLongLong()), 8);
        const ScanResult full = scanner.rescanAll();
        QVERIFY2(full.ok, qPrintable(full.error));
        QCOMPARE(full.unchanged, 12);
        QCOMPARE(full.updated + full.added + full.failed + full.missing, 0);
        QCOMPARE(fs->stats(), qint64(12 + 4)); // 12 files, Artist and its 3 albums
        // Quick: only folders.
        const qint64 before = fs->stats();
        const ScanResult quick = scanner.rescanAll(Scanner::Mode::ChangedFolders);
        QVERIFY(quick.ok);
        QCOMPARE(quick.unchanged, 12);
        QCOMPARE(quick.foldersSkipped, 3);
        QCOMPARE(fs->stats() - before, qint64(4));
        // A file added on the NAS changes its folder's time: only that folder is listed.
        QTest::qWait(10);
        write(share->root + "/Artist/Album 2/05 New.mp3", "new");
        const ScanResult after = scanner.rescanAll(Scanner::Mode::ChangedFolders);
        QCOMPARE(after.added, 1);
        // The share root and Artist (no stored times: no audio in them) and Album 2.
        QCOMPARE(after.foldersListed, 3);
        QDirIterator again(share->root, {"*.mp3"}, QDir::Files, QDirIterator::Subdirectories);
        while (again.hasNext())
            QFile::setPermissions(again.next(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    void scanWorkersPerVolume() {
        auto share = makeShare(1, 1);
        Library &library = *share->library;
        const qint64 id = value(library, "SELECT id FROM volumes").toLongLong();
        Scanner scanner(library, share->mounts.resolver());
        QCOMPARE(scanner.walkWorkers(id), Volumes::defaultNetworkWorkers);
        QString error;
        QVERIFY(Volumes::setScanWorkers(library, id, 16, &error));
        QCOMPARE(scanner.walkWorkers(id), 16);
        QVERIFY(!Volumes::setScanWorkers(library, id, 65, &error));
        QVERIFY(Volumes::setScanWorkers(library, id, 0, &error));
        QCOMPARE(scanner.walkWorkers(id), 8);
        scanner.setWalkWorkers(2);
        QCOMPARE(scanner.walkWorkers(id), 2);
        QSqlQuery q(library.db());
        QVERIFY(q.exec(QString("UPDATE volumes SET kind = 'local' WHERE id = %1").arg(id)));
        scanner.setWalkWorkers(0);
        QCOMPARE(scanner.walkWorkers(id), Volumes::defaultLocalWorkers);
    }

    void databaseOnAShareIsRefused() {
        QTemporaryDir dir;
        FakeMounts mounts;
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        mounts.setMounts({{root, "/", "nfs4", "nas:/export/home"}});
        QString error;
        QVERIFY(!checkDatabaseIsLocal(root + "/sub/library.db", mounts.resolver(), &error));
        QVERIFY2(error.contains("network share") && error.contains("nfs4"), qPrintable(error));
        mounts.setMounts({{root, "/", "ext4", "/dev/sda1"}});
        QVERIFY(checkDatabaseIsLocal(root + "/sub/library.db", mounts.resolver(), &error));

        // Library::open checks the real mount table, which tests can replace.
        mounts.setMounts({{root, "/", "cifs", "//nas/home"}});
        qputenv("THRUM_MOUNTINFO", QFile::encodeName(mounts.mountInfo()));
        Library library(root + "/library.db");
        const bool opened = library.open(&error);
        qunsetenv("THRUM_MOUNTINFO");
        QVERIFY(!opened);
        QVERIFY(error.contains("network share"));
        QVERIFY(!QFile::exists(root + "/library.db"));
    }

    void peaksCacheIsKeyedByVolumeAndFingerprint() {
        QTemporaryDir dir;
        PeaksCache cache(dir.filePath("peaks"));
        PeaksKey key{"marker:abc", "Artist/Album/01.flac", 1234, 5678};
        QVERIFY(!cache.contains(key));
        QVERIFY(cache.store(key, QByteArray("\x01\x02\x03\x04", 4), 512, 2));
        QVERIFY(cache.contains(key));
        QByteArray peaks;
        quint32 spp = 0, count = 0;
        QVERIFY(cache.load(key, &peaks, &spp, &count));
        QCOMPARE(peaks, QByteArray("\x01\x02\x03\x04", 4));
        QCOMPARE(spp, 512u);
        QCOMPARE(count, 2u);
        // A changed file is a new key: its old peaks are not used.
        PeaksKey changed = key;
        changed.mtimeNs = 9999;
        QVERIFY(!cache.contains(changed));
        QVERIFY(cache.pathFor(key).startsWith(dir.filePath("peaks") + '/'));
        {
            QFile old(cache.pathFor(key));
            QVERIFY(old.open(QIODevice::ReadWrite));
            QVERIFY(old.setFileTime(QDateTime::currentDateTime().addSecs(-100), QFileDevice::FileModificationTime));
        }
        QVERIFY(cache.store(changed, QByteArray(100, 'x'), 512, 50));
        QCOMPARE(cache.prune(130), qint64(20)); // the older, smaller one goes first
        QVERIFY(!cache.contains(key));
        QVERIFY(cache.contains(changed));

        auto share = makeShare(1, 1);
        const qint64 track = value(*share->library, "SELECT id FROM tracks").toLongLong();
        const PeaksKey forTrack = PeaksCache::keyFor(*share->library, track);
        QVERIFY(forTrack.isValid());
        QVERIFY(forTrack.volumeKey.startsWith("marker:"));
        QCOMPARE(forTrack.relPath, QString("Artist/Album 1/01 Song.mp3"));
        QVERIFY(!PeaksCache::keyFor(*share->library, 999).isValid());
        // The default folder follows XDG_CACHE_HOME, which these tests point at a temporary folder.
        QVERIFY(PeaksCache().dir().startsWith(m_xdg.path()));
    }

    void playableUrlForFiles() {
        auto share = makeShare(1, 1);
        Library &library = *share->library;
        const qint64 track = value(library, "SELECT id FROM tracks").toLongLong();
        const QUrl url = library.playableUrl(track);
        QVERIFY(url.isLocalFile());
        QCOMPARE(url.toLocalFile(), share->root + "/Artist/Album 1/01 Song.mp3");
        share->mounts.setMounts({});
        library.refreshVolumes(share->mounts.resolver());
        QString error;
        QVERIFY(library.playableUrl(track, &error).isEmpty());
        QVERIFY(error.contains("offline"));
    }

    void mountGuideForSmbAndNfs() {
        QString out, err;
        QCOMPARE(cli({"nas", "mount-guide", "--smb", "//nas/music", "--at", "/mnt/music", "--uid", "1000", "--gid",
                      "1000"},
                     &out, &err),
                 0);
        for (const char *expected :
             {"[Mount]", "What=//nas/music", "Where=/mnt/music", "Type=cifs", "_netdev", "nofail", "uid=1000",
              "gid=1000", "vers=3.1.1", "credentials=/etc/samba/credentials/nas-music", "noserverino",
              "[Automount]", "TimeoutIdleSec=600", "x-systemd.automount", "mnt-music.mount", "mnt-music.automount",
              "sudo systemctl enable --now mnt-music.automount", "sudo pacman -S --needed cifs-utils",
              "sudo systemctl daemon-reload", "sudoedit /etc/samba/credentials/nas-music"})
            QVERIFY2(out.contains(expected), expected);
        QVERIFY(!out.contains("password=secret"));

        QCOMPARE(cli({"nas", "mount-guide", "--nfs", "nas:/volume1/music", "--at", "/mnt/my music", "--json"}, &out, &err), 0);
        const QJsonObject j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j["protocol"].toString(), QString("nfs"));
        const QJsonObject units = j["units"].toObject();
        QVERIFY(units.contains("mnt-my\\x20music.mount"));
        QVERIFY(units.contains("mnt-my\\x20music.automount"));
        const QString mount = units["mnt-my\\x20music.mount"].toString();
        QVERIFY(mount.contains("Type=nfs\n"));
        QVERIFY(mount.contains("vers=4.1"));
        QVERIFY(mount.contains("_netdev,nofail"));
        QVERIFY(j["fstab"].toString().contains("x-systemd.automount"));
        QVERIFY(j["commands"].toArray().first().toString().contains("nfs-utils"));

        // --write-to only writes into the folder given.
        QTemporaryDir dir;
        QCOMPARE(cli({"nas", "mount-guide", "--smb", "//nas/music", "--at", "/mnt/music", "--write-to", dir.path()},
                     &out, &err),
                 0);
        QVERIFY(QFile::exists(dir.filePath("mnt-music.mount")));
        QVERIFY(QFile::exists(dir.filePath("mnt-music.automount")));

        QCOMPARE(cli({"nas", "mount-guide", "--smb", "nas/music", "--at", "/mnt/music"}, &out, &err), 1);
        QCOMPARE(cli({"nas", "mount-guide", "--smb", "//nas/music"}, &out, &err), 1);
        QCOMPARE(cli({"nas", "mount-guide", "--smb", "//nas/music", "--nfs", "a:/b", "--at", "/mnt/x"}, &out, &err), 1);
        QCOMPARE(cli({"nas", "mount-guide", "--nfs", "nas:/x", "--at", "/mnt/old-root/x"}, &out, &err), 1);
    }
};

int runNasTests(int argc, char **argv) {
    NasTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "nas_tests.moc"
