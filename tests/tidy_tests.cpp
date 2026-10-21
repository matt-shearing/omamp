// Phase 2 backend: tag writing with its journal and undo, organising files
// by a mask, the Files to Edit audits, the duplicate finder and their
// commands. Every library, music file and picture lives in a temporary
// folder; the tag writer and organiser are fenced into it, and the tests
// check every path they report. Nothing is played.

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <sys/stat.h>

#include "audit.h"
#include "cli.h"
#include "duplicates.h"
#include "fixture_library.h"
#include "job.h"
#include "library.h"
#include "organise.h"
#include "scanner.h"
#include "tagreader.h"
#include "tagwriter.h"

namespace {

QVariant value(Library &library, const QString &sql) {
    QSqlQuery q(library.db());
    if (!q.exec(sql) || !q.next())
        return {};
    return q.value(0);
}

bool run(const QString &program, const QStringList &args) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        qWarning() << program << args << p.readAllStandardError();
        return false;
    }
    return true;
}

// A quiet tone, as in make-fixtures; extra holds -metadata and codec options.
bool tone(const QString &file, const QStringList &extra, double seconds = 1) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds),
                        "-af", "volume=0.05", "-map_metadata", "-1"};
    return run("ffmpeg", args + extra + QStringList{file});
}

// Quiet pink noise: different seeds sound different to Chromaprint.
bool noise(const QString &file, int seed, const QStringList &extra) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("anoisesrc=d=10:c=pink:r=22050:a=0.05:seed=%1").arg(seed), "-map_metadata", "-1"};
    return run("ffmpeg", args + extra + QStringList{file});
}

QByteArray png(const QColor &colour) {
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray bytesOf(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

qint64 mtimeNs(const QString &path) {
    struct stat st {};
    ::stat(QFile::encodeName(path).constData(), &st);
    return qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

// Every file under a folder (hidden ones too), relative, with a hash of its
// bytes: a picture of the tree to compare before and after.
QMap<QString, QByteArray> tree(const QString &root) {
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        out.insert(path.mid(root.size() + 1), QCryptographicHash::hash(bytesOf(path), QCryptographicHash::Sha1));
    }
    return out;
}

QStringList temporaries(const QString &root) {
    QStringList out;
    for (const QString &rel : tree(root).keys())
        if (QFileInfo(rel).fileName().startsWith(".thrum-"))
            out << rel;
    return out;
}

// Fails the test if any absolute path anywhere in a JSON value lies outside
// root: the paranoid check that nothing strayed from the temporary folder.
bool inside(const QString &root, const QJsonValue &v, QString *stray) {
    if (v.isString()) {
        const QString s = v.toString();
        if (s.startsWith('/') && !(s == root || s.startsWith(root + '/'))) {
            *stray = s;
            return false;
        }
        return true;
    }
    if (v.isArray()) {
        for (const QJsonValue &x : v.toArray())
            if (!inside(root, x, stray))
                return false;
        return true;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!inside(root, it.value(), stray))
                return false;
    }
    return true;
}

#define VERIFY_INSIDE(root, json)                                                                                      \
    do {                                                                                                               \
        QString stray;                                                                                                 \
        QVERIFY2(inside(root, json, &stray), qPrintable("outside the test folder: " + stray));                        \
    } while (0)

// A temporary folder with a music folder and a library, scanned.
struct Sandbox {
    QTemporaryDir dir;
    QString root, music;
    std::unique_ptr<Library> library;

    Sandbox() {
        root = QFileInfo(dir.path()).canonicalFilePath();
        music = root + "/music";
        QDir().mkpath(music);
    }
    bool open() {
        library = std::make_unique<Library>(root + "/library.db");
        return library->open();
    }
    ScanResult scan() { return Scanner(*library, VolumeResolver()).scan({music}); }
    qint64 id(const QString &rel) {
        QSqlQuery q(library->db());
        q.prepare("SELECT id FROM tracks WHERE rel_path LIKE ?");
        q.addBindValue("%" + rel);
        return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
    }
    QString path(qint64 id) { return library->absolutePath(id); }
};

int cli(const QStringList &args, QJsonObject *json, QString *errors = nullptr) {
    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    const int code = Cli::run(args, out, err, nullptr);
    out.flush();
    err.flush();
    if (json)
        *json = QJsonDocument::fromJson(outText.toUtf8()).object();
    if (errors)
        *errors = errText + outText;
    return code;
}

}

class TidyTests : public QObject {
    Q_OBJECT

private slots:
    void tagRoundTrip_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QStringList>("codec");
        QTest::newRow("mp3") << "a.mp3" << QStringList{"-c:a", "libmp3lame", "-b:a", "64k"};
        QTest::newRow("flac") << "a.flac" << QStringList{};
        QTest::newRow("ogg") << "a.ogg" << QStringList{"-c:a", "libvorbis"};
        QTest::newRow("opus") << "a.opus" << QStringList{"-c:a", "libopus", "-b:a", "32k"};
        QTest::newRow("m4a") << "a.m4a" << QStringList{"-c:a", "aac", "-b:a", "64k"};
    }

    void tagRoundTrip() {
        QFETCH(QString, file);
        QFETCH(QStringList, codec);
        Sandbox s;
        const QString path = s.music + "/x/" + file;
        QVERIFY(tone(path, codec + QStringList{"-metadata", "title=Old Title", "-metadata", "artist=Old Artist",
                                               "-metadata", "genre=Old"}));
        QVERIFY(s.open());
        QCOMPARE(s.scan().added, 1);
        const qint64 id = s.id(file);
        QVERIFY(id);

        const QByteArray picture = png(Qt::red);
        const QList<TagEdit> edits = {
            {id, "title", "New Title", {}, {}},
            {id, "artist", "Neon Coast; Lena Park", {}, {}},
            {id, "Album Artist", "Neon Coast", {}, {}},
            {id, "album", "Night Drive", {}, {}},
            {id, "genre", "Synthwave; Pop", {}, {}},
            {id, "composer", "Ada Moreau", {}, {}},
            {id, "year", "1997", {}, {}},
            {id, "track", "3", {}, {}},
            {id, "tracktotal", "12", {}, {}},
            {id, "disc", "1", {}, {}},
            {id, "disctotal", "2", {}, {}},
            {id, "comment", "A comment", {}, {}},
            {id, "bpm", "118", {}, {}},
            {id, "lyrics", "line one\nline two", {}, {}},
            {id, "rating", "80", {}, {}},
            {id, "artwork", {}, picture, "image/png"},
        };
        TagWriter writer(*s.library);
        writer.setSandbox(s.root);
        TagWriteOptions options;
        options.writeRatingsToFiles = 1;
        const TagBatchResult r = writer.apply(edits, options);
        VERIFY_INSIDE(s.root, r.toJson());
        QVERIFY2(r.ok, qPrintable(QJsonDocument(r.toJson()).toJson()));
        QCOMPARE(r.changed, 1);
        QVERIFY(!r.batch.isEmpty());
        QVERIFY(r.files[0].fileWritten);
        QVERIFY(temporaries(s.root).isEmpty());

        // The file says what was written.
        QString error;
        const QMap<QString, QString> v = TagWriter::readFile(path, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(v["title"], QString("New Title"));
        QCOMPARE(v["artist"], QString("Neon Coast; Lena Park"));
        QCOMPARE(v["albumartist"], QString("Neon Coast"));
        QCOMPARE(v["album"], QString("Night Drive"));
        QCOMPARE(v["genre"], QString("Synthwave; Pop"));
        QCOMPARE(v["composer"], QString("Ada Moreau"));
        QCOMPARE(v["year"], QString("1997"));
        QCOMPARE(v["track"], QString("3"));
        QCOMPARE(v["tracktotal"], QString("12"));
        QCOMPARE(v["disc"], QString("1"));
        QCOMPARE(v["disctotal"], QString("2"));
        QCOMPARE(v["comment"], QString("A comment"));
        QCOMPARE(v["bpm"], QString("118"));
        QCOMPARE(v["lyrics"], QString("line one\nline two"));
        const TrackTags tags = TagReader::read(path);
        QVERIFY(tags.ok);
        QCOMPARE(tags.tagRating, 80);
        QVERIFY(tags.hasEmbeddedArt);
        QVERIFY(tags.hasLyrics);
        QCOMPARE(TagReader::embeddedPicture(path), picture);

        // The library row matches the file, size and time included, so a
        // rescan finds nothing to do.
        QCOMPARE(value(*s.library, QString("SELECT title FROM tracks WHERE id = %1").arg(id)).toString(),
                 QString("New Title"));
        QCOMPARE(value(*s.library, QString("SELECT rating FROM tracks WHERE id = %1").arg(id)).toInt(), 80);
        QCOMPARE(value(*s.library, QString("SELECT count(*) FROM track_artists WHERE track_id = %1 AND role = 'artist'")
                                       .arg(id))
                     .toInt(),
                 2);
        QCOMPARE(value(*s.library, QString("SELECT size FROM tracks WHERE id = %1").arg(id)).toLongLong(),
                 QFileInfo(path).size());
        QCOMPARE(value(*s.library, QString("SELECT mtime_ns FROM tracks WHERE id = %1").arg(id)).toLongLong(),
                 mtimeNs(path));
        QCOMPARE(value(*s.library, QString("SELECT count(*) FROM tracks_fts WHERE tracks_fts MATCH 'night'")).toInt(), 1);
        const ScanResult again = s.scan();
        QCOMPARE(again.unchanged, 1);
        QCOMPARE(again.updated, 0);

        // Every change is journalled, with the old value.
        QCOMPARE(value(*s.library, QString("SELECT old_value FROM tag_journal WHERE batch = '%1' AND field = 'title'")
                                       .arg(r.batch))
                     .toString(),
                 QString("Old Title"));
        QCOMPARE(value(*s.library, QString("SELECT count(*) FROM tag_journal WHERE batch = '%1'").arg(r.batch)).toInt(),
                 16);

        // Writing the same again changes nothing and writes nothing.
        const qint64 before = mtimeNs(path);
        const TagBatchResult same = writer.apply(edits, options);
        QCOMPARE(same.unchanged, 1);
        QVERIFY(same.batch.isEmpty());
        QCOMPARE(mtimeNs(path), before);

        // Removing the picture.
        const TagBatchResult removed = writer.apply({{id, "artwork", {}, {}, {}}}, options);
        QVERIFY(removed.ok);
        QVERIFY(TagReader::embeddedPicture(path).isEmpty());
        QCOMPARE(value(*s.library, QString("SELECT has_embedded_art FROM tracks WHERE id = %1").arg(id)).toInt(), 0);
    }

    void ratingsStayInTheDatabase() {
        Sandbox s;
        const QString path = s.music + "/a.flac";
        QVERIFY(tone(path, {"-metadata", "title=A"}));
        QVERIFY(s.open());
        s.scan();
        const qint64 id = s.id("a.flac");
        const QByteArray original = bytesOf(path);
        TagWriter writer(*s.library);
        writer.setSandbox(s.root);
        const TagBatchResult r = writer.apply({{id, "rating", "60", {}, {}}, {id, "custom2", "vinyl rip", {}, {}}});
        QVERIFY(r.ok);
        QCOMPARE(r.changed, 1);
        QVERIFY(!r.files[0].fileWritten);
        QCOMPARE(bytesOf(path), original);
        QCOMPARE(value(*s.library, QString("SELECT rating FROM tracks WHERE id = %1").arg(id)).toInt(), 60);
        QCOMPARE(value(*s.library, QString("SELECT custom2 FROM tracks WHERE id = %1").arg(id)).toString(),
                 QString("vinyl rip"));
        // The setting turns file writes on.
        s.library->setSetting("tags.write_ratings", "1");
        QVERIFY(writer.apply({{id, "rating", "100", {}, {}}}).ok);
        QCOMPARE(TagReader::read(path).tagRating, 100);
        // And both undo.
        QVERIFY(writer.undo(Journal::lastUndoable(*s.library)).ok);
        QCOMPARE(value(*s.library, QString("SELECT rating FROM tracks WHERE id = %1").arg(id)).toInt(), 60);
    }

    void dryRunWritesNothing() {
        Sandbox s;
        QVERIFY(tone(s.music + "/a.flac", {"-metadata", "title=A"}));
        QVERIFY(s.open());
        s.scan();
        const auto before = tree(s.root + "/music");
        TagWriteOptions options;
        options.dryRun = true;
        const TagBatchResult r = TagWriter(*s.library).apply({{s.id("a.flac"), "title", "B", {}, {}}}, options);
        QVERIFY(r.ok);
        QCOMPARE(r.changed, 1);
        QCOMPARE(r.files[0].changes[0].oldValue, QString("A"));
        QVERIFY(r.batch.isEmpty());
        QCOMPARE(tree(s.root + "/music"), before);
        QCOMPARE(value(*s.library, "SELECT count(*) FROM tag_journal").toInt(), 0);
        // An unknown field fails the whole batch before anything happens.
        const TagBatchResult bad = TagWriter(*s.library).apply({{s.id("a.flac"), "colour", "B", {}, {}}});
        QVERIFY(!bad.ok);
        QVERIFY(bad.error.contains("Unknown field"));
    }

    void failureMidBatchLeavesNoHalfWrittenFile() {
        Sandbox s;
        for (const char *name : {"1.flac", "2.flac", "3.flac"})
            QVERIFY(tone(s.music + "/" + name, {"-metadata", QString("title=T%1").arg(name[0])}));
        QVERIFY(s.open());
        s.scan();
        const QString second = s.music + "/2.flac";
        const QByteArray original = bytesOf(second);
        TagWriter writer(*s.library);
        writer.setSandbox(s.root);
        int calls = 0;
        // The second file "loses its connection" after its copy is written.
        bool copyWasThere = false;
        writer.setBeforeReplace([&](const QString &temporary, const QString &target) {
            ++calls;
            if (target != second)
                return true;
            copyWasThere = QFileInfo::exists(temporary);
            return false;
        });
        QList<TagEdit> edits;
        for (const char *name : {"1.flac", "2.flac", "3.flac"})
            edits << TagEdit{s.id(name), "title", "Changed", {}, {}};
        const TagBatchResult r = writer.apply(edits);
        VERIFY_INSIDE(s.root, r.toJson());
        QCOMPARE(calls, 3);
        QVERIFY(copyWasThere);
        QVERIFY(!r.ok);
        QCOMPARE(r.changed, 2);
        QCOMPARE(r.failed, 1);
        QVERIFY(r.files[1].error.contains("original is unchanged"));
        QCOMPARE(bytesOf(second), original);
        QVERIFY(temporaries(s.root).isEmpty());
        QCOMPARE(TagWriter::readFile(s.music + "/1.flac")["title"], QString("Changed"));
        QCOMPARE(TagWriter::readFile(s.music + "/3.flac")["title"], QString("Changed"));
        QCOMPARE(value(*s.library, QString("SELECT title FROM tracks WHERE id = %1").arg(s.id("2.flac"))).toString(),
                 QString("T2"));
        QCOMPARE(value(*s.library, QString("SELECT count(*) FROM tag_journal WHERE track_id = %1").arg(s.id("2.flac")))
                     .toInt(),
                 0);

        // A file changed by something else during the write is left alone.
        writer.setBeforeReplace([&](const QString &, const QString &target) {
            QFile f(target);
            f.open(QIODevice::Append);
            f.write("x");
            return true;
        });
        const QByteArray third = bytesOf(s.music + "/3.flac");
        const TagBatchResult raced = writer.apply({{s.id("3.flac"), "title", "Raced", {}, {}}});
        QVERIFY(!raced.ok);
        QVERIFY(raced.files[0].error.contains("Something else changed"));
        QCOMPARE(bytesOf(s.music + "/3.flac"), third + "x");
        QVERIFY(temporaries(s.root).isEmpty());
    }

    void undoRestoresFilesAndDatabase() {
        Sandbox s;
        QVERIFY(tone(s.music + "/a.flac", {"-metadata", "title=Alpha", "-metadata", "genre=Rock"}));
        QVERIFY(tone(s.music + "/b.mp3", {"-c:a", "libmp3lame", "-b:a", "64k", "-metadata", "title=Beta",
                                          "-metadata", "genre=Pop"}));
        QVERIFY(s.open());
        s.scan();
        const qint64 a = s.id("a.flac"), b = s.id("b.mp3");
        TagWriter writer(*s.library);
        writer.setSandbox(s.root);
        const QByteArray red = png(Qt::red), green = png(Qt::green);
        const TagBatchResult first = writer.apply({{a, "artwork", {}, red, {}}});
        QVERIFY(first.ok);
        const TagBatchResult second = writer.apply({{a, "title", "New", {}, {}},
                                                    {a, "artwork", {}, green, {}},
                                                    {b, "title", "New", {}, {}},
                                                    {b, "genre", "Jazz; Blues", {}, {}}});
        QVERIFY(second.ok);
        QCOMPARE(TagReader::embeddedPicture(s.music + "/a.flac"), green);
        QCOMPARE(Journal::lastUndoable(*s.library), second.batch);

        const QJsonObject undone = undoBatch(*s.library, second.batch);
        VERIFY_INSIDE(s.root, undone);
        QVERIFY(undone.value("ok").toBool());
        QCOMPARE(undone.value("kind").toString(), QString("tag"));
        QCOMPARE(TagWriter::readFile(s.music + "/a.flac")["title"], QString("Alpha"));
        QCOMPARE(TagReader::embeddedPicture(s.music + "/a.flac"), red);
        QCOMPARE(TagWriter::readFile(s.music + "/b.mp3")["title"], QString("Beta"));
        QCOMPARE(TagWriter::readFile(s.music + "/b.mp3")["genre"], QString("Pop"));
        QCOMPARE(value(*s.library, QString("SELECT title FROM tracks WHERE id = %1").arg(b)).toString(), QString("Beta"));
        QCOMPARE(value(*s.library, QString("SELECT genre FROM tracks WHERE id = %1").arg(b)).toString(), QString("Pop"));
        QCOMPARE(value(*s.library,
                       QString("SELECT count(*) FROM tag_journal WHERE batch = '%1' AND undone = 0").arg(second.batch))
                     .toInt(),
                 0);
        // The undo is journalled; the next undo skips it and reaches the first batch.
        const QString undoId = undone.value("batch").toString();
        QVERIFY(undoId.startsWith("undo-"));
        QCOMPARE(value(*s.library, QString("SELECT min(source) FROM tag_journal WHERE batch = '%1'").arg(undoId)).toString(),
                 "undo:" + second.batch);
        QCOMPARE(Journal::lastUndoable(*s.library), first.batch);
        QCOMPARE(Journal::batches(*s.library).size(), 3);
        // Undoing the undo is a redo.
        QVERIFY(undoBatch(*s.library, undoId).value("ok").toBool());
        QCOMPARE(TagWriter::readFile(s.music + "/b.mp3")["genre"], QString("Jazz; Blues"));
        QCOMPARE(TagReader::embeddedPicture(s.music + "/a.flac"), green);
        // A batch that is fully undone has nothing left.
        QVERIFY(!undoBatch(*s.library, second.batch).value("ok").toBool());
        QVERIFY(temporaries(s.root).isEmpty());
    }

    void sandboxRefusesOtherFolders() {
        Sandbox s;
        QVERIFY(tone(s.music + "/a.flac", {"-metadata", "title=A"}));
        QVERIFY(s.open());
        s.scan();
        TagWriter writer(*s.library);
        writer.setSandbox(s.root + "/elsewhere");
        const QByteArray original = bytesOf(s.music + "/a.flac");
        const TagBatchResult r = writer.apply({{s.id("a.flac"), "title", "B", {}, {}}});
        QVERIFY(!r.ok);
        QVERIFY(r.files[0].error.contains("Outside"));
        QCOMPARE(bytesOf(s.music + "/a.flac"), original);
    }

    void organisePlanApplyUndo() {
        Sandbox s;
        const QStringList album = {"-metadata", "album=Night Drive", "-metadata", "album_artist=Neon Coast",
                                   "-metadata", "artist=Neon Coast"};
        QVERIFY(tone(s.music + "/inbox/a.flac", album + QStringList{"-metadata", "title=Alpha", "-metadata", "track=1"}));
        QVERIFY(tone(s.music + "/inbox/b.flac", album + QStringList{"-metadata", "title=Beta", "-metadata", "track=2"}));
        QVERIFY(write(s.music + "/inbox/a.lrc", "[00:00.00]la la\n"));
        QVERIFY(write(s.music + "/inbox/cover.jpg", "jpeg"));
        QVERIFY(tone(s.music + "/other/c.flac", album + QStringList{"-metadata", "title=Alpha", "-metadata", "track=1"}));
        // Already in its place, and in the way of b.
        QVERIFY(tone(s.music + "/Neon Coast/Night Drive/02 Beta.flac",
                     album + QStringList{"-metadata", "title=Beta", "-metadata", "track=2"}, 2));
        QVERIFY(s.open());
        QCOMPARE(s.scan().added, 4);
        const qint64 a = s.id("inbox/a.flac"), b = s.id("inbox/b.flac"), c = s.id("other/c.flac");
        const qint64 inPlace = s.id("Night Drive/02 Beta.flac");
        QSqlQuery q(s.library->db());
        q.exec(QString("UPDATE tracks SET rating = 100, play_count = 7 WHERE id = %1").arg(a));
        q.exec("INSERT INTO playlists (id, name, created) VALUES (1, 'Mix', 0)");
        q.exec(QString("INSERT INTO playlist_items (playlist_id, track_id, position) VALUES (1, %1, 0)").arg(a));
        const QByteArray conflict = bytesOf(s.music + "/Neon Coast/Night Drive/02 Beta.flac");

        Organiser organiser(*s.library);
        organiser.setSandbox(s.root);
        OrganiseOptions options;
        options.mask = "<Album Artist>/<Album>/<Track#:2> <Title>";
        const auto before = tree(s.root + "/music");
        const OrganisePlan plan = organiser.plan({a, b, c, inPlace}, options);
        VERIFY_INSIDE(s.root, plan.toJson());
        QVERIFY(plan.ok);
        QCOMPARE(tree(s.root + "/music"), before); // planning touches nothing
        const QString target = s.music + "/Neon Coast/Night Drive/";
        QCOMPARE(plan.items[0].newPath, target + "01 Alpha.flac");
        QCOMPARE(plan.items[0].status, QString("move"));
        QCOMPARE(plan.items[1].newPath, target + "02 Beta (2).flac");
        QCOMPARE(plan.items[1].status, QString("renamed"));
        QCOMPARE(plan.items[2].newPath, target + "01 Alpha (2).flac");
        QCOMPARE(plan.items[2].status, QString("renamed"));
        QCOMPARE(plan.items[3].status, QString("unchanged"));
        QCOMPARE(plan.moves, 3);
        QCOMPARE(plan.renamed, 2);
        QCOMPARE(plan.unchanged, 1);
        QCOMPARE(plan.items[0].companions.size(), 1);
        QCOMPARE(plan.items[0].companions[0].to, target + "01 Alpha.lrc");
        QCOMPARE(plan.folderCompanions.size(), 1);
        QCOMPARE(plan.folderCompanions[0].to, target + "cover.jpg");

        const OrganiseResult r = organiser.apply(plan, options);
        VERIFY_INSIDE(s.root, r.toJson());
        QVERIFY2(r.ok, qPrintable(QJsonDocument(r.toJson()).toJson()));
        QCOMPARE(r.moved, 3);
        QCOMPARE(r.unchanged, 1);
        QCOMPARE(r.copied, 0);
        QCOMPARE(r.companionsMoved, 2);
        QCOMPARE(r.foldersRemoved, 2);
        QVERIFY(!QDir(s.music + "/inbox").exists());
        QVERIFY(!QDir(s.music + "/other").exists());
        QVERIFY(QFileInfo::exists(target + "01 Alpha.flac"));
        QVERIFY(QFileInfo::exists(target + "01 Alpha.lrc"));
        QVERIFY(QFileInfo::exists(target + "cover.jpg"));
        QCOMPARE(bytesOf(target + "02 Beta.flac"), conflict); // never overwritten
        // The tracks kept their ids, ratings and playlist places.
        QCOMPARE(s.path(a), target + "01 Alpha.flac");
        QCOMPARE(s.path(c), target + "01 Alpha (2).flac");
        QCOMPARE(value(*s.library, QString("SELECT rating FROM tracks WHERE id = %1").arg(a)).toInt(), 100);
        QCOMPARE(value(*s.library, "SELECT track_id FROM playlist_items").toLongLong(), a);
        QCOMPARE(value(*s.library, QString("SELECT file_name FROM tracks WHERE id = %1").arg(c)).toString(),
                 QString("01 Alpha (2)"));
        QVERIFY(temporaries(s.root).isEmpty());
        // A rescan agrees with the database.
        const ScanResult again = s.scan();
        QCOMPARE(again.added, 0);
        QCOMPARE(again.missing, 0);
        QCOMPARE(again.unchanged, 4);
        // Running the same plan again moves nothing.
        QCOMPARE(organiser.plan({a, b, c, inPlace}, options).moves, 0);

        QVERIFY(Journal::isOrganise(*s.library, r.batch));
        QCOMPARE(Journal::lastUndoable(*s.library), r.batch);
        const QJsonObject undone = undoBatch(*s.library, r.batch);
        VERIFY_INSIDE(s.root, undone);
        QVERIFY2(undone.value("ok").toBool(), qPrintable(QJsonDocument(undone).toJson()));
        QCOMPARE(undone.value("kind").toString(), QString("organise"));
        QCOMPARE(tree(s.root + "/music"), before);
        QCOMPARE(s.path(a), s.music + "/inbox/a.flac");
        QCOMPARE(s.path(c), s.music + "/other/c.flac");
        const ScanResult after = s.scan();
        QCOMPARE(after.added, 0);
        QCOMPARE(after.missing, 0);
        QVERIFY(temporaries(s.root).isEmpty());
    }

    void organiseAcrossFilesystems() {
        Sandbox s;
        QVERIFY(tone(s.music + "/in/a.flac", {"-metadata", "title=Alpha", "-metadata", "artist=Neon Coast"}));
        QVERIFY(s.open());
        s.scan();
        const qint64 a = s.id("in/a.flac");
        const QByteArray original = bytesOf(s.music + "/in/a.flac");
        const qint64 time = mtimeNs(s.music + "/in/a.flac");
        Organiser organiser(*s.library);
        organiser.setSandbox(s.root);
        int asked = 0;
        organiser.setCrossDevice([&](const QString &, const QString &) {
            ++asked;
            return true;
        });
        OrganiseOptions options;
        options.mask = "<Artist>/<Title>";
        const OrganiseResult r = organiser.apply(organiser.plan({a}, options), options);
        VERIFY_INSIDE(s.root, r.toJson());
        QVERIFY(r.ok);
        QCOMPARE(r.copied, 1);
        QVERIFY(asked >= 1);
        const QString moved = s.music + "/Neon Coast/Alpha.flac";
        QCOMPARE(bytesOf(moved), original);
        QCOMPARE(mtimeNs(moved), time); // the copy keeps its time, as a rename would
        QVERIFY(!QFileInfo::exists(s.music + "/in/a.flac"));
        QVERIFY(temporaries(s.root).isEmpty());
        QCOMPARE(s.path(a), moved);
        QCOMPARE(s.scan().unchanged, 1);
        const QJsonObject undone = organiser.undo(r.batch).toJson(); // the same organiser, so the copy seam holds
        QVERIFY(undone.value("ok").toBool());
        QCOMPARE(undone.value("copied").toInt(), 1);
        QCOMPARE(bytesOf(s.music + "/in/a.flac"), original);
        QVERIFY(!QFileInfo::exists(moved));
        QVERIFY(!QDir(s.music + "/Neon Coast").exists());
    }

    void organiseNeverOverwrites() {
        Sandbox s;
        QVERIFY(tone(s.music + "/in/a.flac", {"-metadata", "title=Alpha"}));
        QVERIFY(tone(s.music + "/in/b.flac", {"-metadata", "title=Beta"}));
        QVERIFY(s.open());
        s.scan();
        Organiser organiser(*s.library);
        organiser.setSandbox(s.root);
        OrganiseOptions options;
        options.mask = "sorted/<Title>";
        const OrganisePlan plan = organiser.plan({s.id("in/a.flac"), s.id("in/b.flac")}, options);
        // Something takes the first name after the plan was made.
        QVERIFY(write(plan.items[0].newPath, "someone else's file"));
        const OrganiseResult r = organiser.apply(plan, options);
        QVERIFY(!r.ok);
        QCOMPARE(r.failed, 1);
        QCOMPARE(r.moved, 1);
        QVERIFY(r.items[0].note.contains("already exists"));
        QCOMPARE(bytesOf(plan.items[0].newPath), QByteArray("someone else's file"));
        QVERIFY(QFileInfo::exists(s.music + "/in/a.flac"));
        QCOMPARE(s.path(s.id("in/a.flac")), s.music + "/in/a.flac");
        // The folder still holds a.flac, so it stays.
        QVERIFY(QDir(s.music + "/in").exists());
    }

    void organiseDestinationAndNumbers() {
        Sandbox s;
        QVERIFY(tone(s.music + "/in/a.flac", {"-metadata", "title=Alpha"}));
        QVERIFY(tone(s.music + "/in/b.flac", {"-metadata", "title=Beta"}));
        QVERIFY(s.open());
        s.scan();
        Organiser organiser(*s.library);
        organiser.setSandbox(s.root);
        OrganiseOptions options;
        options.mask = "<Auto Number:2> $Upper(<Title>)";
        options.destination = s.music + "/flat";
        options.removeEmptyFolders = false;
        const OrganisePlan plan = organiser.plan({s.id("in/a.flac"), s.id("in/b.flac")}, options);
        QCOMPARE(plan.items[0].newPath, s.music + "/flat/01 ALPHA.flac");
        QCOMPARE(plan.items[1].newPath, s.music + "/flat/02 BETA.flac");
        const OrganiseResult r = organiser.apply(plan, options);
        QVERIFY(r.ok);
        QCOMPARE(r.foldersRemoved, 0);
        QVERIFY(QDir(s.music + "/in").exists());
        // A bad mask is refused with where the problem is.
        options.mask = "<Title>/<Albm>";
        const OrganisePlan bad = organiser.plan({s.id("ALPHA.flac")}, options);
        QVERIFY(!bad.ok);
        QCOMPARE(bad.errorPosition, 8);
        // Outside the sandbox nothing is planned.
        options.mask = "<Title>";
        options.destination = "/tmp/thrum-should-never-exist";
        const OrganisePlan outside = organiser.plan({s.id("ALPHA.flac")}, options);
        QCOMPARE(outside.items[0].status, QString("error"));
        QVERIFY(!organiser.apply(outside, options).items.isEmpty());
        QVERIFY(!QFileInfo::exists("/tmp/thrum-should-never-exist"));
        // Without a library folder above it, a track has nowhere to go but
        // the root of its disk, which is refused.
        QSqlQuery q(s.library->db());
        QVERIFY(q.exec("UPDATE folders SET is_root = 0"));
        options.destination.clear();
        const OrganisePlan rootless = organiser.plan({s.id("ALPHA.flac")}, options);
        QCOMPARE(rootless.items[0].status, QString("error"));
        QVERIFY(rootless.items[0].note.contains("library folder"));
    }

    void audits() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QVERIFY(Fixture::fill(library));
        auto count = [&](const char *id) { return Audits::count(library, id); };
        QCOMPARE(count("unknown-title"), 1);
        QCOMPARE(count("unknown-artist"), 1);
        QCOMPARE(count("unknown-album"), 1);
        QCOMPARE(count("unknown-genre"), 2);
        QCOMPARE(count("unknown-year"), 2);
        QCOMPARE(count("missing-artwork"), 12);
        QCOMPARE(count("unlevelled"), 12);
        QCOMPARE(count("no-musicbrainz"), 12);
        QCOMPARE(count("dead-links"), 0);
        QCOMPARE(count("offline"), 0);
        QCOMPARE(count("no-album-artist"), 0); // Summer Mix is a compilation
        QCOMPARE(count("mixed-year"), 3);      // Summer Mix: 2004, 2003, 2001
        QCOMPARE(count("mixed-genre"), 6);     // Night Drive and Summer Mix
        QCOMPARE(count("mixed-album-artist"), 0);
        QCOMPARE(count("no-track-number"), 0);
        QCOMPARE(count("track-gaps"), 0);

        QSqlQuery q(library.db());
        q.exec("UPDATE tracks SET availability = 'missing' WHERE title = 'Go West'");
        q.exec("UPDATE tracks SET availability = 'offline' WHERE title = 'Love Song'");
        q.exec("UPDATE tracks SET compilation = 0 WHERE album = 'Summer Mix'");
        q.exec("UPDATE tracks SET track_no = 5 WHERE title = 'Glove Box'");
        q.exec("UPDATE tracks SET track_no = NULL WHERE title = 'Coastline'");
        QVERIFY(q.exec("INSERT INTO folders (id, volume_id, rel_path, added) VALUES (1, 1, 'Night Drive', 0)"));
        QVERIFY(q.exec("UPDATE tracks SET folder_id = 1 WHERE album = 'Night Drive'"));
        QVERIFY(q.exec("UPDATE tracks SET album_artist = 'Someone' WHERE title = 'Afterglow'"));
        q.exec("UPDATE tracks SET rg_track_gain = -6.5, mb_track_id = 'x', has_embedded_art = 1 WHERE title = 'Tidal'");
        QCOMPARE(count("dead-links"), 1);
        QCOMPARE(count("offline"), 1);
        QCOMPARE(count("no-album-artist"), 3);
        // Live at Home skips 2 to 4. Coastline lost its number, which
        // no-track-number reports; the rest of its disc has no gap.
        QCOMPARE(count("track-gaps"), 2);
        QCOMPARE(count("no-track-number"), 1);
        QCOMPARE(count("mixed-album-artist"), 3);
        QCOMPARE(count("unlevelled"), 11);
        QCOMPARE(count("no-musicbrainz"), 11);
        QCOMPARE(count("missing-artwork"), 11);

        // Every check compiles, counts and lists the same tracks.
        for (const Audits::Count &c : Audits::counts(library)) {
            QVERIFY2(c.count >= 0, qPrintable(c.audit.id));
            QCOMPARE(Audits::ids(library, c.audit.id).size(), c.count);
            QVERIFY(Audits::compile(c.audit).ok());
            QVERIFY(!c.audit.title.isEmpty() && !c.audit.group.isEmpty());
        }
        QCOMPARE(Audits::ids(library, "dead-links").value(0),
                 value(library, "SELECT id FROM tracks WHERE title = 'Go West'").toLongLong());
        QString error;
        QCOMPARE(Audits::count(library, "nope", &error), -1);
        QVERIFY(!error.isEmpty());
    }

    void duplicatesByTagsAndContent() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QVERIFY(Fixture::fill(library));
        QSqlQuery q(library.db());
        auto clone = [&](const QString &title, const QString &artist, const QString &rel, const QString &format,
                         int bitrate, qint64 duration, int plays) {
            q.prepare("INSERT INTO tracks (volume_id, rel_path, file_name, format, size, mtime_ns, title, artist, "
                      "duration_ms, bitrate, play_count, added) VALUES (1, ?, 'x', ?, 1, 0, ?, ?, ?, ?, ?, 0)");
            for (const QVariant &v : QVariantList{rel, format, title, artist, duration, bitrate, plays})
                q.addBindValue(v);
            QVERIFY(q.exec());
        };
        clone("Midnight Highway!", "the Neon Coast", "dup/a.mp3", "mp3", 320, 245900, 50);
        clone("midnight highway", "Neon Coast", "dup/b.mp3", "mp3", 128, 260000, 0);

        Duplicates::Report r = Duplicates::find(library);
        QCOMPARE(r.groups.size(), 1);
        QCOMPARE(r.groups[0].tracks.size(), 2);
        QCOMPARE(r.groups[0].tracks[0].format, QString("flac"));
        QVERIFY(r.groups[0].tracks[0].keeper);
        QCOMPARE(r.groups[0].reason, QString("lossless (flac)"));
        QCOMPARE(r.groups[0].keeperId, r.groups[0].tracks[0].id);

        Duplicates::Options any;
        any.matchLength = false;
        r = Duplicates::find(library, any);
        QCOMPARE(r.groups.size(), 1);
        QCOMPARE(r.groups[0].tracks.size(), 3);

        // Gone files are left out unless asked for.
        q.exec("UPDATE tracks SET availability = 'missing' WHERE rel_path = 'dup/a.mp3'");
        QCOMPARE(Duplicates::find(library).groups.size(), 0);
        Duplicates::Options missing;
        missing.includeMissing = true;
        r = Duplicates::find(library, missing);
        QCOMPARE(r.groups.size(), 1);
        QCOMPARE(r.groups[0].tracks.last().availability, QString("missing"));
        QCOMPARE(r.groups[0].tracks.first().availability, QString("online"));

        // The same bytes.
        q.exec("UPDATE tracks SET quick_hash = 'abc' WHERE title IN ('Coastline', 'Tidal')");
        Duplicates::Options content;
        content.by = Duplicates::By::Content;
        r = Duplicates::find(library, content);
        QCOMPARE(r.groups.size(), 1);
        QCOMPARE(r.groups[0].tracks[0].title, QString("Coastline"));
        QCOMPARE(r.groups[0].reason, QString("most plays (30)"));
        // Nothing was deleted.
        QCOMPARE(value(library, "SELECT count(*) FROM tracks").toInt(), 14);

        QCOMPARE(Duplicates::normalise("The Beatlés!"), QString("beatles"));
        Duplicates::By by;
        QVERIFY(Duplicates::parse("fingerprint", &by) && by == Duplicates::By::Fingerprint);
        QVERIFY(!Duplicates::parse("vibes", &by));
    }

    void duplicatesByFingerprint() {
        if (Fingerprinter::findProgram().isEmpty())
            QSKIP("fpcalc (chromaprint) is not installed");
        Sandbox s;
        QVERIFY(noise(s.music + "/one.flac", 1, {"-metadata", "title=One"}));
        QVERIFY(noise(s.music + "/copy.mp3", 1, {"-c:a", "libmp3lame", "-b:a", "96k", "-metadata", "title=Other"}));
        QVERIFY(noise(s.music + "/two.flac", 2, {"-metadata", "title=Two"}));
        QVERIFY(s.open());
        QCOMPARE(s.scan().added, 3);
        Duplicates::Options options;
        options.by = Duplicates::By::Fingerprint;
        QCOMPARE(Duplicates::find(*s.library, options).withoutFingerprint, 3);

        Job job("fingerprint");
        Fingerprinter fingerprinter(*s.library, &job);
        fingerprinter.setSandbox(s.root);
        const Fingerprinter::Result f = fingerprinter.run();
        QVERIFY(f.ok);
        QCOMPARE(f.computed, 3);
        QCOMPARE(job.done(), 3);
        const Duplicates::Report r = Duplicates::find(*s.library, options);
        VERIFY_INSIDE(s.root, r.toJson());
        QCOMPARE(r.withoutFingerprint, 0);
        QCOMPARE(r.groups.size(), 1);
        QCOMPARE(r.groups[0].tracks.size(), 2);
        QCOMPARE(r.groups[0].tracks[0].path, s.music + "/one.flac");
        QCOMPARE(r.groups[0].tracks[1].path, s.music + "/copy.mp3");
        // Fingerprints are kept until a file changes.
        QCOMPARE(fingerprinter.run().total, 0);
    }

    void schemaSix() {
        QTemporaryDir dir;
        {
            Library old(dir.filePath("library.db"));
            QVERIFY(old.open(nullptr, 4));
            QCOMPARE(old.schemaVersion(), 4);
        }
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QVERIFY(Library::latestVersion() >= 6);
        QVERIFY(!library.lastBackup().isEmpty());
        QCOMPARE(value(library, "SELECT count(*) FROM sqlite_master WHERE name = 'fingerprints'").toInt(), 1);
    }

    void commands() {
        Sandbox s;
        QVERIFY(tone(s.music + "/inbox/01 - Sunset Riders - Heatwave.flac", {}));
        QVERIFY(tone(s.music + "/inbox/02 - Sunset Riders - Tidal.flac", {}));
        QVERIFY(tone(s.music + "/inbox/odd name.flac", {}));
        QVERIFY(s.open());
        s.scan();
        s.library->close();
        const QString db = s.root + "/library.db";
        QJsonObject j;
        QString errors;

        // Tag from file names: a dry run, then for real.
        QCOMPARE(cli({"tag-from-filename", "path:inbox", "--pattern", "<Track#> - <Artist> - <Title>", "--dry-run",
                      "--json", "--db", db},
                     &j),
                 0);
        VERIFY_INSIDE(s.root, j);
        QCOMPARE(j.value("changed").toInt(), 2);
        QCOMPARE(j.value("unmatched").toArray().size(), 1);
        QCOMPARE(cli({"tag-from-filename", "path:inbox", "--pattern", "<Track#> - <Artist> - <Title>", "--json",
                      "--db", db},
                     &j),
                 0);
        QCOMPARE(j.value("changed").toInt(), 2);
        QCOMPARE(TagWriter::readFile(s.music + "/inbox/02 - Sunset Riders - Tidal.flac")["title"], QString("Tidal"));
        QCOMPARE(cli({"tag-from-filename", "path:inbox", "--pattern", "<Track#> - <Nope>", "--db", db}, nullptr,
                     &errors),
                 1);
        QVERIFY(errors.contains("Unknown field <Nope>"));

        // Tag by query, journal, undo.
        QCOMPARE(cli({"tag", "artist:\"sunset riders\"", "--set", "album=Summer Mix", "--set", "rating=4.5",
                      "--set-mask", "comment=<Track#:2>/<Title>", "--json", "--db", db},
                     &j),
                 0);
        VERIFY_INSIDE(s.root, j);
        QCOMPARE(j.value("changed").toInt(), 2);
        const QString batch = j.value("batch").toString();
        QVERIFY(!batch.isEmpty());
        QCOMPARE(TagWriter::readFile(s.music + "/inbox/01 - Sunset Riders - Heatwave.flac")["comment"],
                 QString("01/Heatwave"));
        QCOMPARE(cli({"journal", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("batches").toArray().first().toObject().value("batch").toString(), batch);
        QCOMPARE(j.value("last_undoable").toString(), batch);
        QCOMPARE(cli({"journal", batch, "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("entries").toArray().size(), 6);
        QCOMPARE(cli({"tag", "title:tidal", "--set", "colour=red", "--db", db}, nullptr, &errors), 1);
        QVERIFY(errors.contains("Unknown field"));
        QCOMPARE(cli({"tag", "title:tidal", "--set", "rating=9", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"undo", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("undid").toString(), batch);
        QCOMPARE(TagWriter::readFile(s.music + "/inbox/01 - Sunset Riders - Heatwave.flac")["album"], QString());

        // Organise: refuses without a mode, previews, applies, undoes.
        QCOMPARE(cli({"organise", "path:inbox", "--mask", "<Artist>/<Title>", "--db", db}, nullptr, &errors), 1);
        QVERIFY(errors.contains("--dry-run"));
        QCOMPARE(cli({"organise", "path:inbox", "--mask", "<Artist>/<Titel>", "--dry-run", "--db", db}, nullptr,
                     &errors),
                 1);
        QVERIFY(errors.contains("at character 10"));
        const auto before = tree(s.music);
        QCOMPARE(cli({"organise", "path:inbox", "--mask", "<Artist>/<Title>", "--dry-run", "--json", "--db", db}, &j), 0);
        VERIFY_INSIDE(s.root, j);
        QCOMPARE(j.value("moves").toInt(), 3);
        QCOMPARE(tree(s.music), before);
        QCOMPARE(cli({"organise", "path:inbox", "--mask", "<Artist>/<Title>", "--apply", "--json", "--db", db}, &j), 0);
        VERIFY_INSIDE(s.root, j);
        QCOMPARE(j.value("moved").toInt(), 3);
        QVERIFY(QFileInfo::exists(s.music + "/Sunset Riders/Tidal.flac"));
        QVERIFY(QFileInfo::exists(s.music + "/Untitled.flac")); // no tags at all: no artist folder
        QCOMPARE(cli({"undo", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("kind").toString(), QString("organise"));
        QCOMPARE(tree(s.music), before);

        // Audits and duplicates.
        QCOMPARE(cli({"audit", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("audits").toArray().size(), Audits::audits().size());
        QCOMPARE(cli({"audit", "unknown-title", "--json", "--db", db}, &j), 0);
        VERIFY_INSIDE(s.root, j);
        QCOMPARE(j.value("total").toInt(), 1);
        QCOMPARE(cli({"audit", "nope", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"duplicates", "--by", "content", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("by").toString(), QString("content"));
        QCOMPARE(cli({"duplicates", "--by", "vibes", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"undo", "no-such-batch", "--db", db}, nullptr, &errors), 1);
        QVERIFY(temporaries(s.root).isEmpty());
    }
};

int runTidyTests(int argc, char **argv) {
    TidyTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tidy_tests.moc"
