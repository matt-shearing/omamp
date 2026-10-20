// Phase 6: device sync and reports.
//
// SyncTests: the core (profiles, rules, the dry run, copying and converting
// with the real ffmpeg command, the manifest, deletes, targets that aren't
// there) and `omamp sync` / `omamp report`. SyncWindowTests: the Devices
// page, the Devices node in the tree, and saving a list as a report.
//
// Everything lives in temporary folders, the target "devices" too; nothing
// is played and no real drive is looked at (detectDevices gets fake roots).
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "spotifycontroller.h"

#include "artprovider.h"
#include "cli.h"
#include "devicescontroller.h"
#include "devicesync.h"
#include "job.h"
#include "library.h"
#include "librarycontroller.h"
#include "player.h"
#include "playlists.h"
#include "playrecorder.h"
#include "report.h"
#include "scanner.h"
#include "theme.h"
#include "translator.h"
#include "volumes.h"
#include "webcontroller.h"

namespace {

bool haveFfmpeg() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }

bool tone(const QString &file, double seconds, const QStringList &extra) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds),
                        "-af", "volume=0.05", "-map_metadata", "-1"};
    QProcess p;
    p.start("ffmpeg", args + extra + QStringList{file});
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

QStringList tagsOf(const QString &file) {
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_entries", "format_tags=title,artist:stream_tags=title,artist", "-of",
                        "default=noprint_wrappers=1", file});
    p.waitForFinished(30000);
    return QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
}

QString codecOf(const QString &file) {
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "a:0", "-show_entries", "stream=codec_name", "-of",
                        "default=noprint_wrappers=1:nokey=1", file});
    p.waitForFinished(30000);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void write(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

// Every file under a folder, relative, sorted.
QStringList filesUnder(const QString &root) {
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next().mid(root.size() + 1);
    out.sort();
    return out;
}

bool seen(QSignalSpy &spy, int timeout = 30000) { return !spy.isEmpty() || spy.wait(timeout); }

// The four files every case starts from: FLAC, MP3, M4A and WMA.
bool makePristine(const QString &dir) {
    const QString album = dir + "/Neon Coast/Night Drive";
    return tone(album + "/01 midnight highway.flac", 3,
                {"-metadata", "title=Midnight Highway", "-metadata", "artist=Neon Coast", "-metadata",
                 "album=Night Drive", "-metadata", "track=1"}) &&
           tone(album + "/02 Coastline.mp3", 3,
                {"-c:a", "libmp3lame", "-b:a", "64k", "-metadata", "title=Coastline", "-metadata", "artist=Neon Coast",
                 "-metadata", "album=Night Drive", "-metadata", "track=2"}) &&
           tone(album + "/03 Afterglow.m4a", 3,
                {"-c:a", "aac", "-b:a", "64k", "-metadata", "title=Afterglow", "-metadata", "artist=Neon Coast",
                 "-metadata", "album=Night Drive", "-metadata", "track=3"}) &&
           tone(album + "/04 Static.wma", 3,
                {"-c:a", "wmav2", "-b:a", "64k", "-metadata", "title=Static", "-metadata", "artist=Neon Coast",
                 "-metadata", "album=Night Drive", "-metadata", "track=4"});
}

// A case: the pristine files copied into <root>/music and scanned, and a
// static playlist "Road trip" holding all four in track order.
QString makeCase(const QString &tmp, const QString &pristine, int n) {
    const QString root = tmp + QString("/case%1").arg(n);
    QDirIterator it(pristine, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString from = it.next();
        const QString to = root + "/music" + from.mid(pristine.size());
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile::copy(from, to);
    }
    Library library(root + "/library.db");
    library.open();
    Scanner(library, VolumeResolver()).scan({root + "/music"});
    const qint64 pl = Playlists::create(library, "Road trip");
    Playlists::addTracks(library, pl, Query::ids(library, "sort:track"));
    return root;
}

DeviceSync::Profile phoneProfile(const QString &target) {
    DeviceSync::Profile p;
    p.name = "Phone";
    p.kind = "folder";
    p.target = target;
    p.sources << DeviceSync::Source::fromString("playlist:Road trip");
    p.rules << DeviceSync::FormatRule::fromString("flac->opus:96") << DeviceSync::FormatRule::fromString("wma->skip");
    return p;
}

int cli(const QStringList &args, QString *out, QString *err = nullptr) {
    QString o, e;
    QTextStream os(&o), es(&e);
    const int code = Cli::run(args, os, es);
    os.flush();
    es.flush();
    if (out)
        *out = o;
    if (err)
        *err = e;
    return code;
}

} // namespace

class SyncTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_pristine;
    int m_counter = 0;

    QString freshCase() { return makeCase(m_tmp.path(), m_pristine, ++m_counter); }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/xdg-config").toUtf8());
        qputenv("XDG_STATE_HOME", (m_tmp.path() + "/xdg-state").toUtf8());
        if (!haveFfmpeg())
            return;
        m_pristine = m_tmp.path() + "/pristine";
        QVERIFY(makePristine(m_pristine));
    }

    void cleanup() {
        DeviceSync::setFfmpegProgram({});
        DeviceSync::setMountCheck({});
        qunsetenv("OMAMP_SYNC_FREE_BYTES");
    }

    void rulesAndSourcesReadAndWrite() {
        QString why;
        DeviceSync::FormatRule r = DeviceSync::FormatRule::fromString("FLAC, wav -> opus:128", &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(r.from, QStringList({"flac", "wav"}));
        QCOMPARE(r.action, QString("convert"));
        QCOMPARE(r.to, QString("opus"));
        QCOMPARE(r.bitrate, 128);
        QCOMPARE(r.toString(), QString("flac,wav->opus:128"));
        QCOMPARE(DeviceSync::FormatRule::fromString("lossless->mp3").signature(), QString("mp3:256"));
        QCOMPARE(DeviceSync::FormatRule::fromString("*->m4a:192").to, QString("aac"));
        QCOMPARE(DeviceSync::FormatRule::fromString("*->ogg").to, QString("vorbis"));
        QCOMPARE(DeviceSync::FormatRule::fromString("wma->skip").action, QString("skip"));
        QCOMPARE(DeviceSync::FormatRule::fromString("wma->skip").signature(), QString("skip"));
        QCOMPARE(DeviceSync::FormatRule::fromString("flac->flac").signature(), QString("flac"));
        why.clear();
        DeviceSync::FormatRule::fromString("flac->wav", &why);
        QVERIFY(why.contains("opus, mp3, aac, vorbis or flac"));
        why.clear();
        DeviceSync::FormatRule::fromString("flac opus", &why);
        QVERIFY(why.contains("->"));
        why.clear();
        DeviceSync::FormatRule::fromString("flac->opus:fast", &why);
        QVERIFY(why.contains("bitrate"));
        QCOMPARE(DeviceSync::extensionFor("aac"), QString("m4a"));
        QCOMPARE(DeviceSync::extensionFor("vorbis"), QString("ogg"));

        why.clear();
        DeviceSync::Source s = DeviceSync::Source::fromString("autoplaylist:Five stars", &why);
        QVERIFY(why.isEmpty());
        QCOMPARE(s.kind, QString("playlist"));
        QCOMPARE(s.toString(), QString("playlist:Five stars"));
        QCOMPARE(DeviceSync::Source::fromString("query:rating:5 genre:jazz").ref, QString("rating:5 genre:jazz"));
        QCOMPARE(DeviceSync::Source::fromString("all").toString(), QString("library"));
        DeviceSync::Source::fromString("album:Night Drive", &why);
        QVERIFY(why.contains("playlist:<name>"));
    }

    void profilesAreCheckedAndKept() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QString why;
        DeviceSync::Profile p = phoneProfile(dir.filePath("device"));
        p.name = " ";
        QVERIFY(!DeviceSync::save(library, p, &why));
        QVERIFY(why.contains("name"));
        p.name = "Phone";
        p.target = QDir::homePath();
        QVERIFY(!DeviceSync::save(library, p, &why));
        QVERIFY(why.contains("folder of its own"));
        p.target = dir.filePath("device");
        p.sources.clear();
        QVERIFY(!DeviceSync::save(library, p, &why));
        QVERIFY(why.contains("at least one"));
        p = phoneProfile(dir.filePath("device"));
        p.mask = "<Artist>/$If(";
        QVERIFY(!DeviceSync::save(library, p, &why));
        QVERIFY(why.contains("mask"));
        p.mask.clear();
        p.kind = "cloud";
        QVERIFY(!DeviceSync::save(library, p, &why));
        p.kind = "folder";

        const qint64 id = DeviceSync::save(library, p, &why);
        QVERIFY2(id > 0, qPrintable(why));
        p.target = dir.filePath("other");
        QVERIFY(!DeviceSync::save(library, p, &why)); // the same name again
        QVERIFY(why.contains("already"));
        QCOMPARE(DeviceSync::find(library, "phone").id, id);
        QCOMPARE(DeviceSync::find(library, QString::number(id)).name, QString("Phone"));
        QVERIFY(!DeviceSync::find(library, "Car", &why).id);

        // Kept in the settings, so another connection sees it.
        library.close();
        Library again(dir.filePath("library.db"));
        QVERIFY(again.open());
        const QList<DeviceSync::Profile> all = DeviceSync::list(again);
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].rules.size(), 2);
        QCOMPARE(all[0].rules[0].toString(), QString("flac->opus:96"));
        QCOMPARE(all[0].sources[0].toString(), QString("playlist:Road trip"));
        DeviceSync::Profile edited = all[0];
        edited.deleteOthers = true;
        QCOMPARE(DeviceSync::save(again, edited, &why), id);
        QVERIFY(DeviceSync::find(again, "Phone").deleteOthers);
        DeviceSync::recordRun(again, id, {{"ok", true}, {"copied", 3}});
        QVERIFY(DeviceSync::find(again, "Phone").lastSync > 0);
        QVERIFY(DeviceSync::remove(again, id, &why));
        QVERIFY(DeviceSync::list(again).isEmpty());
        QVERIFY(!DeviceSync::remove(again, id, &why));
    }

    // The dry run writes nothing; the sync copies, converts and leaves off
    // as the rules say; a second sync finds everything already there.
    void dryRunThenSyncThenNothingToDo() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = freshCase();
        const QString target = root + "/device/Music";
        Library library(root + "/library.db");
        QVERIFY(library.open());
        const DeviceSync::Profile p = phoneProfile(target);

        const DeviceSync::Plan plan = DeviceSync::plan(library, p);
        QVERIFY2(plan.ok(), qPrintable(plan.error));
        QCOMPARE(plan.target.state, QString("missing"));
        QCOMPARE(plan.copies, 2);       // mp3, m4a
        QCOMPARE(plan.conversions, 1);  // flac
        QCOMPARE(plan.skipped, 1);      // wma, by rule
        QCOMPARE(plan.kept, 0);
        QCOMPARE(plan.deletions, 0);
        QVERIFY(plan.fits);
        QCOMPARE(plan.playlists, QStringList({"Road trip.m3u8"}));
        QVERIFY(!QFileInfo::exists(root + "/device")); // a dry run makes nothing
        QStringList targets;
        for (const DeviceSync::Action &a : plan.actions)
            targets << a.op + " " + a.target;
        QVERIFY(targets.contains("convert Neon Coast/Night Drive/01 Midnight Highway.opus"));
        QVERIFY(targets.contains("copy Neon Coast/Night Drive/02 Coastline.mp3"));
        QVERIFY(targets.contains("copy Neon Coast/Night Drive/03 Afterglow.m4a"));
        const QJsonObject j = plan.toJson();
        QCOMPARE(j.value("convert").toInt(), 1);
        QCOMPARE(j.value("actions").toArray().size(), 4); // keeps are left out, skips are not

        Job job("sync");
        const DeviceSync::Result r = DeviceSync::apply(library, plan, &job);
        QVERIFY2(r.ok, qPrintable(r.failures.join("; ") + r.error));
        QCOMPARE(r.copied, 2);
        QCOMPARE(r.converted, 1);
        QCOMPARE(r.skipped, 1);
        QCOMPARE(job.done(), 3);
        QCOMPARE(filesUnder(target), QStringList({".omamp-sync.json", "Neon Coast/Night Drive/01 Midnight Highway.opus",
                                                  "Neon Coast/Night Drive/02 Coastline.mp3",
                                                  "Neon Coast/Night Drive/03 Afterglow.m4a", "Road trip.m3u8"}));
        // Converted for real, with its tags; copies are byte for byte.
        const QString opus = target + "/Neon Coast/Night Drive/01 Midnight Highway.opus";
        QCOMPARE(codecOf(opus), QString("opus"));
        QVERIFY2(tagsOf(opus).contains("TAG:title=Midnight Highway"), qPrintable(tagsOf(opus).join(",")));
        QCOMPARE(readAll(target + "/Neon Coast/Night Drive/02 Coastline.mp3"),
                 readAll(root + "/music/Neon Coast/Night Drive/02 Coastline.mp3"));
        // The playlist file, in the playlist's order, relative.
        QCOMPARE(QString::fromUtf8(readAll(target + "/Road trip.m3u8")),
                 QString("#EXTM3U\nNeon Coast/Night Drive/01 Midnight Highway.opus\nNeon Coast/Night Drive/02 Coastline.mp3\n"
                         "Neon Coast/Night Drive/03 Afterglow.m4a\n"));
        // No temporary files left.
        for (const QString &f : filesUnder(target))
            QVERIFY2(!f.contains(".omamp-sync-"), qPrintable(f));

        const DeviceSync::Plan again = DeviceSync::plan(library, p);
        QCOMPARE(again.kept, 3);
        QCOMPARE(again.copies + again.conversions + again.deletions, 0);
        QCOMPARE(again.bytesToWrite, 0);
        const DeviceSync::Result r2 = DeviceSync::apply(library, again);
        QVERIFY(r2.ok);
        QCOMPARE(r2.copied + r2.converted, 0);
        QCOMPARE(r2.kept, 3);
    }

    // What changes on the next sync: a changed source is copied again, a
    // track taken out of the list is deleted with its emptied folder, a new
    // mask renames, other files stay unless "delete other files" is on, and
    // hidden files are never touched.
    void changesDeletesAndOtherFiles() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = freshCase();
        const QString target = root + "/device";
        Library library(root + "/library.db");
        QVERIFY(library.open());
        DeviceSync::Profile p = phoneProfile(target);
        QVERIFY(DeviceSync::apply(library, DeviceSync::plan(library, p)).ok);
        write(target + "/.stfolder/marker", "x");
        write(target + "/Notes/readme.txt", "mine");

        // The MP3 changes on disk and is rescanned.
        const QString mp3 = root + "/music/Neon Coast/Night Drive/02 Coastline.mp3";
        QVERIFY(tone(mp3, 5, {"-c:a", "libmp3lame", "-b:a", "64k", "-metadata", "title=Coastline", "-metadata",
                              "artist=Neon Coast", "-metadata", "album=Night Drive", "-metadata", "track=2"}));
        Scanner(library, VolumeResolver()).scan({root + "/music"});
        // The M4A leaves the playlist.
        const qint64 pl = Playlists::resolve(library, "Road trip");
        QList<qint64> remove;
        for (const Playlists::Item &i : Playlists::items(library, pl))
            if (i.path.endsWith(".m4a"))
                remove << i.id;
        QCOMPARE(Playlists::removeItems(library, pl, remove), 1);

        DeviceSync::Plan plan = DeviceSync::plan(library, p);
        QCOMPARE(plan.copies, 1);
        QCOMPARE(plan.kept, 1);
        QCOMPARE(plan.deletions, 1);
        for (const DeviceSync::Action &a : plan.actions) {
            if (a.op == "copy")
                QCOMPARE(a.reason, QString("changed"));
            if (a.op == "delete") {
                QCOMPARE(a.target, QString("Neon Coast/Night Drive/03 Afterglow.m4a"));
                QCOMPARE(a.reason, QString("no longer in the list"));
            }
        }
        QVERIFY(DeviceSync::apply(library, plan).ok);
        QCOMPARE(readAll(target + "/Neon Coast/Night Drive/02 Coastline.mp3"), readAll(mp3));
        QVERIFY(!QFileInfo::exists(target + "/Neon Coast/Night Drive/03 Afterglow.m4a"));
        QVERIFY(QFileInfo::exists(target + "/Notes/readme.txt"));

        // A new mask: the old files are ours, so they go; the folder they
        // leave empty goes too.
        p.mask = "<Artist> - <Title>";
        plan = DeviceSync::plan(library, p);
        QCOMPARE(plan.conversions + plan.copies, 2);
        QCOMPARE(plan.deletions, 2);
        QVERIFY(DeviceSync::apply(library, plan).ok);
        QCOMPARE(filesUnder(target), QStringList({".omamp-sync.json", ".stfolder/marker", "Neon Coast - Coastline.mp3",
                                                  "Neon Coast - Midnight Highway.opus", "Notes/readme.txt",
                                                  "Road trip.m3u8"}));
        QVERIFY(!QFileInfo::exists(target + "/Neon Coast"));

        // Delete other files: the note goes, the hidden folder stays.
        p.deleteOthers = true;
        plan = DeviceSync::plan(library, p);
        QCOMPARE(plan.deletions, 1);
        QCOMPARE(plan.actions.last().reason, QString("not from this sync"));
        QVERIFY(DeviceSync::apply(library, plan).ok);
        QCOMPARE(filesUnder(target), QStringList({".omamp-sync.json", ".stfolder/marker", "Neon Coast - Coastline.mp3",
                                                  "Neon Coast - Midnight Highway.opus", "Road trip.m3u8"}));
    }

    void targetsThatArentThere() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = freshCase();
        Library library(root + "/library.db");
        QVERIFY(library.open());

        // A USB drive that isn't plugged in: refused, and its folder isn't made.
        DeviceSync::Profile usb = phoneProfile(root + "/media/PHONE/Music");
        usb.kind = "usb";
        DeviceSync::setMountCheck([](const QString &, QString *) { return false; });
        DeviceSync::Plan plan = DeviceSync::plan(library, usb);
        QCOMPARE(plan.target.state, QString("not-connected"));
        QVERIFY(plan.error.contains("isn't plugged in"));
        QVERIFY(!DeviceSync::apply(library, plan).ok);
        QVERIFY(!QFileInfo::exists(root + "/media"));
        // Plugged in, with a Music folder not made yet.
        QDir().mkpath(root + "/media/PHONE");
        DeviceSync::setMountCheck([&](const QString &, QString *mount) {
            *mount = root + "/media/PHONE";
            return true;
        });
        plan = DeviceSync::plan(library, usb);
        QVERIFY(!plan.target.ready);
        QCOMPARE(plan.target.state, QString("not-connected"));
        QDir().mkpath(root + "/media/PHONE/Music");
        plan = DeviceSync::plan(library, usb);
        QVERIFY2(plan.ok(), qPrintable(plan.error));
        QCOMPARE(plan.target.mountPoint, root + "/media/PHONE");

        // Syncthing wants its marker.
        DeviceSync::Profile st = phoneProfile(root + "/Sync/Music");
        st.kind = "syncthing";
        QDir().mkpath(root + "/Sync/Music");
        QCOMPARE(DeviceSync::checkTarget(library, st).state, QString("not-syncthing"));
        QDir().mkpath(root + "/Sync/Music/.stfolder");
        QVERIFY(DeviceSync::checkTarget(library, st).ready);

        // Inside the library (or holding it) is refused: the copies would be scanned.
        DeviceSync::Profile inside = phoneProfile(root + "/music/phone");
        QCOMPARE(DeviceSync::checkTarget(library, inside).state, QString("inside-library"));
        inside.target = root;
        QCOMPARE(DeviceSync::checkTarget(library, inside).state, QString("inside-library"));

        // Too little room: the dry run says so and the sync refuses.
        qputenv("OMAMP_SYNC_FREE_BYTES", "1000");
        plan = DeviceSync::plan(library, phoneProfile(root + "/small"));
        QVERIFY(plan.ok());
        QVERIFY(!plan.fits);
        QVERIFY(plan.warnings.join(" ").contains("free on the device"));
        const DeviceSync::Result r = DeviceSync::apply(library, plan);
        QVERIFY(!r.ok);
        QVERIFY(!QFileInfo::exists(root + "/small"));
        qunsetenv("OMAMP_SYNC_FREE_BYTES");

        // No ffmpeg: converting is refused before anything is written; a
        // profile that only copies still works.
        DeviceSync::setFfmpegProgram(root + "/no-such-ffmpeg");
        plan = DeviceSync::plan(library, phoneProfile(root + "/nf"));
        QVERIFY(plan.error.contains("ffmpeg"));
        DeviceSync::Profile copies = phoneProfile(root + "/nf");
        copies.rules.clear();
        plan = DeviceSync::plan(library, copies);
        QVERIFY2(plan.ok(), qPrintable(plan.error));
        QCOMPARE(plan.copies, 4);
        QVERIFY(DeviceSync::apply(library, plan).ok);
    }

    // Cancelled before it starts: nothing is copied, and the run says so.
    void cancelStopsTheSync() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = freshCase();
        Library library(root + "/library.db");
        QVERIFY(library.open());
        const DeviceSync::Plan plan = DeviceSync::plan(library, phoneProfile(root + "/device"));
        Job job("sync");
        job.cancel();
        const DeviceSync::Result r = DeviceSync::apply(library, plan, &job);
        QVERIFY(r.cancelled);
        QVERIFY(!r.ok);
        QCOMPARE(r.copied + r.converted, 0);
        QVERIFY(!QFileInfo::exists(root + "/device/Road trip.m3u8"));
    }

    void devicesAreFound() {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("media/PHONE"));
        QDir().mkpath(dir.filePath("media/CARSTICK"));
        write(dir.filePath("syncthing/config.xml"),
              QString("<configuration><folder id=\"a\" label=\"Phone music\" path=\"%1\"/><folder id=\"b\" path=\"%2\"/>"
                      "</configuration>")
                  .arg(dir.filePath("Sync/Music"), dir.filePath("Sync/Other"))
                  .toUtf8());
        DeviceSync::setDeviceRoots({dir.filePath("media")}, {dir.filePath("syncthing/config.xml")});
        const QList<DeviceSync::Device> found = DeviceSync::detectDevices();
        QStringList seen;
        for (const DeviceSync::Device &d : found)
            seen << d.kind + ":" + d.label;
        seen.sort();
        QCOMPARE(seen, QStringList({"syncthing:Other", "syncthing:Phone music", "usb:CARSTICK", "usb:PHONE"}));
        DeviceSync::setDeviceRoots({}, {});
    }

    void reportsAsCsvAndHtml() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QSqlQuery q(library.db());
        QVERIFY(q.exec("INSERT INTO volumes (key, kind, root_path, added) VALUES ('v', 'local', '/music', 1)"));
        auto add = [&](const QString &title, const QString &artist, int rating) {
            QSqlQuery i(library.db());
            i.prepare("INSERT INTO tracks (volume_id, rel_path, file_name, format, size, mtime_ns, title, artist, album, "
                      "track_no, year, duration_ms, rating, added) VALUES (1, ?, ?, 'flac', 1000, 1, ?, ?, 'A <b>', 1, 1997, "
                      "185000, ?, 1)");
            i.addBindValue(title + ".flac");
            i.addBindValue(title + ".flac");
            i.addBindValue(title);
            i.addBindValue(artist);
            i.addBindValue(rating);
            return i.exec() ? i.lastInsertId().toLongLong() : 0;
        };
        const qint64 a = add("Plain", "Neon Coast", 100);
        const qint64 b = add("=HYPERLINK(\"x\")", "Lia, \"the\" singer", 60);
        QVERIFY(a && b);

        Report::Options csv;
        Report::Output out = Report::render(library, {b, a}, csv);
        QVERIFY2(out.ok(), qPrintable(out.error));
        QCOMPARE(out.tracks, 2);
        const QStringList lines = QString::fromUtf8(out.data).split("\r\n", Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0], QString("Title,Artist,Album,Track,Year,Genre,Length,Rating"));
        // In the order asked for; a formula is kept as text; quotes doubled.
        QCOMPARE(lines[1], QString("\"'=HYPERLINK(\"\"x\"\")\",\"Lia, \"\"the\"\" singer\",A <b>,1,1997,,3:05,3"));
        QCOMPARE(lines[2], QString("Plain,Neon Coast,A <b>,1,1997,,3:05,5"));

        csv.columns = {"artist", "path", "size"};
        out = Report::render(library, {a}, csv);
        QCOMPARE(QString::fromUtf8(out.data), QString("Artist,Path,Size\r\nNeon Coast,/music/Plain.flac,1000\r\n"));
        csv.columns = {"artist", "mood"};
        QVERIFY(Report::render(library, {a}, csv).error.contains("mood"));
        csv.columns.clear();
        csv.format = "pdf";
        QVERIFY(!Report::render(library, {a}, csv).ok());

        Report::Options html;
        html.format = "html";
        html.title = "Road <trip>";
        out = Report::write(library, {a, b}, html, dir.filePath("r.html"));
        QVERIFY(out.ok());
        const QString page = QString::fromUtf8(readAll(dir.filePath("r.html")));
        QVERIFY(page.startsWith("<!doctype html>"));
        QVERIFY(page.contains("<h1>Road &lt;trip&gt;</h1>"));
        QVERIFY(page.contains("2 tracks · 6:10"));
        QVERIFY(page.contains("<td>A &lt;b&gt;</td>"));
        QVERIFY(!page.contains("<b>"));
        QVERIFY(page.contains("prefers-color-scheme: dark"));
        QCOMPARE(page.count("<tr>"), 3);
    }

    // `omamp sync` and `omamp report`, in-process, with --json.
    void commandLine() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = freshCase();
        const QString db = root + "/library.db";
        const QString target = root + "/device";
        QString out, err;
        QCOMPARE(cli({"sync", "create", "Phone", "--to", target, "--source", "playlist:Road trip", "--rule",
                      "flac->opus:96", "--rule", "wma->skip", "--json", "--db", db},
                     &out, &err),
                 0);
        QJsonObject j = QJsonDocument::fromJson(out.toUtf8()).object();
        QVERIFY(j.value("ok").toBool());
        QCOMPARE(j.value("rules").toArray().size(), 2);
        QCOMPARE(cli({"sync", "create", "Bad", "--to", target, "--rule", "flac->wav", "--source", "library", "--db", db}, &out, &err), 1);
        QVERIFY(err.contains("opus, mp3"));

        QCOMPARE(cli({"sync", "run", "Phone", "--dry-run", "--json", "--db", db}, &out, &err), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QVERIFY(j.value("dry_run").toBool());
        QCOMPARE(j.value("copy").toInt(), 2);
        QCOMPARE(j.value("convert").toInt(), 1);
        QCOMPARE(j.value("skip").toInt(), 1);
        QVERIFY(!QFileInfo::exists(target));

        QCOMPARE(cli({"sync", "run", "Phone", "--json", "--db", db}, &out, &err), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QVERIFY2(j.value("ok").toBool(), qPrintable(out));
        QCOMPARE(j.value("converted").toInt(), 1);
        QCOMPARE(j.value("copied").toInt(), 2);
        QVERIFY(QFileInfo::exists(target + "/Road trip.m3u8"));

        QCOMPARE(cli({"sync", "list", "--json", "--db", db}, &out, &err), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        const QJsonObject listed = j.value("profiles").toArray().first().toObject();
        QCOMPARE(listed.value("name").toString(), QString("Phone"));
        QVERIFY(listed.value("last_sync").toInteger() > 0);
        QCOMPARE(listed.value("last_result").toObject().value("converted").toInt(), 1);
        QCOMPARE(listed.value("target_state").toObject().value("state").toString(), QString("ready"));

        QCOMPARE(cli({"sync", "set", "Phone", "--clear-rules", "--delete-others", "--db", db}, &out, &err), 0);
        QVERIFY(out.contains("deletes other files"));
        QCOMPARE(cli({"sync", "run", "Phone", "--dry-run", "--db", db}, &out, &err), 0);
        QVERIFY2(out.contains("delete   Neon Coast/Night Drive/01 Midnight Highway.opus"), qPrintable(out));
        QVERIFY(out.contains("copy     Neon Coast/Night Drive/04 Static.wma"));
        QCOMPARE(cli({"sync", "run", "Nope", "--db", db}, &out, &err), 1);

        // Reports.
        QCOMPARE(cli({"report", "--playlist", "Road trip", "--columns", "track,title", "--db", db}, &out, &err), 0);
        QCOMPARE(out, QString("Track,Title\r\n1,Midnight Highway\r\n2,Coastline\r\n3,Afterglow\r\n4,Static\r\n"));
        QCOMPARE(cli({"report", "title:coastline", "--out", root + "/r.html", "--json", "--db", db}, &out, &err), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j.value("format").toString(), QString("html"));
        QCOMPARE(j.value("tracks").toInt(), 1);
        QVERIFY(QString::fromUtf8(readAll(root + "/r.html")).contains("<h1>title:coastline</h1>"));
        QCOMPARE(cli({"report", "--json", "--db", db}, &out, &err), 1);

        QCOMPARE(cli({"sync", "remove", "Phone", "--db", db}, &out, &err), 0);
        QVERIFY(QFileInfo::exists(target + "/Road trip.m3u8")); // the device is left alone
    }
};

// --- the window ------------------------------------------------------------

namespace {

struct Window {
    QString root;
    Translator translator;
    Theme theme;
    std::unique_ptr<Player> player;
    std::unique_ptr<LibraryController> library;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    explicit Window(const QString &folder)
        : root(folder), translator(folder + "/language"), theme(folder + "/theme", nullptr, folder + "/no-hyprctl") {}

    bool load() {
        {
            QSettings conf(root + "/window.conf", QSettings::IniFormat);
            conf.setValue("window/advanced", true);
        }
        player = std::make_unique<Player>(root + "/state.json");
        library = std::make_unique<LibraryController>(root + "/library.db");
        library->setPlayer(player.get());
        library->setWatching(false);
        library->setArtCacheDir(root + "/art");
        library->setWriteSandbox(root);
        library->recorder()->setFlushing(false);
        library->web()->setPreviewDir(root + "/previews");
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", player.get());
        engine.rootContext()->setContextProperty("library", library.get());
        engine.rootContext()->setContextProperty("commandPalette", library->palette());
        engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(root + "/window.conf"));
        engine.rootContext()->setContextProperty(
            "spotify", new SpotifyController(root + "/spotify.conf", root + "/spotify-art", &engine));
        engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider(root + "/library.db", root + "/art"));
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().size() != 1)
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        return window && QTest::qWaitForWindowExposed(window);
    }
    static QObject *findItem(QQuickItem *item, const QString &name) {
        if (item->objectName() == name)
            return item;
        for (QQuickItem *child : item->childItems())
            if (QObject *o = findItem(child, name))
                return o;
        return nullptr;
    }
    QObject *find(const char *name) const {
        QObject *o = window->findChild<QObject *>(name);
        if (!o)
            o = findItem(window->contentItem(), QString::fromLatin1(name));
        if (!o && window->contentItem()->parentItem())
            o = findItem(window->contentItem()->parentItem(), QString::fromLatin1(name));
        return o;
    }
    bool palette(const QString &text, const QString &id) {
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        for (const QChar c : ">" + text)
            QTest::keyClick(window, c.toLatin1());
        if (!QTest::qWaitFor([&] { return library->palette()->get(0).value("itemId").toString() == id; }, 3000))
            return false;
        QTest::keyClick(window, Qt::Key_Return);
        return true;
    }
};

} // namespace

class SyncWindowTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_pristine;
    int m_counter = 0;

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_CACHE_HOME", (m_tmp.path() + "/xdg-cache").toUtf8());
        qputenv("XDG_DATA_HOME", (m_tmp.path() + "/xdg-data").toUtf8());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/xdg-config").toUtf8());
        qputenv("XDG_STATE_HOME", (m_tmp.path() + "/xdg-state").toUtf8());
        DeviceSync::setDeviceRoots({m_tmp.path() + "/media"}, {});
        if (!haveFfmpeg())
            return;
        m_pristine = m_tmp.path() + "/pristine";
        QVERIFY(makePristine(m_pristine));
    }
    void cleanupTestCase() { DeviceSync::setDeviceRoots({}, {}); }

    // The Devices page from the palette: a profile saved, previewed (the
    // dry run as a job, listed in the sheet) and synced (a job), and the
    // profile's node in the tree showing its tracks.
    void devicesPagePreviewAndSync() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = makeCase(m_tmp.path(), m_pristine, ++m_counter);
        QDir().mkpath(m_tmp.path() + "/media/PHONE");
        Window w(root);
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        DevicesController *devices = c.devices();
        QTRY_VERIFY(c.ready());

        QVERIFY(w.palette("devices", "devices"));
        QObject *sheet = w.find("devicesSheet");
        QVERIFY(sheet);
        QTRY_VERIFY(sheet->property("opened").toBool());
        // No profiles yet: a new one is ready to fill in, and the plugged-in drive is offered.
        QCOMPARE(sheet->property("editId").toInt(), 0);
        QCOMPARE(devices->devices().size(), 1);
        QCOMPARE(devices->devices().first().toMap().value("label").toString(), QString("PHONE"));
        bool hasRoadTrip = false;
        for (const QVariant &v : devices->sourceChoices())
            hasRoadTrip = hasRoadTrip || v.toMap().value("source").toString() == "playlist:Road trip";
        QVERIFY(hasRoadTrip);

        // Fill the form as the page does and save.
        sheet->setProperty("editName", "Phone");
        sheet->setProperty("editTarget", root + "/device");
        sheet->setProperty("editSources", QStringList{"playlist:Road trip"});
        sheet->setProperty("editRules", "flac->opus:96; wma->skip");
        sheet->setProperty("editDirty", true);
        QVariant id;
        QMetaObject::invokeMethod(sheet, "save", Q_RETURN_ARG(QVariant, id));
        QVERIFY(id.toInt() > 0);
        QCOMPARE(devices->profiles().size(), 1);
        QCOMPARE(devices->profiles().first().toMap().value("rules").toStringList(),
                 QStringList({"flac->opus:96", "wma->skip"}));
        QCOMPARE(sheet->property("editDirty").toBool(), false);

        // A bad rule is refused with the reason.
        sheet->setProperty("editRules", "flac->wav");
        QMetaObject::invokeMethod(sheet, "save", Q_RETURN_ARG(QVariant, id));
        QCOMPARE(id.toInt(), 0);
        QVERIFY(devices->lastError().contains("opus, mp3"));
        sheet->setProperty("editRules", "flac->opus:96; wma->skip");
        QMetaObject::invokeMethod(sheet, "save", Q_RETURN_ARG(QVariant, id));
        QVERIFY(id.toInt() > 0);

        // Preview: a job, then the list.
        QSignalSpy previewed(devices, &DevicesController::previewChanged);
        QMetaObject::invokeMethod(w.find("previewProfile"), "clicked");
        QTRY_VERIFY_WITH_TIMEOUT(devices->preview().value("ok").toBool(), 30000);
        QCOMPARE(devices->preview().value("copy").toInt(), 2);
        QCOMPARE(devices->preview().value("convert").toInt(), 1);
        QCOMPARE(devices->preview().value("skip").toInt(), 1);
        QVERIFY(!QFileInfo::exists(root + "/device"));
        QObject *list = w.find("previewList");
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 5); // 4 tracks and the playlist file
        QVERIFY(w.find("previewSummary")->property("text").toString().startsWith("2 to copy, 1 to convert"));

        // Sync now: a job in the jobs bar; the files arrive and the profile says so.
        QSignalSpy synced(devices, &DevicesController::synced);
        QMetaObject::invokeMethod(w.find("syncProfile"), "clicked");
        QVERIFY(seen(synced, 60000));
        QVERIFY2(synced.first().at(1).toJsonObject().value("ok").toBool(),
                 qPrintable(QJsonDocument(synced.first().at(1).toJsonObject()).toJson()));
        QVERIFY(QFileInfo::exists(root + "/device/Neon Coast/Night Drive/01 Midnight Highway.opus"));
        QVERIFY(QFileInfo::exists(root + "/device/Road trip.m3u8"));
        QVERIFY(devices->profiles().first().toMap().value("lastText").toString().startsWith("Synced"));
        QVERIFY(devices->preview().isEmpty());

        // The tree: Devices > Phone shows the playlist's four tracks.
        QMetaObject::invokeMethod(sheet, "close");
        const int row = c.tree()->reveal({"devices", "device:" + QString::number(id.toInt())});
        QVERIFY(row >= 0);
        c.selectNode(row);
        QCOMPARE(c.currentKind(), QString("device"));
        QTRY_COMPARE(c.tracks()->count(), 4);
        // Sync from the palette, on the chosen device: nothing left to do.
        synced.clear();
        QVERIFY(w.palette("sync the chosen", "sync-device"));
        QVERIFY(seen(synced, 60000));
        QCOMPARE(synced.first().at(1).toJsonObject().value("kept").toInt(), 3);
    }

    // Saving the list on screen as a report.
    void listAsReport() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg");
        const QString root = makeCase(m_tmp.path(), m_pristine, ++m_counter);
        Window w(root);
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        QTRY_VERIFY(c.ready());
        QTRY_COMPARE(c.tracks()->count(), 4);
        QCOMPARE(c.exportListTo(root + "/everything.csv", "csv"), 4);
        QCOMPARE(QString::fromUtf8(readAll(root + "/everything.csv")).count("\r\n"), 5);

        // A playlist: its entries, in its order, and its name as the title.
        const int row = c.tree()->reveal({"playlists", "playlist:1"});
        QVERIFY(row >= 0);
        c.selectNode(row);
        QCOMPARE(c.exportListTo(root + "/road.html", "html"), 4);
        const QString page = QString::fromUtf8(readAll(root + "/road.html"));
        QVERIFY(page.contains("<h1>Road trip</h1>"));
        QVERIFY(page.indexOf("Midnight Highway") < page.indexOf("Static"));
        // The window writes nowhere outside its sandbox.
        QCOMPARE(c.exportListTo(m_tmp.path() + "/outside.csv", "csv"), -1);
        QVERIFY(!QFileInfo::exists(m_tmp.path() + "/outside.csv"));
    }
};

int runSyncTests(int argc, char **argv) {
    SyncTests tests;
    return QTest::qExec(&tests, argc, argv);
}

int runSyncWindowTests(int argc, char **argv) {
    SyncWindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "sync_tests.moc"
