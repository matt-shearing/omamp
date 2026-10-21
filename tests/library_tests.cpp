// The library core: migrations, volumes, the scanner, search and the CLI.
// Every database and every music folder lives in a temporary directory.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "cli.h"
#include "library.h"
#include "query.h"
#include "scanner.h"
#include "tagreader.h"
#include "volumes.h"

namespace {

void write(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QVariant one(Library &library, const QString &sql) {
    QSqlQuery q(library.db());
    if (!q.exec(sql) || !q.next())
        return {};
    return q.value(0);
}

QStringList titles(Library &library, const QString &query) {
    const Query::Result r = Query::run(library, query);
    if (!r.ok())
        qWarning() << query << r.error;
    QStringList out;
    for (const auto &t : r.tracks)
        out << t.title;
    out.sort();
    return out;
}

// A fake machine: a mountinfo file and a /dev/disk/by-uuid folder, so a test
// can mount, unmount and move a "disk" by rewriting one file.
struct FakeMounts {
    QTemporaryDir dir;
    QString mountInfo() const { return dir.filePath("mountinfo"); }
    QString byUuid() const { return dir.filePath("by-uuid"); }

    // A block device file and its by-uuid link.
    QString device(const QString &name, const QString &uuid) {
        const QString dev = dir.filePath("dev/" + name);
        write(dev, "");
        QDir().mkpath(byUuid());
        QFile::link(dev, byUuid() + '/' + uuid);
        return dev;
    }

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

} // namespace

class LibraryTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_fixtures;
    bool m_haveFixtures = false;

    // A fresh copy of the fixture library, so a test can delete and change files.
    QString copyFixtures(QTemporaryDir &into) {
        const QString target = into.filePath("music");
        QProcess cp;
        cp.start("cp", {"-a", m_fixtures.filePath("music"), target});
        cp.waitForFinished();
        return QFileInfo(target).canonicalFilePath();
    }

private slots:
    void initTestCase() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            return;
        QProcess make;
        make.start(QString(THRUM_TESTS_DIR) + "/make-fixtures", {m_fixtures.filePath("music")});
        QVERIFY(make.waitForFinished(120000));
        QVERIFY2(make.exitCode() == 0, make.readAllStandardError().constData());
        m_haveFixtures = true;
    }

    // ---- Migrations --------------------------------------------------------

    void newLibraryGetsTheLatestSchema() {
        QTemporaryDir dir;
        Library library(dir.filePath("deep/folder/library.db"));
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QVERIFY(library.lastBackup().isEmpty());
        QCOMPARE(one(library, "PRAGMA journal_mode").toString(), QString("wal"));
        for (const char *table : {"volumes", "folders", "tracks", "artists", "track_artists", "genres", "albums",
                                  "playlists", "playlist_items", "collections", "play_history", "tag_journal",
                                  "artwork", "jobs", "settings", "tracks_fts"})
            QVERIFY2(one(library, QString("SELECT count(*) FROM sqlite_master WHERE name = '%1'").arg(table)).toInt() == 1,
                     table);
        library.setSetting("answer", 42);
        QCOMPARE(library.setting("answer").toInt(), 42);
    }

    void migratingBacksUpAndIndexesOldRows() {
        QTemporaryDir dir;
        const QString path = dir.filePath("library.db");
        {
            Library v1(path);
            QVERIFY(v1.open(nullptr, 1));
            QCOMPARE(v1.schemaVersion(), 1);
            QSqlQuery q(v1.db());
            QVERIFY(q.exec("INSERT INTO volumes (key, kind, root_path, added) VALUES ('mount:x', 'local', '/m', 0)"));
            QVERIFY(q.exec("INSERT INTO tracks (volume_id, rel_path, file_name, size, mtime_ns, title, added) "
                           "VALUES (1, 'a.flac', 'a', 1, 1, 'Midnight Highway', 0)"));
        }
        Library library(path);
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QVERIFY(!library.lastBackup().isEmpty());
        QVERIFY(QFile::exists(library.lastBackup()));
        QVERIFY(library.lastBackup().contains(".v1-"));
        // The search index was built for the row that predates it.
        QCOMPARE(titles(library, "highway"), QStringList{"Midnight Highway"});
        // The backup is a working version 1 database.
        Library old(library.lastBackup());
        QVERIFY(old.open(nullptr, 1));
        QCOMPARE(old.schemaVersion(), 1);
        QCOMPARE(one(old, "SELECT title FROM tracks").toString(), QString("Midnight Highway"));
    }

    void newerSchemaIsRefused() {
        QTemporaryDir dir;
        const QString path = dir.filePath("library.db");
        {
            Library library(path);
            QVERIFY(library.open());
            QSqlQuery(library.db()).exec("PRAGMA user_version = 999");
        }
        Library library(path);
        QString error;
        QVERIFY(!library.open(&error));
        QVERIFY(error.contains("newer"));
    }

    // ---- Volumes -----------------------------------------------------------

    void mountInfoIsParsed() {
        FakeMounts fake;
        fake.setMounts({{"/mnt/my music", "/", "ext4", "/dev/sdb1"}, {"/mnt/nas", "/", "cifs", "//nas/music"}});
        const VolumeResolver r = fake.resolver();
        const QList<Mount> mounts = r.mounts();
        QCOMPARE(mounts.size(), 3);
        QCOMPARE(mounts[1].mountPoint, QString("/mnt/my music"));
        QVERIFY(!mounts[1].isNetwork());
        QVERIFY(mounts[2].isNetwork());
        QCOMPARE(r.mountFor("/mnt/my music/a/b.flac").mountPoint, QString("/mnt/my music"));
        QCOMPARE(r.mountFor("/mnt/my musicals/x").mountPoint, QString("/"));
    }

    void localDiskIsKnownByUuidWhereverItMounts() {
        FakeMounts fake;
        QTemporaryDir disk;
        const QString root = QFileInfo(disk.path()).canonicalFilePath();
        QDir().mkpath(root + "/Music/Album");
        const QString dev = fake.device("sdb1", "1234-ABCD");
        fake.setMounts({{root, "/", "ext4", dev}});
        VolumeResolver r = fake.resolver();

        const VolumeInfo info = r.identify(root + "/Music/Album", true);
        QCOMPARE(info.key, QString("uuid:1234-ABCD"));
        QCOMPARE(info.kind, QString("local"));
        QCOMPARE(info.rootPath, root);
        QVERIFY(!QFile::exists(root + "/Music/Album/.thrum-volume-id")); // markers are for shares only

        // Unplugged, then plugged in somewhere else.
        fake.setMounts({});
        QVERIFY(r.locate(info.key, root, info.mountSubpath).isEmpty());
        fake.setMounts({{"/run/media/you/usb", "/", "ext4", dev}});
        QCOMPARE(r.locate(info.key, root, info.mountSubpath), QString("/run/media/you/usb"));

        // A btrfs subvolume shares the UUID, so its root is part of the key.
        fake.setMounts({{root, "/@music", "btrfs", dev}});
        QCOMPARE(r.identify(root + "/Music", true).key, QString("uuid:1234-ABCD:/@music"));
    }

    void shareIsKnownByItsMarker() {
        FakeMounts fake;
        QTemporaryDir nas;
        const QString mount = QFileInfo(nas.path()).canonicalFilePath();
        QDir().mkpath(mount + "/music/jazz");
        QDir().mkpath(mount + "/readonly");
        fake.setMounts({{mount, "/", "cifs", "//nas/share"}});
        VolumeResolver r = fake.resolver();

        // Looking (not adding) never writes to the share.
        QCOMPARE(r.identify(mount + "/music", false).key, QString("net://nas/share"));
        QVERIFY(!QFile::exists(mount + "/music/.thrum-volume-id"));

        const VolumeInfo added = r.identify(mount + "/music", true);
        QVERIFY(added.key.startsWith("marker:"));
        QVERIFY(added.markerWritten);
        QCOMPARE(added.rootPath, mount + "/music");
        QCOMPARE(added.mountSubpath, QString("music"));
        QVERIFY(QFile::exists(mount + "/music/.thrum-volume-id"));

        // A subfolder of a known share reuses the share's marker.
        const VolumeInfo sub = r.identify(mount + "/music/jazz", true);
        QCOMPARE(sub.key, added.key);
        QVERIFY(!sub.markerWritten);

        // A read-only share falls back to the server and share name.
        QFile::setPermissions(mount + "/readonly", QFile::ReadOwner | QFile::ExeOwner);
        QCOMPARE(r.identify(mount + "/readonly", true).key, QString("net://nas/share"));
        QFile::setPermissions(mount + "/readonly", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        // Remounted at another path: found by the marker under the new mount.
        QTemporaryDir elsewhere;
        const QString moved = QFileInfo(elsewhere.path()).canonicalFilePath();
        QDir().mkpath(moved + "/music");
        QFile::copy(mount + "/music/.thrum-volume-id", moved + "/music/.thrum-volume-id");
        fake.setMounts({{moved, "/", "nfs4", "nas:/share"}});
        QCOMPARE(r.locate(added.key, added.rootPath, added.mountSubpath), moved + "/music");

        // A share Omamp (Thrum's old name) marked: .omamp-volume-id is read,
        // so it is still found, and adding it again writes no new marker.
        QVERIFY(QFile::rename(moved + "/music/.thrum-volume-id", moved + "/music/.omamp-volume-id"));
        QCOMPARE(r.locate(added.key, added.rootPath, added.mountSubpath), moved + "/music");
        const VolumeInfo legacy = r.identify(moved + "/music", true);
        QCOMPARE(legacy.key, added.key);
        QVERIFY(!legacy.markerWritten);
        QVERIFY(!QFile::exists(moved + "/music/.thrum-volume-id"));
        fake.setMounts({});
        QVERIFY(r.locate(added.key, added.rootPath, added.mountSubpath).isEmpty());
    }

    // ---- Tags --------------------------------------------------------------

    void tagValuesAreParsed() {
        QCOMPARE(TagReader::splitValues({"Neon Coast; Lena Park", "Lena Park"}),
                 (QStringList{"Neon Coast", "Lena Park"}));
        QCOMPARE(TagReader::splitValues({"Simon & Garfunkel"}), QStringList{"Simon & Garfunkel"});
        int n = 0, total = 0;
        TagReader::parsePair("3/12", &n, &total);
        QCOMPARE(n, 3);
        QCOMPARE(total, 12);
        QCOMPARE(TagReader::parseYear("1997-05-01"), 1997);
        QCOMPARE(TagReader::parseGain("-6.20 dB"), -6.2);
        QCOMPARE(TagReader::guessContentType("m4b", {}), QString("audiobook"));
        QCOMPARE(TagReader::guessContentType("flac", {"Classical"}), QString("classical"));
    }

    // ---- Scanning ----------------------------------------------------------

    void scanFillsTheLibrary() {
        if (!m_haveFixtures)
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString music = copyFixtures(dir);
        FakeMounts fake;
        fake.setMounts({{music, "/", "ext4", fake.device("sdc1", "FEED-0001")}});
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, fake.resolver());
        const ScanResult r = scanner.scan({music});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.files, 12);  // the hidden folder and the jpeg are skipped
        QCOMPARE(r.added, 12);
        QCOMPARE(r.failed, 1);  // broken.mp3, still listed by its file name
        QVERIFY(r.failures.first().endsWith("broken.mp3"));

        QCOMPARE(one(library, "SELECT count(*) FROM tracks").toInt(), 12);
        QCOMPARE(one(library, "SELECT key FROM volumes").toString(), QString("uuid:FEED-0001"));
        QCOMPARE(one(library, "SELECT rel_path FROM tracks WHERE title = 'Afterglow'").toString(),
                 QString("Neon Coast/Night Drive/2-01 Afterglow.flac"));
        QCOMPARE(one(library, "SELECT disc_no || '/' || disc_total || ' ' || track_no FROM tracks WHERE title = 'Afterglow'")
                     .toString(),
                 QString("2/2 1"));
        QCOMPARE(one(library, "SELECT format FROM tracks WHERE title = 'Tidal'").toString(), QString("ogg"));
        QVERIFY(one(library, "SELECT duration_ms FROM tracks WHERE title = 'Coastline'").toInt() >= 1900);
        QCOMPARE(one(library, "SELECT content_type FROM tracks WHERE file_name = 'Chapter One'").toString(),
                 QString("audiobook"));
        QCOMPARE(one(library, "SELECT title FROM tracks WHERE rel_path = '日本語/夜の歌.m4a'").toString(), QString("夜の歌"));
        QCOMPARE(one(library, "SELECT artist FROM tracks WHERE title = 'Crüe Song'").toString(), QString("Mötley"));
        QVERIFY(one(library, "SELECT title FROM tracks WHERE file_name = 'untagged track'").isNull());

        // One album per release: two discs are one album, and a compilation's
        // tracks by different artists are one album under Various Artists.
        QCOMPARE(one(library, "SELECT count(DISTINCT album_id) FROM tracks WHERE album = 'Night Drive'").toInt(), 1);
        QCOMPARE(one(library, "SELECT count(DISTINCT album_id) FROM tracks WHERE album = 'Summer Mix'").toInt(), 1);
        QCOMPARE(one(library, "SELECT album_artist FROM albums WHERE title = 'Summer Mix'").toString(),
                 QString("Various Artists"));
        QCOMPARE(one(library, "SELECT count(*) FROM albums").toInt(), 3);
        QCOMPARE(one(library, "SELECT is_root FROM folders WHERE rel_path = ''").toInt(), 1);
        QCOMPARE(one(library, "SELECT state FROM jobs").toString(), QString("done"));
    }

    void rescanSkipsUnchangedFiles() {
        if (!m_haveFixtures)
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString music = copyFixtures(dir);
        FakeMounts fake;
        fake.setMounts({{music, "/", "ext4", fake.device("sdc1", "FEED-0002")}});
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, fake.resolver());
        QVERIFY(scanner.scan({music}).ok);

        // If the rescan opened the file it would put the real title back.
        QSqlQuery(library.db()).exec("UPDATE tracks SET title = 'Changed In DB', rating = 80 WHERE title = 'Afterglow'");
        ScanResult again = scanner.rescanAll();
        QVERIFY2(again.ok, qPrintable(again.error));
        QCOMPARE(again.unchanged, 12);
        QCOMPARE(again.added + again.updated + again.missing, 0);
        QCOMPARE(titles(library, "title:changed"), QStringList{"Changed In DB"});

        // A new modification time makes it read the file again; the rating,
        // which lives only in the database, survives.
        QFile file(music + "/Neon Coast/Night Drive/2-01 Afterglow.flac");
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-60), QFileDevice::FileModificationTime));
        file.close();
        again = scanner.rescanAll();
        QCOMPARE(again.updated, 1);
        QCOMPARE(again.unchanged, 11);
        QCOMPARE(one(library, "SELECT rating FROM tracks WHERE title = 'Afterglow'").toInt(), 80);
    }

    void goneFilesAreMissingAndUnpluggedDisksOffline() {
        if (!m_haveFixtures)
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString music = copyFixtures(dir);
        FakeMounts fake;
        const QString dev = fake.device("sdc1", "FEED-0003");
        fake.setMounts({{music, "/", "ext4", dev}});
        const VolumeResolver resolver = fake.resolver();
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, resolver);
        QVERIFY(scanner.scan({music}).ok);

        // A deleted file on a disk that is there is missing, and only that one.
        QVERIFY(QFile::remove(music + "/Loose/title only.opus"));
        ScanResult r = scanner.rescanAll();
        QCOMPARE(r.missing, 1);
        QCOMPARE(titles(library, "status:missing"), QStringList{"Only A Title"});
        QCOMPARE(titles(library, "status:online").size(), 11);

        // Unplugged: everything is offline, nothing is deleted or marked missing.
        fake.setMounts({});
        library.refreshVolumes(resolver);
        QCOMPARE(titles(library, "status:offline").size(), 11);
        QCOMPARE(titles(library, "status:missing"), QStringList{"Only A Title"});
        QCOMPARE(one(library, "SELECT count(*) FROM tracks").toInt(), 12);
        QVERIFY(library.absolutePath(one(library, "SELECT id FROM tracks WHERE title = 'Tidal'").toLongLong()).isEmpty());
        // Rescanning with the disk away fails instead of marking it all missing.
        r = scanner.rescanAll();
        QVERIFY(!r.ok);
        QCOMPARE(titles(library, "status:offline").size(), 11);

        // Plugged back in at another place: online again, paths follow the mount.
        QTemporaryDir other;
        const QString moved = QFileInfo(other.path()).canonicalFilePath() + "/music";
        QVERIFY(QDir().rename(music, moved));
        fake.setMounts({{moved, "/", "ext4", dev}});
        library.refreshVolumes(resolver);
        QCOMPARE(titles(library, "status:online").size(), 11);
        QCOMPARE(library.absolutePath(one(library, "SELECT id FROM tracks WHERE title = 'Tidal'").toLongLong()),
                 moved + "/Compilations/Summer Mix/02 Tidal.ogg");
        r = scanner.rescanAll();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.unchanged, 11);
        QCOMPARE(r.added, 0);
    }

    void multiValueArtistsAndGenres() {
        if (!m_haveFixtures)
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString music = copyFixtures(dir);
        FakeMounts fake;
        fake.setMounts({{music, "/", "ext4", fake.device("sdc1", "FEED-0004")}});
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QVERIFY(Scanner(library, fake.resolver()).scan({music}).ok);

        QCOMPARE(one(library, "SELECT count(*) FROM track_artists ta JOIN tracks t ON t.id = ta.track_id "
                              "WHERE t.title = 'Coastline' AND ta.role = 'artist'").toInt(),
                 2);
        QCOMPARE(titles(library, "artist:=\"Lena Park\""), QStringList{"Coastline"});
        QCOMPARE(titles(library, "artist:=\"Neon Coast\"").size(), 3);
        QCOMPARE(titles(library, "albumartist:=\"Neon Coast\"").size(), 3);
        QCOMPARE(titles(library, "composer:=\"Ada Moreau\""), QStringList{"Midnight Highway"});
        QCOMPARE(titles(library, "genre:=metal"), QStringList{"Heatwave"});
        QCOMPARE(titles(library, "genre:=rock"), QStringList{"Heatwave"});
        QCOMPARE(one(library, "SELECT count(*) FROM artists WHERE name = 'Lena Park'").toInt(), 1);
    }

    // ---- Search and the query language -------------------------------------

    void searchFindsWordsInsideWords() {
        if (!m_haveFixtures)
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString music = copyFixtures(dir);
        FakeMounts fake;
        fake.setMounts({{music, "/", "ext4", fake.device("sdc1", "FEED-0005")}});
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QVERIFY(Scanner(library, fake.resolver()).scan({music}).ok);

        QCOMPARE(titles(library, "neon").size(), 3);
        QCOMPARE(titles(library, "NEON highway"), QStringList{"Midnight Highway"});
        QCOMPARE(titles(library, "oast").size(), 3);                     // inside "Coast"
        QCOMPARE(titles(library, "bjork"), QStringList{"Tidal"});         // accents ignored
        QCOMPARE(titles(library, "夜の歌"), QStringList{"夜の歌"});
        QCOMPARE(titles(library, "untagged").size(), 1);                  // file names are searched too
        QCOMPARE(titles(library, "\"night drive\"").size(), 3);
        QCOMPARE(titles(library, "dr").size(), 3);                        // too short for the index
        QCOMPARE(titles(library, "year:1997..2001").size(), 6);
        QCOMPARE(titles(library, "year:>2000"), (QStringList{"Heatwave", "Snow Day", "Tidal"}));
        QCOMPARE(titles(library, "summer -genre:christmas"), (QStringList{"Heatwave", "Tidal"}));
        QCOMPARE(titles(library, "genre:rock OR genre:pop summer"), (QStringList{"Heatwave", "Tidal"}));
        QCOMPARE(titles(library, "title:/^(after|coast)/"), (QStringList{"Afterglow", "Coastline"}));
        QCOMPARE(titles(library, "length:2"), QStringList{"Coastline"});
        QCOMPARE(titles(library, "type:audiobook"), QStringList{"Chapter One"});
        QCOMPARE(titles(library, "format:flac").size(), 3);
        QCOMPARE(titles(library, "").size(), 12);
    }

    void queriesParseIntoATree() {
        Query::Parsed p = Query::parse("rating:4.. genre:rock OR genre:metal -live");
        QVERIFY(p.ok());
        QCOMPARE(p.root.kind, Query::Node::And);
        QCOMPARE(p.root.children.size(), 3);
        QCOMPARE(p.root.children[0].op, QString(".."));
        QCOMPARE(p.root.children[0].value, QString("4"));
        QCOMPARE(p.root.children[1].kind, Query::Node::Or); // OR binds tighter than AND
        QCOMPARE(p.root.children[2].kind, Query::Node::Not);

        p = Query::parse("artist:\"neon coast\" (a OR b)");
        QVERIFY(p.ok());
        QCOMPARE(p.root.children[0].value, QString("neon coast"));
        QVERIFY(p.root.children[0].quoted);

        QVERIFY(!Query::parse("(a b").ok());
        QVERIFY(!Query::parse("a)").ok());
        QVERIFY(!Query::parse("a OR").ok());
        QVERIFY(!Query::parse("\"open").ok());
        QVERIFY(!Query::parse("year:").ok());

        QVERIFY(!Query::compile("nosuchfield:1").ok());
        QVERIFY(!Query::compile("year:abc").ok());
        QVERIFY(!Query::compile("type:polka").ok());
        const Query::Compiled c = Query::compile("rating:4.. year:1990..1999");
        QVERIFY(c.ok());
        QCOMPARE(c.params, (QVariantList{80.0, 1990.0, 1999.0}));
    }

    // ---- The command line --------------------------------------------------

    void cliPrintsJson() {
        if (!m_haveFixtures)
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        const QString music = copyFixtures(dir);
        const QString db = dir.filePath("library.db");
        FakeMounts fake;
        fake.setMounts({{music, "/", "ext4", fake.device("sdc1", "FEED-0006")}});
        const VolumeResolver resolver = fake.resolver();

        auto cli = [&](const QStringList &args, QJsonObject *json, QString *errors = nullptr) {
            QString outText, errText;
            QTextStream out(&outText), err(&errText);
            const int code = Cli::run(args, out, err, &resolver);
            out.flush();
            err.flush();
            if (json)
                *json = QJsonDocument::fromJson(outText.toUtf8()).object();
            if (errors)
                *errors = errText;
            return code;
        };

        QJsonObject json;
        QCOMPARE(cli({"scan", "--json", "--db", db, music}, &json), 0);
        QCOMPARE(json["added"].toInt(), 12);
        QCOMPARE(json["roots"].toArray().first().toString(), music);
        QCOMPARE(cli({"scan", "--json", "--db", db}, &json), 0);
        QCOMPARE(json["unchanged"].toInt(), 12);

        QCOMPARE(cli({"query", "--json", "--db", db, "artist:\"neon coast\"", "disc:1"}, &json), 0);
        QCOMPARE(json["total"].toInt(), 2);
        const QJsonObject first = json["tracks"].toArray().first().toObject();
        QCOMPARE(first["title"].toString(), QString("Midnight Highway"));
        QCOMPARE(first["year"].toInt(), 1997);
        QCOMPARE(first["path"].toString(), music + "/Neon Coast/Night Drive/1-01 Midnight Highway.flac");
        QCOMPARE(cli({"query", "--json", "--limit", "1", "--db", db, "neon"}, &json), 0);
        QCOMPARE(json["count"].toInt(), 1);
        QCOMPARE(json["total"].toInt(), 3);

        QCOMPARE(cli({"stats", "--json", "--db", db}, &json), 0);
        QCOMPARE(json["tracks"].toInt(), 12);
        QCOMPARE(json["albums"].toInt(), 3);
        QCOMPARE(json["schema_version"].toInt(), Library::latestVersion());

        QString errors;
        QCOMPARE(cli({"query", "--json", "--db", db, "year:soon"}, &json, &errors), 1);
        QVERIFY(errors.contains("needs a number"));
        QCOMPARE(json["ok"].toBool(), false);
        QCOMPARE(cli({"scan", "--db", db, dir.filePath("nowhere")}, nullptr, &errors), 1);
        QVERIFY(errors.contains("not a folder"));
        QVERIFY(Cli::isCommand("scan"));
        QVERIFY(!Cli::isCommand("/home/you/Music"));
    }
};

int runLibraryTests(int argc, char **argv) {
    LibraryTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "library_tests.moc"
