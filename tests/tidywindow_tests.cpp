// Phase 2 window: the tag editor, undo and redo, organise with its preview,
// tag from file names, Files to Edit and the duplicates view, driven through
// LibraryController and the real Main.qml offscreen.
//
// Every library, music file and trash lives in a temporary folder. The
// controller's write sandbox fences the tag writer, the organiser and the
// trash into it, and the tests check the paths they see. Nothing is played.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
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

#include <atomic>
#include <memory>

#include "artprovider.h"
#include "audit.h"
#include "dates.h"
#include "fixture_library.h"
#include "library.h"
#include "librarycontroller.h"
#include "ownwrites.h"
#include "player.h"
#include "scanner.h"
#include "tagreader.h"
#include "tagwriter.h"
#include "theme.h"
#include "translator.h"
#include "volumes.h"
#include "watcher.h"

namespace {

bool haveFfmpeg() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }

// A second of quiet tone with the given tags, as the other tests make them.
bool tone(const QString &file, const QStringList &metadata) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        "sine=frequency=440:sample_rate=22050:duration=1", "-af", "volume=0.05", "-map_metadata", "-1"};
    for (const QString &m : metadata)
        args << "-metadata" << m;
    args << file;
    QProcess p;
    p.start("ffmpeg", args);
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

// Every file under a folder, relative: a picture of the tree.
QStringList files(const QString &root) {
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next().mid(root.size() + 1);
    out.sort();
    return out;
}

QVariant sql(Library &library, const QString &query, const QVariantList &params = {}) {
    QSqlQuery q(library.db());
    q.prepare(query);
    for (const QVariant &p : params)
        q.addBindValue(p);
    return q.exec() && q.next() ? q.value(0) : QVariant();
}

// A temporary folder with music made by ffmpeg, scanned into a library, and
// a controller fenced into the folder.
struct Music {
    QTemporaryDir dir;
    QString root, music;
    std::unique_ptr<LibraryController> controller;

    Music() {
        root = QFileInfo(dir.path()).canonicalFilePath();
        music = root + "/music";
        QDir().mkpath(music);
    }
    bool add(const QString &rel, const QStringList &metadata) { return tone(music + '/' + rel, metadata); }
    bool scan() {
        Library library(root + "/library.db");
        if (!library.open())
            return false;
        Scanner(library, VolumeResolver()).scan({music});
        return true;
    }
    LibraryController &open() {
        controller = std::make_unique<LibraryController>(root + "/library.db");
        controller->setWatching(false);
        controller->setArtCacheDir(root + "/art");
        controller->setWriteSandbox(root);
        controller->activate();
        return *controller;
    }
    qint64 id(const QString &rel, LibraryController *c = nullptr) {
        return sql(*(c ? c : controller.get())->library(),"SELECT id FROM tracks WHERE rel_path LIKE ?", {"%" + rel}).toLongLong();
    }
    // Waits for the controller's running job (tag, organise, undo...) to end.
    bool waitForJob(int ms = 30000) {
        if (!controller->busy())
            return true;
        QSignalSpy done(controller.get(), &LibraryController::jobFinished);
        return done.wait(ms);
    }
};

void typeText(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

// True once the spy has caught its signal, waiting up to ms for it. Unlike
// QSignalSpy::wait() alone, it counts a signal that came before the call: a
// small job can finish while QTest::keyClick is still delivering the key,
// and wait() only counts what arrives while it waits.
bool seen(QSignalSpy &spy, int ms = 30000) { return !spy.isEmpty() || spy.wait(ms); }

} // namespace

class TidyWindowTests : public QObject {
    Q_OBJECT

private slots:
    void datesReadTheSameEverywhere() {
        const QDate d(2026, 7, 12);
        QCOMPARE(Dates::text(d, QLocale(QLocale::English, QLocale::UnitedStates)), QString("12 Jul 2026"));
        // Australian English writes July out in full; still day first.
        QCOMPARE(Dates::text(d, QLocale(QLocale::English, QLocale::Australia)), QString("12 July 2026"));
        QCOMPARE(Dates::text(d, QLocale(QLocale::German, QLocale::Germany)), QString("12.07.2026"));
        QVERIFY(Dates::text(QDate()).isEmpty());
    }

    void tagEditorEditsSeveralTracksUndoAndRedo() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("Band/Album/01 One.flac", {"title=One", "artist=Band", "album=Album", "genre=Rock", "track=1"}));
        QVERIFY(m.add("Band/Album/02 Two.mp3", {"title=Two", "artist=Band", "album=Album", "genre=Pop", "track=2"}));
        QVERIFY(m.scan());
        LibraryController &c = m.open();
        const qint64 one = m.id("01 One.flac"), two = m.id("02 Two.mp3");
        QVERIFY(one > 0 && two > 0);
        const QString onePath = c.library()->absolutePath(one), twoPath = c.library()->absolutePath(two);
        QVERIFY(onePath.startsWith(m.root + '/') && twoPath.startsWith(m.root + '/'));

        TagEditor *ed = c.tagEditor();
        QVERIFY(ed->load({one, two}));
        ed->waitForLoad();
        QCOMPARE(ed->trackCount(), 2);
        // Agreed fields show their value; differing ones are kept.
        QCOMPARE(ed->value("album"), QString("Album"));
        QVERIFY(!ed->mixed("album"));
        QVERIFY(ed->mixed("genre"));
        QVERIFY(ed->mixed("title"));
        QCOMPARE(ed->originalValues("genre").size(), 2);
        // Typing a mixed field empty goes back to keeping it.
        ed->setValue("genre", "J");
        QVERIFY(ed->edited("genre"));
        ed->setValue("genre", "");
        QVERIFY(ed->mixed("genre"));
        QVERIFY(!ed->dirty());
        // Typing what every track already has is no edit.
        ed->setValue("album", "Album");
        QVERIFY(!ed->dirty());

        ed->setValue("genre", "Jazz; Blues");
        ed->setValue("year", "1999");
        QVERIFY(ed->dirty());
        QSignalSpy applied(ed, &TagEditor::applied);
        QVERIFY(ed->apply());
        QVERIFY(c.busy());
        QVERIFY(applied.wait(30000));
        QCOMPARE(applied.first().first().toBool(), true);
        QVERIFY(ed->failures().isEmpty());
        QVERIFY(!ed->dirty());

        // The files and the library both say so; titles were kept.
        for (const QString &path : {onePath, twoPath}) {
            const QMap<QString, QString> v = TagWriter::readFile(path);
            QCOMPARE(v.value("genre"), QString("Jazz; Blues"));
            QCOMPARE(v.value("year"), QString("1999"));
        }
        QCOMPARE(TagWriter::readFile(onePath).value("title"), QString("One"));
        QCOMPARE(TagWriter::readFile(twoPath).value("title"), QString("Two"));
        QCOMPARE(sql(*c.library(), "SELECT year FROM tracks WHERE id = ?", {one}).toInt(), 1999);
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM track_genres WHERE track_id = ?", {two}).toInt(), 2);
        QVERIFY(c.undoText().startsWith("tag edit of 2 tracks"));
        QVERIFY(c.redoText().isEmpty());

        // Undo restores both files; redo puts the edit back; undo again.
        QSignalSpy toasts(&c, &LibraryController::toast);
        QVERIFY(c.undo());
        QVERIFY(m.waitForJob());
        QCOMPARE(TagWriter::readFile(onePath).value("genre"), QString("Rock"));
        QCOMPARE(TagWriter::readFile(twoPath).value("genre"), QString("Pop"));
        QCOMPARE(TagWriter::readFile(onePath).value("year"), QString());
        QVERIFY(toasts.last().first().toString().startsWith("Undone: tag edit of 2 tracks"));
        QVERIFY(c.undoText().isEmpty());
        QVERIFY(c.redoText().startsWith("tag edit of 2 tracks"));
        QVERIFY(c.redo());
        QVERIFY(m.waitForJob());
        QCOMPARE(TagWriter::readFile(twoPath).value("genre"), QString("Jazz; Blues"));
        QVERIFY(toasts.last().first().toString().startsWith("Redone:"));
        QVERIFY(c.undo());
        QVERIFY(m.waitForJob());
        QCOMPARE(TagWriter::readFile(twoPath).value("genre"), QString("Pop"));
        QCOMPARE(sql(*c.library(), "SELECT genre FROM tracks WHERE id = ?", {two}).toString(), QString("Pop"));

        // A new edit ends what can be redone.
        QVERIFY(ed->load({one}));
        ed->waitForLoad();
        ed->setValue("comment", "New");
        QVERIFY(ed->apply());
        QVERIFY(applied.wait(30000));
        QVERIFY(c.redoText().isEmpty());
        QVERIFY(c.undoText().startsWith("tag edit of 1 tracks") || c.undoText().startsWith("tag edit of 1 track"));

        // The history lists every batch, newest first, and their changes.
        const QVariantList batches = c.journalBatches();
        QCOMPARE(batches.size(), 5);
        QVERIFY(batches.first().toMap().value("label").toString().startsWith("Tag edit of 1 track"));
        QVERIFY(batches[1].toMap().value("label").toString().startsWith("Undo of tag edit of 2 tracks"));
        QVERIFY(batches[2].toMap().value("label").toString().startsWith("Redo of tag edit of 2 tracks"));
        QVERIFY(batches.last().toMap().value("undone").toBool());
        const QVariantList entries = c.journalEntries(batches.last().toMap().value("batch").toString());
        QVERIFY(entries.size() >= 4);
        QVERIFY(QDir(m.root).exists("music/Band/Album"));
        // No temporary copies left behind.
        for (const QString &f : files(m.music))
            QVERIFY2(!QFileInfo(f).fileName().startsWith(".omamp-"), qPrintable(f));
    }

    void tagEditorArtworkAndLyrics() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("A/01 Song.flac", {"title=Song", "artist=A", "album=B"}));
        QVERIFY(m.scan());
        // A folder picture shows when the file has none.
        QImage cover(32, 32, QImage::Format_RGB32);
        cover.fill(Qt::red);
        QVERIFY(cover.save(m.music + "/A/cover.jpg"));
        QImage blue(24, 24, QImage::Format_RGB32);
        blue.fill(Qt::blue);
        QVERIFY(blue.save(m.root + "/new.png"));
        LibraryController &c = m.open();
        const qint64 id = m.id("01 Song.flac");
        TagEditor *ed = c.tagEditor();
        QVERIFY(ed->load({id}));
        ed->waitForLoad();
        QVERIFY(ed->artworkNote().contains("cover.jpg"));
        QVERIFY(ed->artworkSource().startsWith("data:image/jpeg;base64,"));
        QVERIFY(ed->setArtworkFromFile(m.root + "/new.png"));
        QCOMPARE(ed->artworkAction(), QString("new"));
        QVERIFY(!ed->setArtworkFromFile(m.root + "/library.db")); // not a picture
        ed->setValue("lyrics", "Line one\nLine two");
        QSignalSpy applied(ed, &TagEditor::applied);
        QVERIFY(ed->apply());
        QVERIFY(applied.wait(30000));
        const QString path = c.library()->absolutePath(id);
        QString mime;
        QVERIFY(!TagReader::embeddedPicture(path, &mime).isEmpty());
        QCOMPARE(TagWriter::readFile(path).value("lyrics"), QString("Line one\nLine two"));
        // Reloaded, the picture is in the file now; Remove takes it out.
        QVERIFY(ed->load({id}));
        ed->waitForLoad();
        QVERIFY(ed->artworkNote().startsWith("In the file"));
        ed->removeArtwork();
        QVERIFY(ed->apply());
        QVERIFY(applied.wait(30000));
        QVERIFY(TagReader::embeddedPicture(path).isEmpty());
        QVERIFY(QFile::exists(m.music + "/A/cover.jpg")); // folder pictures are left alone
    }

    void organisePreviewEditApplyAndUndo() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("in/a.flac", {"title=First", "artist=Alpha", "album=One", "date=2001", "track=1"}));
        QVERIFY(m.add("in/b.flac", {"title=Second", "artist=Alpha", "album=One", "date=2001", "track=2"}));
        QVERIFY(m.add("in/c.flac", {"title=Third", "artist=Beta", "album=Two", "track=3"}));
        QVERIFY(m.add("in/d.flac", {"title=Fourth", "artist=Beta", "album=Two", "track=4"}));
        QVERIFY(m.scan());
        QFile cue(m.music + "/in/folder.jpg");
        QVERIFY(cue.open(QIODevice::WriteOnly) && cue.write("not really a jpeg") > 0);
        cue.close();
        const QStringList before = files(m.root + "/music");

        LibraryController &c = m.open();
        OrganiseModel *org = c.organiser();
        QVariantList ids;
        for (const char *f : {"a.flac", "b.flac", "c.flac", "d.flac"})
            ids << m.id(f);
        QVERIFY(org->load(ids));
        // The default mask leaves out what a track lacks: no "()" and no "-".
        QCOMPARE(org->count(), 4);
        const QString defaultC = org->data(org->index(2), OrganiseModel::NewPathRole).toString();
        QCOMPARE(defaultC, m.music + "/Beta/Two/03 Third.flac");
        QCOMPARE(org->data(org->index(0), OrganiseModel::NewPathRole).toString(),
                 m.music + "/Alpha/One (2001)/01 First.flac");

        // A broken mask says where.
        org->setMask("<Artist>/<Nope>");
        org->replan();
        QVERIFY(!org->maskError().isEmpty());
        QVERIFY(org->maskErrorPosition() >= 9 && org->maskErrorPosition() <= 10);
        // Completions for a half-typed field and a function.
        const QVariantList fields = org->completions("<Artist>/<Alb", 13);
        QStringList inserts;
        for (const QVariant &f : fields) {
            inserts << f.toMap().value("insert").toString();
            QCOMPARE(f.toMap().value("start").toInt(), 9);
        }
        QVERIFY(inserts.contains("<Album>"));
        QVERIFY(inserts.contains("<Album Artist>"));
        QVERIFY(!inserts.contains("<Artist>"));
        QVERIFY(org->completions("$Mov", 4).first().toMap().value("insert").toString() == "$MovePrefix(");

        org->setMask("<Artist>/<Album>/<Track#:2> <Title>");
        org->replan();
        QVERIFY(org->maskError().isEmpty());
        QCOMPARE(org->moves(), 4);
        QCOMPARE(org->commonRoot(), m.music);
        QCOMPARE(org->data(org->index(0), OrganiseModel::NewShownRole).toString(), QString("Alpha/One/01 First.flac"));
        for (int r = 0; r < org->count(); ++r) {
            QVERIFY(org->data(org->index(r), OrganiseModel::NewPathRole).toString().startsWith(m.root + '/'));
            QCOMPARE(org->data(org->index(r), OrganiseModel::StatusRole).toString(), QString("move"));
        }
        // A new path by hand: a name alone stays in its planned folder and keeps its type.
        org->setNewPath(0, "Opening");
        QCOMPARE(org->data(org->index(0), OrganiseModel::NewPathRole).toString(), m.music + "/Alpha/One/Opening.flac");
        QVERIFY(org->data(org->index(0), OrganiseModel::EditedRole).toBool());
        // Two rows with one name: both conflict, and stay where they are.
        org->setNewPath(3, m.music + "/Beta/Two/03 Third.flac");
        QCOMPARE(org->data(org->index(2), OrganiseModel::StatusRole).toString(), QString("conflict"));
        QCOMPARE(org->data(org->index(3), OrganiseModel::StatusRole).toString(), QString("conflict"));
        QCOMPARE(org->conflicts(), 2);
        // Put one back: the other is free again.
        org->setNewPath(3, m.music + "/Beta/Two/04 Fourth.flac");
        QCOMPARE(org->data(org->index(2), OrganiseModel::StatusRole).toString(), QString("move"));
        // A path onto a file that is there already is a conflict too.
        org->setNewPath(3, m.music + "/in/a.flac");
        QCOMPARE(org->data(org->index(3), OrganiseModel::StatusRole).toString(), QString("conflict"));
        QCOMPARE(org->moves(), 3);

        QSignalSpy appliedSpy(org, &OrganiseModel::appliedChanged);
        QVERIFY(org->apply());
        QVERIFY(m.waitForJob());
        QVERIFY(org->applied());
        QVERIFY(!org->lastBatch().isEmpty());
        QVERIFY(org->resultText().startsWith("3 files moved"));
        QVERIFY(QFile::exists(m.music + "/Alpha/One/Opening.flac"));
        QVERIFY(QFile::exists(m.music + "/Alpha/One/02 Second.flac"));
        QVERIFY(QFile::exists(m.music + "/Beta/Two/03 Third.flac"));
        QVERIFY(QFile::exists(m.music + "/in/d.flac")); // the conflict stayed
        QVERIFY(!QFile::exists(m.music + "/in/a.flac"));
        QCOMPARE(org->data(org->index(3), OrganiseModel::StatusRole).toString(), QString("skipped"));
        QCOMPARE(c.library()->absolutePath(ids[0].toLongLong()), m.music + "/Alpha/One/Opening.flac");
        QVERIFY(c.undoText().startsWith("organise of 3 files"));
        for (const QString &f : files(m.root))
            QVERIFY2(!f.startsWith(".."), qPrintable(f));

        // Undo from the sheet puts every file back, the tree exactly as it was.
        QVERIFY(org->undo());
        QVERIFY(m.waitForJob());
        QCOMPARE(files(m.root + "/music"), before);
        QCOMPARE(c.library()->absolutePath(ids[0].toLongLong()), m.music + "/in/a.flac");
        org->close();
        QCOMPARE(org->count(), 0);
    }

    void tagFromFileNames() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("Some Album/03 - Night Song.flac", {}));
        QVERIFY(m.add("Some Album/04 - Day Song.mp3", {}));
        QVERIFY(m.add("Some Album/stray.flac", {}));
        QVERIFY(m.scan());
        LibraryController &c = m.open();
        TagFromNameModel *tfn = c.tagFromName();
        const qint64 a = m.id("03 - Night Song.flac"), b = m.id("04 - Day Song.mp3"), s = m.id("stray.flac");
        QVERIFY(tfn->load({a, b, s}));
        tfn->setPattern("<Album>/<Track#> - <Title>");
        QCOMPARE(tfn->fields(), QStringList({"album", "track", "title"}));
        QCOMPARE(tfn->fieldLabels(), QStringList({"Album", "Track#", "Title"}));
        QCOMPARE(tfn->matched(), 2);
        const QStringList values = tfn->data(tfn->index(0), TagFromNameModel::ValuesRole).toStringList();
        QCOMPARE(values.size(), 3);
        QCOMPARE(values[0], QString("Some Album"));
        QCOMPARE(values[1].toInt(), 3);
        QCOMPARE(values[2], QString("Night Song"));
        QCOMPARE(tfn->data(tfn->index(0), TagFromNameModel::PathRole).toString(), QString("Some Album/03 - Night Song.flac"));
        QVERIFY(!tfn->data(tfn->index(2), TagFromNameModel::MatchedRole).toBool());
        // A broken pattern says so.
        tfn->setPattern("<Track#> - <Nope>");
        QVERIFY(!tfn->patternError().isEmpty());
        QCOMPARE(tfn->matched(), 0);
        tfn->setPattern("<Album>/<Track#> - <Title>");
        QSignalSpy applied(tfn, &TagFromNameModel::applied);
        QVERIFY(tfn->apply());
        QVERIFY(applied.wait(30000));
        const QMap<QString, QString> v = TagWriter::readFile(c.library()->absolutePath(b));
        QCOMPARE(v.value("title"), QString("Day Song"));
        QCOMPARE(v.value("track"), QString("4"));
        QCOMPARE(v.value("album"), QString("Some Album"));
        QVERIFY(TagWriter::readFile(c.library()->absolutePath(s)).value("title").isEmpty());
        QVERIFY(c.undoText().startsWith("tags from file names for 2 tracks"));
        QVERIFY(c.undo());
        QVERIFY(m.waitForJob());
        QVERIFY(TagWriter::readFile(c.library()->absolutePath(b)).value("title").isEmpty());
    }

    void filesToEditCountsEachCheck() {
        QTemporaryDir dir;
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            QVERIFY(Fixture::fill(library));
        }
        LibraryController c(dir.filePath("library.db"));
        c.setWatching(false);
        c.setArtCacheDir(dir.filePath("art"));
        QVERIFY(c.activate());
        LibraryTree *tree = c.tree();
        const int group = tree->rowOfKey("filestoedit");
        QVERIFY(group > 0);
        QSignalSpy counted(&c, &LibraryController::auditsCounted);
        tree->expand(group);
        QVERIFY(counted.wait(10000));
        QCOMPARE(tree->node(group + 1).kind, QString("duplicates"));
        int checked = 0;
        for (const Audits::Audit &a : Audits::audits()) {
            const int row = tree->rowOfKey("audit:" + a.id);
            QVERIFY(row > group);
            const qint64 expected = Audits::count(*c.library(), a.id);
            QCOMPARE(tree->node(row).count, expected);
            QCOMPARE(tree->data(tree->index(row), LibraryTree::DimRole).toBool(), expected == 0);
            QCOMPARE(tree->data(tree->index(row), LibraryTree::DescriptionRole).toString(), a.description);
            // Choosing the node lists exactly those tracks.
            c.selectNode(row);
            QCOMPARE(qint64(c.tracks()->count()), expected);
            ++checked;
        }
        QCOMPARE(checked, Audits::audits().size());
        // Somewhere in the fixture there is work to do.
        QVERIFY(tree->node(tree->rowOfKey("audit:unknown-genre")).count > 0);
        // After a refresh the group stays open and counts again.
        counted.clear();
        c.refreshAll();
        QVERIFY(counted.wait(10000));
        QVERIFY(tree->filesToEditOpen());
    }

    void duplicatesRemoveAndTrash() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("A/song.flac", {"title=Song", "artist=Band"}));
        QVERIFY(m.add("B/other.flac", {"title=Other", "artist=Band"}));
        QDir().mkpath(m.music + "/Copies");
        QVERIFY(QFile::copy(m.music + "/A/song.flac", m.music + "/Copies/song copy.flac"));
        QVERIFY(QFile::copy(m.music + "/A/song.flac", m.music + "/Copies/song copy 2.flac"));
        QVERIFY(QFile::copy(m.music + "/B/other.flac", m.music + "/Copies/other copy.flac"));
        QVERIFY(m.scan());
        LibraryController &c = m.open();
        // The trash, for the test, is a folder in the sandbox.
        const QString trash = m.root + "/trash";
        QDir().mkpath(trash);
        QStringList trashed;
        c.setTrashFunction([&](const QString &path, QString *error) {
            if (!path.startsWith(m.root + '/')) {
                *error = "outside";
                return false;
            }
            trashed << path;
            return QFile::rename(path, trash + '/' + QFileInfo(path).fileName());
        });

        DuplicatesModel *dup = c.duplicates();
        dup->setBy("content");
        QVERIFY(dup->find());
        QVERIFY(m.waitForJob());
        QVERIFY(dup->searched());
        QCOMPARE(dup->groupCount(), 2);
        QCOMPARE(dup->trackCount(), 5);
        QCOMPARE(dup->rowCount(), 7);
        // Each group: a heading with the reason, then the keeper first.
        QCOMPARE(dup->data(dup->index(0), DuplicatesModel::KindRole).toString(), QString("group"));
        QVERIFY(!dup->data(dup->index(0), DuplicatesModel::ReasonRole).toString().isEmpty());
        QVERIFY(dup->data(dup->index(1), DuplicatesModel::KeeperRole).toBool());
        QVERIFY(!dup->data(dup->index(2), DuplicatesModel::KeeperRole).toBool());
        QCOMPARE(dup->commonRoot(), m.music);

        QStringList keepers;
        for (int r = 0; r < dup->rowCount(); ++r)
            if (dup->data(dup->index(r), DuplicatesModel::KeeperRole).toBool())
                keepers << c.library()->absolutePath(dup->data(dup->index(r), DuplicatesModel::TrackIdRole).toLongLong());
        QCOMPARE(keepers.size(), 2);
        dup->selectNonKeepers();
        QCOMPARE(dup->selectedCount(), 3);
        const QVariantList three = dup->selectedIds();
        // Take one out of the library: the file stays.
        // A copy of the song (a group of three), so its group stays a group.
        qint64 first = 0;
        for (const QVariant &v : three)
            if (c.library()->absolutePath(v.toLongLong()).contains("song copy"))
                first = v.toLongLong();
        QVERIFY(first > 0);
        const QString firstPath = c.library()->absolutePath(first);
        const int before = c.tracks()->count();
        QCOMPARE(c.removeFromLibrary({first}), 1);
        QCOMPARE(c.tracks()->count(), before - 1);
        QVERIFY(QFile::exists(firstPath));
        QVERIFY(sql(*c.library(), "SELECT count(*) FROM tracks WHERE id = ?", {first}).toInt() == 0);
        QCOMPARE(dup->selectedCount(), 2);
        QCOMPARE(dup->trackCount(), 4);

        // Move the rest to the trash: files leave, rows go, keepers stay.
        QVERIFY(c.moveToTrash(dup->selectedIds()));
        QVERIFY(m.waitForJob());
        QCOMPARE(trashed.size(), 2);
        for (const QString &p : trashed)
            QVERIFY(!QFile::exists(p));
        QCOMPARE(QDir(trash).entryList(QDir::Files).size(), 2);
        QCOMPARE(c.tracks()->count(), before - 3);
        QCOMPARE(dup->groupCount(), 0);
        for (const QString &k : keepers) {
            QVERIFY(k.startsWith(m.music + '/'));
            QVERIFY(QFile::exists(k));
        }

        // A track that isn't there is reported, and nothing else is touched.
        QVERIFY(c.moveToTrash({qint64(999999)}));
        QVERIFY(m.waitForJob());
        QCOMPARE(trashed.size(), 2);
    }
};

// The same work from the keyboard, in the real window.
class TidyWindowKeysTests : public QObject {
    Q_OBJECT

    struct Window {
        QTemporaryDir &dir;
        Translator translator;
        Theme theme;
        Player player;
        LibraryController library;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;

        explicit Window(QTemporaryDir &d, const QString &root, bool watching = false)
            : dir(d), translator(d.filePath("language")), theme(d.filePath("theme"), nullptr, d.filePath("no-hyprctl")),
              library(d.filePath("library.db")) {
            player.setMuted(true);
            library.setPlayer(&player);
            library.setWatching(watching);
            library.setArtCacheDir(d.filePath("art"));
            library.setWriteSandbox(root);
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("backend", &player);
            engine.rootContext()->setContextProperty("library", &library);
            engine.rootContext()->setContextProperty("commandPalette", library.palette());
            engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(d.filePath("window.conf")));
            engine.addImageProvider(ArtImageProvider::name,
                                    new ArtImageProvider(d.filePath("library.db"), d.filePath("art")));
        }
        bool load() {
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
    };

private slots:
    void editTagsFromTheKeyboardAndUndo() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("Band/Album/01 One.flac", {"title=One", "artist=Band", "album=Album", "genre=Rock", "track=1"}));
        QVERIFY(m.add("Band/Album/02 Two.flac", {"title=Two", "artist=Band", "album=Album", "genre=Pop", "track=2"}));
        QVERIFY(m.scan());
        {
            QSettings conf(m.dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", true);
        }
        Window w(m.dir, m.root);
        QVERIFY(w.load());
        QQuickWindow *window = w.window;
        LibraryController &c = w.library;
        QTRY_VERIFY(c.ready());
        QTRY_VERIFY(w.find("advancedView"));
        QTRY_COMPARE(c.tracks()->count(), 2);
        QTRY_VERIFY(w.find("trackTable") && w.find("trackTable")->property("activeFocus").toBool());

        // Both tracks, Ctrl+E: the editor opens with genre kept.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(c.tracks()->selectedCount(), 2);
        QTest::keyClick(window, Qt::Key_E, Qt::ControlModifier);
        QObject *sheet = w.find("tagEditor");
        QVERIFY(sheet);
        QTRY_VERIFY(sheet->property("opened").toBool());
        QTRY_VERIFY(!c.tagEditor()->loading());
        QVERIFY(window->property("sheetOpen").toBool());
        QVERIFY(c.tagEditor()->mixed("genre"));

        // Type a genre. Q and Space go into the field, not to the player.
        QObject *genre = w.find("tagField_genre");
        QVERIFY(genre);
        QMetaObject::invokeMethod(genre, "focusInput");
        QTRY_VERIFY(genre->property("inputFocused").toBool());
        typeText(window, "Q Jazz");
        QCOMPARE(c.tagEditor()->value("genre"), QString("Q Jazz"));
        QVERIFY(window->isVisible());
        QVERIFY(c.tagEditor()->edited("genre"));

        // Ctrl+Enter saves; the editor closes when every file is written.
        QSignalSpy finished(&c, &LibraryController::jobFinished);
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(seen(finished));
        QTRY_VERIFY(!sheet->property("opened").toBool());
        QVERIFY(!window->property("sheetOpen").toBool());
        const QString one = c.library()->absolutePath(m.id("01 One.flac", &c));
        const QString two = c.library()->absolutePath(m.id("02 Two.flac", &c));
        QCOMPARE(TagWriter::readFile(one).value("genre"), QString("Q Jazz"));
        QCOMPARE(TagWriter::readFile(two).value("genre"), QString("Q Jazz"));
        QTRY_COMPARE(c.tracks()->data(c.tracks()->index(0, TrackListModel::Genre), Qt::DisplayRole).toString(),
                     QString("Q Jazz"));
        QObject *toast = w.find("toast");
        QVERIFY(toast);

        // Ctrl+Z from the list puts both back, and the toast says what.
        QTRY_VERIFY(w.find("trackTable")->property("activeFocus").toBool());
        finished.clear();
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(seen(finished));
        QCOMPARE(TagWriter::readFile(one).value("genre"), QString("Rock"));
        QCOMPARE(TagWriter::readFile(two).value("genre"), QString("Pop"));
        QTRY_VERIFY(toast->property("visible").toBool());
        QVERIFY(toast->property("text").toString().startsWith("Undone: tag edit of 2 tracks"));
        QCOMPARE(w.player.state(), QString("stopped"));

        // Ctrl+Shift+Z redoes it, once the journal says there is something to
        // redo (it is re-read just after the undo job reports back).
        QTRY_VERIFY(!c.property("redoText").toString().isEmpty());
        finished.clear();
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(seen(finished));
        QCOMPARE(TagWriter::readFile(two).value("genre"), QString("Q Jazz"));

        // F2 renames the track under the cursor in place.
        c.tracks()->select(0);
        QTest::keyClick(window, Qt::Key_F2);
        QObject *input = w.find("titleInput");
        QVERIFY(input);
        QTRY_VERIFY(input->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        typeText(window, "Uno");
        finished.clear();
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(seen(finished));
        QCOMPARE(TagWriter::readFile(one).value("title"), QString("Uno"));

        // The undo history opens from the palette and lists all four changes.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        typeText(window, ">undo history");
        QTRY_COMPARE(c.palette()->get(0).value("itemId").toString(), QString("undo-history"));
        QTest::keyClick(window, Qt::Key_Return);
        QObject *history = w.find("undoHistory");
        QTRY_VERIFY(history && history->property("opened").toBool());
        QCOMPARE(history->property("batches").toList().size(), 4);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!history->property("opened").toBool());

        // Organise from the keyboard: Ctrl+Shift+O, a mask, Ctrl+Enter.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
        QObject *organise = w.find("organiseSheet");
        QTRY_VERIFY(organise && organise->property("opened").toBool());
        c.organiser()->setMask("<Artist>/<Track#:2> <Title>");
        c.organiser()->replan();
        QCOMPARE(c.organiser()->moves(), 2);
        finished.clear();
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(seen(finished));
        QVERIFY(QFile::exists(m.music + "/Band/01 Uno.flac"));
        QVERIFY(QFile::exists(m.music + "/Band/02 Two.flac"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!organise->property("opened").toBool());
        // Ctrl+Z moves them back.
        finished.clear();
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(seen(finished));
        QVERIFY(QFile::exists(one));
        QVERIFY(QFile::exists(two));
        QVERIFY(!QDir(m.music + "/Band").exists("01 Uno.flac"));
        window->close();
    }

    // A save followed at once by Ctrl+Z, while a rescan for a change from
    // outside is waiting on the watcher: the undo still happens (it waits
    // its turn if the save is still running), it undoes that save, and the
    // only rescan is the one for the outside change.
    void saveThenUndoAtOnceWhileTheWatcherWaits() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(m.add("Band/Album/01 One.flac", {"title=One", "artist=Band", "album=Album", "track=1"}));
        QVERIFY(m.add("Band/Album/02 Two.flac", {"title=Two", "artist=Band", "album=Album", "track=2"}));
        QVERIFY(m.scan());
        {
            QSettings conf(m.dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", true);
        }
        Window w(m.dir, m.root, true);
        QVERIFY(w.load());
        QQuickWindow *window = w.window;
        LibraryController &c = w.library;
        QTRY_VERIFY(c.ready());
        QTRY_VERIFY(c.watcher() && c.watcher()->watchedFolders() >= 3);
        Watcher *watcher = c.watcher();
        QTRY_COMPARE(c.tracks()->count(), 2);
        QTRY_VERIFY(w.find("trackTable") && w.find("trackTable")->property("activeFocus").toBool());

        QStringList jobs;
        connect(&c, &LibraryController::jobFinished, &c, [&](const QString &kind, const QJsonObject &) { jobs << kind; });
        int rescans = 0, added = 0;
        connect(watcher, &Watcher::rescanFinished, &c, [&](const QJsonObject &r) {
            if (!r.value("yielded").toBool()) {
                ++rescans;
                added += r.value("added").toInt();
            }
        });

        // A file arrives from outside: its rescan waits out the debounce.
        QVERIFY(m.add("Band/Album/03 Three.flac", {"title=Three", "artist=Band", "album=Album", "track=3"}));

        // F2, a new title, Return; then Ctrl+Z straight away.
        const qint64 one = m.id("01 One.flac", &c);
        const QString onePath = c.library()->absolutePath(one);
        c.tracks()->select(0);
        QCOMPARE(c.tracks()->trackId(0), one);
        QTest::keyClick(window, Qt::Key_F2);
        QObject *input = w.find("titleInput");
        QVERIFY(input);
        QTRY_VERIFY(input->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        typeText(window, "Uno");
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(w.find("trackTable")->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);

        // Both ran, in order, and the title is back.
        QTRY_COMPARE_WITH_TIMEOUT(jobs, (QStringList{"tag", "undo"}), 30000);
        QCOMPARE(TagWriter::readFile(onePath).value("title"), QString("One"));
        QCOMPARE(c.queuedJobs(), 0);

        // The outside change is rescanned once; our two writes add none.
        QTRY_COMPARE_WITH_TIMEOUT(added, 1, 30000);
        QTRY_COMPARE(c.tracks()->count(), 3);
        QTest::qWait(2500); // longer than the debounce: anything late would show
        QCOMPARE(rescans, 1);
        QVERIFY(watcher->ownEventsIgnored() >= 2);
        window->close();
    }

    void filesToEditAndDuplicatesInTheWindow() {
        QTemporaryDir dir;
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            QVERIFY(Fixture::fill(library));
        }
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", true);
        }
        Window w(dir, QFileInfo(dir.path()).canonicalFilePath());
        QVERIFY(w.load());
        LibraryController &c = w.library;
        QTRY_VERIFY(c.ready());
        QTRY_VERIFY(w.find("advancedView"));

        // The palette goes to a Files to Edit check.
        QTest::keyClick(w.window, Qt::Key_K, Qt::ControlModifier);
        typeText(w.window, ">unknown genre");
        QTRY_COMPARE(c.palette()->get(0).value("itemId").toString(), QString("audit:unknown-genre"));
        QSignalSpy counted(&c, &LibraryController::auditsCounted);
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(c.tree()->key(c.currentNode()), QString("audit:unknown-genre"));
        QCOMPARE(qint64(c.tracks()->count()), Audits::count(*c.library(), "unknown-genre"));
        QVERIFY(counted.wait(10000) || c.tree()->hasAuditCounts());

        // Find duplicates shows its own view and looks by artist and title.
        QTest::keyClick(w.window, Qt::Key_K, Qt::ControlModifier);
        typeText(w.window, ">find duplicates");
        QTRY_COMPARE(c.palette()->get(0).value("itemId").toString(), QString("find-duplicates"));
        QTest::keyClick(w.window, Qt::Key_Return);
        QCOMPARE(c.currentKind(), QString("duplicates"));
        QObject *view = w.find("duplicatesView");
        QTRY_VERIFY(view && view->property("visible").toBool());
        QVERIFY(!w.find("trackTable")->property("visible").toBool());
        QTRY_VERIFY(c.duplicates()->searched());
        // Editing tags needs tracks; the duplicates view has none chosen.
        QTest::keyClick(w.window, Qt::Key_E, Qt::ControlModifier);
        QVERIFY(!w.find("tagEditor")->property("opened").toBool());
        w.window->close();
    }
};

// Omamp's own writes and the folder watcher: tag saves, organise, undo and
// the trash never start a rescan, a change from outside still does, and
// the window's jobs never wait for (or give way to) a rescan.
class OwnWritesTests : public QObject {
    Q_OBJECT

    // A controller with the folder watcher on, quick to react.
    static LibraryController &openWatching(Music &m) {
        m.controller = std::make_unique<LibraryController>(m.root + "/library.db");
        m.controller->setArtCacheDir(m.root + "/art");
        m.controller->setWriteSandbox(m.root);
        m.controller->activate();
        if (Watcher *w = m.controller->watcher()) {
            w->setDebounce(100);
            w->setMaxDelay(1000);
        }
        return *m.controller;
    }

    static bool twoTracks(Music &m) {
        return m.add("Band/Album/01 One.flac", {"title=One", "artist=Band", "album=Album", "track=1"}) &&
               m.add("Band/Album/02 Two.flac", {"title=Two", "artist=Band", "album=Album", "track=2"}) && m.scan();
    }

private slots:
    void cleanup() { OwnWrites::clear(); }

    void ownWritesStartNoRescan() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(twoTracks(m));
        LibraryController &c = openWatching(m);
        QVERIFY(c.ready());
        Watcher *w = c.watcher();
        QVERIFY(w && w->isRunning());
        QCOMPARE(w->watchedFolders(), 3); // music, Band and Album
        QSignalSpy started(w, &Watcher::rescanStarted);
        QSignalSpy finished(&c, &LibraryController::jobFinished);
        // Longer than the debounce: a rescan the write caused would start.
        auto quiet = [&] {
            QTest::qWait(600);
            return started.isEmpty();
        };
        const qint64 one = m.id("01 One.flac"), two = m.id("02 Two.flac");

        // A tag save (written beside the file, then renamed over it).
        QVERIFY(c.tagEditor()->quickSet(one, "title", "Uno"));
        QVERIFY(seen(finished));
        QCOMPARE(TagWriter::readFile(m.music + "/Band/Album/01 One.flac").value("title"), QString("Uno"));
        QVERIFY(quiet());
        QVERIFY(w->ownEventsIgnored() > 0);

        // Its undo.
        finished.clear();
        QVERIFY(c.undo());
        QVERIFY(seen(finished));
        QCOMPARE(TagWriter::readFile(m.music + "/Band/Album/01 One.flac").value("title"), QString("One"));
        QVERIFY(quiet());

        // An organise into folders it makes, removing the one it empties.
        OrganiseModel *org = c.organiser();
        QVERIFY(org->load({one, two}));
        org->setMask("<Artist> Sorted/<Album>/<Track#:2> <Title>");
        org->replan();
        QCOMPARE(org->moves(), 2);
        finished.clear();
        QVERIFY(org->apply());
        QVERIFY(seen(finished));
        QVERIFY(QFile::exists(m.music + "/Band Sorted/Album/01 One.flac"));
        QVERIFY(!QDir(m.music + "/Band/Album").exists());
        QVERIFY(quiet());
        // The folders it made are watched, so later changes in them count.
        QTRY_COMPARE(w->watchedFolders(), 3); // music, Band Sorted and its Album

        // Its undo puts the files and the folders back.
        finished.clear();
        QVERIFY(c.undo());
        QVERIFY(seen(finished));
        QVERIFY(QFile::exists(m.music + "/Band/Album/01 One.flac"));
        QVERIFY(QFile::exists(m.music + "/Band/Album/02 Two.flac"));
        QVERIFY(quiet());

        // The trash.
        c.setTrashFunction([&](const QString &path, QString *) {
            QDir().mkpath(m.root + "/trash");
            return QFile::rename(path, m.root + "/trash/" + QFileInfo(path).fileName());
        });
        finished.clear();
        QVERIFY(c.moveToTrash({two}));
        QVERIFY(seen(finished));
        QVERIFY(QFile::exists(m.root + "/trash/02 Two.flac"));
        QVERIFY(quiet());
        QCOMPARE(w->rescansStarted(), 0);

        // A change from outside still starts one.
        QSignalSpy rescanned(w, &Watcher::rescanFinished);
        QVERIFY(m.add("Band/Album/03 Three.flac", {"title=Three", "artist=Band", "album=Album", "track=3"}));
        QVERIFY(seen(rescanned, 10000));
        QCOMPARE(rescanned.first().at(0).toJsonObject().value("added").toInt(), 1);
        QCOMPARE(started.size(), 1);
        // So does an outside edit of a file we wrote, once the note has run out.
        OwnWrites::clear();
        rescanned.clear();
        {
            QFile f(m.music + "/Band/Album/01 One.flac");
            QVERIFY(f.open(QIODevice::Append));
            f.write(QByteArray(16, '\0'));
        }
        QVERIFY(seen(rescanned, 10000));
        QCOMPARE(rescanned.first().at(0).toJsonObject().value("updated").toInt(), 1);
        QCOMPARE(w->rescansStarted(), 2);
        // Nothing temporary is left behind.
        for (const QString &f : files(m.music))
            QVERIFY2(!QFileInfo(f).fileName().startsWith(OwnWrites::temporaryPrefix), qPrintable(f));
    }

    void ownWritesAreNotedForALittleWhile() {
        OwnWrites::setLifetime(200);
        QTemporaryDir dir;
        const QString folder = dir.filePath("a/b/c");
        OwnWrites::noteFolder(folder);
        QVERIFY(OwnWrites::isOwn(folder));
        QVERIFY(OwnWrites::isOwn(dir.filePath("a/b")));
        QVERIFY(OwnWrites::isOwn(dir.filePath("a")));
        QVERIFY(!OwnWrites::isOwn(dir.path())); // it was there already
        OwnWrites::note(dir.filePath("x//y.flac"));
        QVERIFY(OwnWrites::isOwn(dir.filePath("x/y.flac")));
        QVERIFY(!OwnWrites::isOwn(dir.filePath("x/z.flac")));
        QTest::qWait(300);
        QVERIFY(!OwnWrites::isOwn(folder));
        QVERIFY(!OwnWrites::isOwn(dir.filePath("x/y.flac")));
        OwnWrites::setLifetime(5000);
    }

    // Jobs asked for while another runs wait their turn, in order, and the
    // jobs bar says so; an undo waiting behind a save undoes that save. The
    // watcher holds its rescans meanwhile and runs them after.
    void userJobsWaitTheirTurnAndRescansWait() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(twoTracks(m));
        LibraryController &c = openWatching(m);
        Watcher *w = c.watcher();
        QVERIFY(w);
        QStringList order;
        connect(&c, &LibraryController::jobFinished, &c, [&](const QString &kind, const QJsonObject &) { order << "job:" + kind; });
        connect(w, &Watcher::rescanStarted, &c, [&] { order << "rescan"; });
        connect(w, &Watcher::rescanFinished, &c, [&](const QJsonObject &r) {
            order << (r.value("yielded").toBool() ? "yielded" : "rescanned");
        });

        // A long job holds the slot, as a big rescan of everything would.
        std::atomic_bool release = false;
        QVERIFY(c.runJob("scan", "Blocking", [&](Library &, Job *) {
            while (!release)
                QThread::msleep(5);
            return QJsonObject{{"ok", true}};
        }, {}));
        QVERIFY(c.busy());
        QVERIFY(w->isHeld());
        QVERIFY(c.waitingText().isEmpty());

        // A title edit and Ctrl+Z meanwhile: neither is refused.
        const qint64 one = m.id("01 One.flac");
        QVERIFY(c.tagEditor()->quickSet(one, "title", "Uno"));
        QVERIFY(c.undo());
        QCOMPARE(c.queuedJobs(), 2);
        QVERIFY(c.waitingText().startsWith("Waiting for the scan to finish"));
        QVERIFY(c.waitingText().contains("2 jobs waiting"));
        QVERIFY(c.message().isEmpty());

        // A change from outside is noticed, but its rescan waits too.
        QVERIFY(m.add("Band/Album/03 Three.flac", {"title=Three", "artist=Band", "album=Album", "track=3"}));
        QTest::qWait(500);
        QCOMPARE(w->rescansStarted(), 0);

        release = true;
        QTRY_COMPARE_WITH_TIMEOUT(order.size(), 5, 30000);
        QCOMPARE(order, (QStringList{"job:scan", "job:tag", "job:undo", "rescan", "rescanned"}));
        QVERIFY(!c.busy());
        QVERIFY(!w->isHeld());
        QCOMPARE(c.queuedJobs(), 0);
        // The undo waited for the save, so it undid the save.
        QCOMPARE(TagWriter::readFile(m.music + "/Band/Album/01 One.flac").value("title"), QString("One"));
        QCOMPARE(c.undoText(), QString());
        QVERIFY(!c.redoText().isEmpty());
    }

    // A rescan already running when a job of the window's starts gives way:
    // it stops, and runs again in full once the job is done.
    void aRunningRescanGivesWay() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        Music m;
        QVERIFY(twoTracks(m));
        LibraryController &c = openWatching(m);
        Watcher *w = c.watcher();
        QVERIFY(w);
        QStringList order;
        std::atomic_bool release = false;
        bool blocked = false;
        connect(&c, &LibraryController::jobFinished, &c, [&](const QString &kind, const QJsonObject &) { order << "job:" + kind; });
        // The job starts the moment the rescan does.
        connect(w, &Watcher::rescanStarted, &c, [&] {
            order << "rescan";
            if (blocked)
                return;
            blocked = true;
            QVERIFY(c.runJob("tag", "Blocking", [&](Library &, Job *) {
                while (!release)
                    QThread::msleep(5);
                return QJsonObject{{"ok", true}};
            }, {}));
        });
        QList<QJsonObject> results;
        connect(w, &Watcher::rescanFinished, &c, [&](const QJsonObject &r) {
            results << r;
            order << (r.value("yielded").toBool() ? "yielded" : "rescanned");
        });

        QVERIFY(m.add("Band/Album/03 Three.flac", {"title=Three", "artist=Band", "album=Album", "track=3"}));
        QTRY_COMPARE_WITH_TIMEOUT(order.size(), 2, 10000);
        QCOMPARE(order, (QStringList{"rescan", "yielded"}));
        QVERIFY(results.first().value("cancelled").toBool());
        QVERIFY(w->isHeld());
        QTest::qWait(300);
        QCOMPARE(order.size(), 2); // held: no rerun yet

        release = true;
        QTRY_COMPARE_WITH_TIMEOUT(order.size(), 5, 30000);
        QCOMPARE(order, (QStringList{"rescan", "yielded", "job:tag", "rescan", "rescanned"}));
        QCOMPARE(results.last().value("added").toInt(), 1);
        QCOMPARE(w->rescansStarted(), 2);
    }
};

int runTidyWindowTests(int argc, char **argv) {
    TidyWindowTests tests;
    TidyWindowKeysTests keys;
    OwnWritesTests own;
    return QTest::qExec(&tests, argc, argv) + QTest::qExec(&keys, argc, argv) + QTest::qExec(&own, argc, argv);
}

#include "tidywindow_tests.moc"
