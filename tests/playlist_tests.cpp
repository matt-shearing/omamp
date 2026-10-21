// Phase 3 core: playlists, auto-playlists, collections, playlist files, play
// statistics, Auto-DJ, party mode and the MediaMonkey importer, with their
// commands. Every library, playlist file and MediaMonkey database is made up
// and lives in a temporary folder. Nothing is played.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include "autodj.h"
#include "cli.h"
#include "fixture_library.h"
#include "library.h"
#include "m3u.h"
#include "mmimport.h"
#include "playlists.h"
#include "stats.h"

namespace {

QVariant value(Library &library, const QString &sql) {
    QSqlQuery q(library.db());
    if (!q.exec(sql) || !q.next())
        return {};
    return q.value(0);
}

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

bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

double toOle(qint64 unixSeconds) { return unixSeconds / 86400.0 + 25569.0; }

// A library filled with the fixture rows, its one volume rooted in a
// temporary folder so playlist files can point at it with relative paths.
struct Lib {
    QTemporaryDir dir;
    QString root;
    std::unique_ptr<Library> library;
    Lib() {
        root = dir.filePath("music");
        QDir().mkpath(root);
        library = std::make_unique<Library>(dir.filePath("library.db"));
        QString error;
        if (!library->open(&error))
            qFatal("%s", qPrintable(error));
        if (!Fixture::fill(*library))
            qFatal("fixture");
        QSqlQuery q(library->db());
        q.prepare("UPDATE volumes SET root_path = ?");
        q.addBindValue(root);
        q.exec();
    }
    QString db() const { return library->path(); }
    qint64 track(const QString &title) {
        QSqlQuery q(library->db());
        q.prepare("SELECT id FROM tracks WHERE title = ?");
        q.addBindValue(title);
        return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
    }
    QString path(const QString &title) {
        QSqlQuery q(library->db());
        q.prepare("SELECT rel_path FROM tracks WHERE title = ?");
        q.addBindValue(title);
        return q.exec() && q.next() ? root + '/' + q.value(0).toString() : QString();
    }
    QVariant field(qint64 id, const QString &column) {
        return value(*library, QString("SELECT %1 FROM tracks WHERE id = %2").arg(column).arg(id));
    }
};

QStringList titles(const QList<Playlists::Item> &items) {
    QStringList out;
    for (const Playlists::Item &it : items)
        out << it.title;
    return out;
}

// ---- A MediaMonkey-style database ------------------------------------------
//
// Built from MediaMonkey 4's documented schema (see docs/mediamonkey-import.md
// for the columns assumed and which are unverified). The text columns use the
// IUNICODE and NUMERICSTRING collations, as MediaMonkey's do, so a reader that
// doesn't register them fails.

int testCollation(void *, int la, const void *a, int lb, const void *b) {
    return QString::fromUtf8(static_cast<const char *>(a), la)
        .compare(QString::fromUtf8(static_cast<const char *>(b), lb), Qt::CaseInsensitive);
}

struct MmBuilder {
    sqlite3 *db = nullptr;
    explicit MmBuilder(const QString &path) {
        sqlite3_open(QFile::encodeName(path).constData(), &db);
        sqlite3_create_collation(db, "IUNICODE", SQLITE_UTF8, nullptr, testCollation);
        sqlite3_create_collation(db, "NUMERICSTRING", SQLITE_UTF8, nullptr, testCollation);
    }
    ~MmBuilder() { sqlite3_close(db); }
    bool exec(const QString &sql) {
        char *message = nullptr;
        if (sqlite3_exec(db, sql.toUtf8().constData(), nullptr, nullptr, &message) != SQLITE_OK) {
            qWarning() << message << sql.left(120);
            sqlite3_free(message);
            return false;
        }
        return true;
    }
    bool exec(const QString &sql, const QVariantList &params) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &st, nullptr) != SQLITE_OK) {
            qWarning() << sqlite3_errmsg(db);
            return false;
        }
        QList<QByteArray> keep;
        for (int i = 0; i < params.size(); ++i) {
            const QVariant &v = params[i];
            if (v.isNull())
                sqlite3_bind_null(st, i + 1);
            else if (v.typeId() == QMetaType::Double)
                sqlite3_bind_double(st, i + 1, v.toDouble());
            else if (v.typeId() == QMetaType::Int || v.typeId() == QMetaType::LongLong)
                sqlite3_bind_int64(st, i + 1, v.toLongLong());
            else {
                keep << v.toString().toUtf8();
                sqlite3_bind_text(st, i + 1, keep.last().constData(), keep.last().size(), SQLITE_STATIC);
            }
        }
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok)
            qWarning() << sqlite3_errmsg(db);
        sqlite3_finalize(st);
        return ok;
    }
};

const QVariant none;

// MediaMonkey 4: five songs (two by mapped path, one by folder and file name,
// one by tags, one nowhere), play history and four playlists.
bool makeMm4(const QString &path, qint64 lastPlayed, qint64 added) {
    MmBuilder mm(path);
    bool ok = mm.exec("CREATE TABLE Medias (IDMedia INTEGER PRIMARY KEY, DriveLetter INTEGER, Label TEXT COLLATE IUNICODE)") &&
              mm.exec("INSERT INTO Medias VALUES (1, 3, 'MUSIC')") &&
              mm.exec("CREATE TABLE Songs (ID INTEGER PRIMARY KEY, SongPath TEXT COLLATE IUNICODE, IDMedia INTEGER, "
                      "SongTitle TEXT COLLATE IUNICODE, Artist TEXT COLLATE IUNICODE, AlbumArtist TEXT COLLATE IUNICODE, "
                      "Album TEXT COLLATE IUNICODE, Genre TEXT COLLATE IUNICODE, Year INTEGER, "
                      "TrackNumber TEXT COLLATE NUMERICSTRING, SongLength INTEGER, Rating INTEGER, PlayCounter INTEGER, "
                      "SkipCount INTEGER, LastTimePlayed REAL, DateAdded REAL, Custom1 TEXT COLLATE IUNICODE, "
                      "Custom2 TEXT, Custom3 TEXT, Custom4 TEXT, Custom5 TEXT)") &&
              mm.exec("CREATE INDEX SongsArtist ON Songs (Artist COLLATE IUNICODE)");
    const QString insert = "INSERT INTO Songs (ID, SongPath, IDMedia, SongTitle, Artist, Album, SongLength, Rating, "
                           "PlayCounter, SkipCount, LastTimePlayed, DateAdded, Custom1, TrackNumber) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    ok = ok &&
         mm.exec(insert, {1, ":\\Music\\Night Drive\\1-01 Midnight Highway.flac", 1, "Midnight Highway", "Neon Coast",
                          "Night Drive", 245000, 100, 40, 2, toOle(lastPlayed), toOle(added), "Vinyl rip", "1"}) &&
         mm.exec(insert, {2, "\\\\nas\\music\\Summer Mix\\01 Heatwave.flac", 0, "Heatwave", "Sun Machine", "Summer Mix",
                          200000, 60, 1, none, none, none, none, "1"}) &&
         mm.exec(insert, {3, "E:\\Old\\Live at Home\\01 Love Song.flac", 0, "Love Song", "The Lovers", "Live at Home",
                          600000, 40, 50, none, none, none, none, "1"}) &&
         mm.exec(insert, {4, "F:\\x\\Go West.mp3", 0, "Go West", "Pet Dogs", "Go", 291000, 80, 7, none, none, none,
                          none, "10"}) &&
         mm.exec(insert, {5, "D:\\Music\\Nobody\\Ghost.mp3", 0, "Ghost", "Nobody", "", 100000, -1, 3, none, none, none,
                          none, "2"});
    ok = ok && mm.exec("CREATE TABLE Played (IDPlayed INTEGER PRIMARY KEY, IDSong INTEGER, PlayDate REAL)") &&
         mm.exec("INSERT INTO Played (IDSong, PlayDate) VALUES (?, ?)", {1, toOle(lastPlayed)}) &&
         mm.exec("INSERT INTO Played (IDSong, PlayDate) VALUES (?, ?)", {1, toOle(lastPlayed - 86400)}) &&
         mm.exec("INSERT INTO Played (IDSong, PlayDate) VALUES (?, ?)", {4, toOle(lastPlayed - 3600)}) &&
         mm.exec("INSERT INTO Played (IDSong, PlayDate) VALUES (?, ?)", {5, toOle(lastPlayed - 7200)});
    ok = ok &&
         mm.exec("CREATE TABLE Playlists (IDPlaylist INTEGER PRIMARY KEY, PlaylistName TEXT COLLATE IUNICODE, "
                 "ParentPlaylist INTEGER, isAutoPlaylist INTEGER, QueryData TEXT)") &&
         mm.exec("INSERT INTO Playlists VALUES (10, 'Road', 0, 0, NULL)") &&
         mm.exec("INSERT INTO Playlists VALUES (11, 'Summer', 10, 0, NULL)") &&
         mm.exec("INSERT INTO Playlists VALUES (12, 'Best synth', 0, 1, "
                 "'Rating >= 80 AND Genre LIKE ''%Synth%'' ORDER BY Rating DESC LIMIT 10')") &&
         mm.exec("INSERT INTO Playlists VALUES (13, 'Weird', 0, 1, 'SongPath LIKE ''D:%''')") &&
         mm.exec("CREATE TABLE PlaylistSongs (IDPlaylistSong INTEGER PRIMARY KEY, IDPlaylist INTEGER, IDSong INTEGER, "
                 "SongOrder INTEGER)");
    const QList<QVariantList> entries = {{11, 4, 0}, {11, 1, 1}, {11, 5, 2}, {11, 4, 3}, {13, 2, 0}};
    for (const QVariantList &e : entries)
        ok = ok && mm.exec("INSERT INTO PlaylistSongs (IDPlaylist, IDSong, SongOrder) VALUES (?, ?, ?)", e);
    return ok;
}

QByteArray sha(const QString &path) { return QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256); }

} // namespace

class PlaylistTests : public QObject {
    Q_OBJECT

private slots:
    // ---- Schema ------------------------------------------------------------

    void schemaSevenKeepsOldPlaylists() {
        QTemporaryDir dir;
        const QString path = dir.filePath("library.db");
        {
            Library old(path);
            QVERIFY(old.open(nullptr, 6));
            QSqlQuery q(old.db());
            QVERIFY(q.exec("INSERT INTO volumes (id, key, kind, root_path, added) VALUES (1, 'mount:x', 'local', '/m', 0)"));
            QVERIFY(q.exec("INSERT INTO tracks (id, volume_id, rel_path, file_name, size, mtime_ns, title, added) "
                           "VALUES (1, 1, 'a.flac', 'a', 1, 1, 'A', 0)"));
            QVERIFY(q.exec("INSERT INTO playlists (id, name, created) VALUES (1, 'Old', 0)"));
            QVERIFY(q.exec("INSERT INTO playlist_items (playlist_id, track_id, position) VALUES (1, 1, 0), (1, 1, 1)"));
        }
        Library library(path);
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QVERIFY(Library::latestVersion() >= 7);
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QCOMPARE(value(library, "SELECT count(*) FROM playlist_items WHERE playlist_id = 1").toInt(), 2);
        QCOMPARE(value(library, "SELECT count(*) FROM sqlite_master WHERE name = 'import_journal'").toInt(), 1);
        // An entry now outlives its track.
        QVERIFY(QSqlQuery(library.db()).exec("DELETE FROM tracks WHERE id = 1"));
        QCOMPARE(value(library, "SELECT count(*) FROM playlist_items WHERE track_id IS NULL").toInt(), 2);
    }

    // ---- Static playlists --------------------------------------------------

    void staticPlaylists() {
        Lib l;
        Library &lib = *l.library;
        QString error;
        const qint64 folder = Playlists::createFolder(lib, "Road trips");
        const qint64 summer = Playlists::create(lib, "Summer", folder, &error);
        QVERIFY2(summer, qPrintable(error));
        QVERIFY(!Playlists::create(lib, "   "));
        const qint64 coast = l.track("Coastline"), heat = l.track("Heatwave"), tidal = l.track("Tidal");
        // Duplicates are allowed.
        QCOMPARE(Playlists::addTracks(lib, summer, {coast, heat, coast}), 3);
        QCOMPARE(titles(Playlists::items(lib, summer)), (QStringList{"Coastline", "Heatwave", "Coastline"}));
        // Insert at a position.
        QCOMPARE(Playlists::addTracks(lib, summer, {tidal}, 1), 1);
        QCOMPARE(titles(Playlists::items(lib, summer)), (QStringList{"Coastline", "Tidal", "Heatwave", "Coastline"}));
        QCOMPARE(Playlists::addTracks(lib, summer, {999999}, -1, &error), -1);
        QVERIFY(error.contains("not in the library"));
        // Move the last entry to the front, then remove the second.
        QList<Playlists::Item> items = Playlists::items(lib, summer);
        QVERIFY(Playlists::moveItems(lib, summer, {items[3].id}, 0));
        QCOMPARE(titles(Playlists::items(lib, summer)), (QStringList{"Coastline", "Coastline", "Tidal", "Heatwave"}));
        QCOMPARE(Playlists::removePositions(lib, summer, {1}), 1);
        QCOMPARE(titles(Playlists::items(lib, summer)), (QStringList{"Coastline", "Tidal", "Heatwave"}));
        items = Playlists::items(lib, summer);
        QCOMPARE(items[2].position, 2);
        QVERIFY(Playlists::reorder(lib, summer, {items[2].id, items[0].id, items[1].id}));
        QCOMPARE(titles(Playlists::items(lib, summer)), (QStringList{"Heatwave", "Coastline", "Tidal"}));
        QVERIFY(!Playlists::reorder(lib, summer, {items[0].id}, &error));
        // Positions stay 0..n-1 in the table.
        QCOMPARE(value(lib, QString("SELECT group_concat(position) FROM (SELECT position FROM playlist_items "
                                    "WHERE playlist_id = %1 ORDER BY position)")
                                .arg(summer))
                     .toString(),
                 QString("0,1,2"));

        // Names and paths resolve.
        QCOMPARE(Playlists::resolve(lib, "Road trips/Summer"), summer);
        QCOMPARE(Playlists::resolve(lib, "summer"), summer);
        QCOMPARE(Playlists::resolve(lib, QString::number(summer)), summer);
        QCOMPARE(Playlists::resolve(lib, "Nope", &error), 0);
        Playlists::create(lib, "Summer");
        QCOMPARE(Playlists::resolve(lib, "Summer", &error), 0);
        QVERIFY(error.contains("2 playlists"));

        // Tree order: parents first, then children.
        const QList<Playlists::Playlist> all = Playlists::list(lib);
        QCOMPARE(all.size(), 3);
        QCOMPARE(all[0].name, QString("Road trips"));
        QCOMPARE(all[1].parentId, folder);
        // A folder can't take tracks, and a playlist can't go inside itself.
        QCOMPARE(Playlists::addTracks(lib, folder, {coast}, -1, &error), -1);
        QVERIFY(error.contains("folder"));
        QVERIFY(!Playlists::move(lib, folder, summer, -1, &error));
        QVERIFY(error.contains("itself"));
        QVERIFY(Playlists::move(lib, summer, 0, 0));
        QCOMPARE(Playlists::children(lib, 0).first().id, summer);
        QVERIFY(Playlists::rename(lib, summer, "Summer 2026"));

        // A track that leaves the library stays in the list with its path.
        const QString tidalPath = l.path("Tidal");
        QVERIFY(QSqlQuery(lib.db()).exec(QString("DELETE FROM tracks WHERE id = %1").arg(tidal)));
        items = Playlists::items(lib, summer);
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[2].trackId, 0);
        QCOMPARE(items[2].availability, QString("unmatched"));
        QCOMPARE(items[2].path, tidalPath);
        QCOMPARE(items[2].title, QString("Tidal"));
        QCOMPARE(Playlists::trackIds(lib, summer).size(), 2);
        // A missing track is still listed, as missing.
        QVERIFY(QSqlQuery(lib.db()).exec(QString("UPDATE tracks SET availability = 'missing' WHERE id = %1").arg(heat)));
        QCOMPARE(Playlists::items(lib, summer).first().availability, QString("missing"));

        // Deleting a folder deletes what is in it.
        const qint64 inner = Playlists::create(lib, "Inner", folder);
        Playlists::addTracks(lib, inner, {coast});
        QVERIFY(Playlists::remove(lib, folder));
        QVERIFY(!Playlists::get(lib, inner, nullptr));
        QCOMPARE(value(lib, QString("SELECT count(*) FROM playlist_items WHERE playlist_id = %1").arg(inner)).toInt(), 0);
        QVERIFY(Playlists::clear(lib, summer));
        QCOMPARE(Playlists::items(lib, summer).size(), 0);
    }

    // ---- Auto-playlists ----------------------------------------------------

    void autoPlaylists() {
        Lib l;
        Library &lib = *l.library;
        QString error;
        QCOMPARE(Playlists::createAuto(lib, "Bad", "artst:x", 0, &error), 0);
        QVERIFY(error.contains("Unknown field"));
        const qint64 rock = Playlists::createAuto(lib, "Rock", "genre:rock sort:title");
        QVERIFY(rock);
        QCOMPARE(titles(Playlists::items(lib, rock)), (QStringList{"Glove Box", "Love Song", "Tidal"}));
        // Live: a new rock track shows up without touching the playlist.
        QVERIFY(QSqlQuery(lib.db()).exec("UPDATE tracks SET genre = 'Rock' WHERE title = 'Heatwave'"));
        QCOMPARE(Playlists::items(lib, rock).size(), 4);
        QCOMPARE(Playlists::addTracks(lib, rock, {1}, -1, &error), -1);
        QVERIFY(error.contains("auto-playlist"));
        QVERIFY(Playlists::setQuery(lib, rock, "genre:rock year:<2000"));
        QCOMPARE(Playlists::items(lib, rock).size(), 2);
        QVERIFY(!Playlists::setQuery(lib, rock, "year:soon", &error));

        // Random with a limit: the same pick until it is rerolled.
        const qint64 random = Playlists::createAuto(lib, "Five at random", "type:music sort:random limit:5");
        const QList<qint64> first = Playlists::trackIds(lib, random);
        QCOMPARE(first.size(), 5);
        QCOMPARE(Playlists::trackIds(lib, random), first);
        bool changed = false;
        for (qint64 seed = 1; seed < 20 && !changed; ++seed) {
            QCOMPARE(Playlists::reroll(lib, random, seed), seed);
            changed = Playlists::trackIds(lib, random) != first;
        }
        QVERIFY(changed);
        QCOMPARE(Playlists::reroll(lib, rock - 1000, 3), -1);
        Playlists::Playlist p;
        QVERIFY(Playlists::get(lib, random, &p));
        QCOMPARE(p.kind, QString("auto"));
        QCOMPARE(p.toJson().value("query").toString(), QString("type:music sort:random limit:5"));
    }

    // ---- Collections -------------------------------------------------------

    void collections() {
        Lib l;
        Library &lib = *l.library;
        QString error;
        QList<Collections::Collection> all = Collections::list(lib);
        QCOMPARE(all.size(), 4);
        QCOMPARE(all[0].name, QString("Music"));
        const qint64 books = Collections::resolve(lib, "audiobooks");
        QCOMPARE(Collections::trackIds(lib, books).size(), 1);
        QCOMPARE(Collections::trackIds(lib, Collections::resolve(lib, "Music")).size(), 11);
        QVERIFY(Collections::remove(lib, books));
        QCOMPARE(Collections::ensureDefaults(lib), 1);
        QCOMPARE(Collections::ensureDefaults(lib), 0);

        Collections::Collection c;
        c.name = "Synth";
        c.query = "genre:synthwave";
        c.columns = QJsonArray{"title", "artist", "rating"};
        c.tree = QJsonArray{"artist", "album"};
        c.sort = "-rating";
        c.view = "grid";
        const qint64 id = Collections::create(lib, c, &error);
        QVERIFY2(id, qPrintable(error));
        QVERIFY(!Collections::create(lib, c, &error));
        QVERIFY(error.contains("already"));
        Collections::Collection back;
        QVERIFY(Collections::get(lib, id, &back));
        QCOMPARE(back.columns, c.columns);
        QCOMPARE(back.tree, c.tree);
        QCOMPARE(back.view, QString("grid"));
        // The collection's own sort applies: best rated first, unrated last.
        const QList<qint64> ids = Collections::trackIds(lib, id);
        QCOMPARE(ids.size(), 3);
        QCOMPARE(ids.first(), l.track("Coastline"));
        back.query = "genre:pop";
        back.visible = "nonempty";
        QVERIFY(Collections::update(lib, back));
        back.visible = "sometimes";
        QVERIFY(!Collections::update(lib, back, &error));
        back.visible = "always";
        back.query = "nope:x";
        QVERIFY(!Collections::update(lib, back, &error));
    }

    // ---- Playlist files ----------------------------------------------------

    void m3uImportAndExport() {
        Lib l;
        Library &lib = *l.library;
        const QString lists = l.dir.filePath("lists");
        // Relative and absolute paths, a file:// URL, an entry known only by
        // its #EXTINF artist and title, a stream and a file that isn't there.
        const QByteArray m3u = "\xEF\xBB\xBF#EXTM3U\n"
                               "#EXTINF:245,Neon Coast - Midnight Highway\n"
                               "../music/Night Drive/1-01 Midnight Highway.flac\n"
                               "#EXTINF:185,Neon Coast - Coastline\n" +
                               l.path("Coastline").toUtf8() +
                               "\n"
                               "file://" +
                               QUrl::toPercentEncoding(l.path("Tidal"), "/") +
                               "\n"
                               "#EXTINF:290,Pet Dogs - Go West\n"
                               "D:\\Old Music\\Go West.mp3\n"
                               "#EXTINF:-1,Radio\n"
                               "http://radio.example/stream\n"
                               "#EXTINF:100,Nobody - Ghost\n"
                               "../elsewhere/ghost.mp3\n"
                               "../music/Night Drive/1-01 Midnight Highway.flac\n";
        QVERIFY(writeFile(lists + "/Mixed.m3u8", m3u));
        const PlaylistFiles::ImportResult r = PlaylistFiles::import(lib, lists + "/Mixed.m3u8");
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.name, QString("Mixed"));
        QCOMPARE(r.entries, 7);
        QCOMPARE(r.byPath, 4);
        QCOMPARE(r.byTags, 1);
        QCOMPARE(r.unmatched, 2);
        const QList<Playlists::Item> items = Playlists::items(lib, r.playlistId);
        QCOMPARE(items.size(), 7);
        QCOMPARE(items[0].trackId, l.track("Midnight Highway"));
        QCOMPARE(items[3].trackId, l.track("Go West"));
        QCOMPARE(items[4].availability, QString("unmatched"));
        QCOMPARE(items[4].path, QString("http://radio.example/stream"));
        QCOMPARE(items[5].path, l.dir.filePath("elsewhere/ghost.mp3"));
        QCOMPARE(items[5].artist, QString("Nobody"));
        QCOMPARE(items[5].durationMs, 100000);
        QCOMPARE(items[6].trackId, items[0].trackId); // a duplicate stays

        // Export with relative paths and read it back: the same entries.
        const QString out = lists + "/out/Mixed again.m3u8";
        PlaylistFiles::ExportOptions relative;
        relative.relative = true;
        int written = 0;
        QString error;
        QDir().mkpath(lists + "/out");
        QVERIFY2(PlaylistFiles::exportFile(lib, r.playlistId, out, relative, &written, &error), qPrintable(error));
        QCOMPARE(written, 7);
        const QString text = QString::fromUtf8(readFile(out));
        QVERIFY(text.startsWith("#EXTM3U\n#EXTINF:245,Neon Coast - Midnight Highway\n../../music/Night Drive/"));
        QVERIFY(text.contains("http://radio.example/stream"));
        const PlaylistFiles::Parsed again = PlaylistFiles::read(out);
        QCOMPARE(again.entries.size(), 7);
        for (int i = 0; i < 7; ++i)
            QCOMPARE(again.entries[i].path, items[i].path);
        // Absolute paths, and PLS.
        QVERIFY(PlaylistFiles::exportFile(lib, r.playlistId, lists + "/abs.m3u", {}, &written));
        QVERIFY(QString::fromUtf8(readFile(lists + "/abs.m3u")).contains('\n' + l.path("Coastline") + '\n'));
        QVERIFY(PlaylistFiles::exportFile(lib, r.playlistId, lists + "/Mixed.pls", {}, &written));
        const PlaylistFiles::Parsed pls = PlaylistFiles::read(lists + "/Mixed.pls");
        QVERIFY(pls.ok());
        QCOMPARE(pls.entries.size(), 7);
        QCOMPARE(pls.entries[1].title, QString("Coastline"));
        QCOMPARE(pls.entries[1].artist, QString("Neon Coast"));
        QCOMPARE(pls.entries[1].durationMs, 185000);
        const PlaylistFiles::ImportResult fromPls = PlaylistFiles::import(lib, lists + "/Mixed.pls", 0, "From PLS");
        QCOMPARE(fromPls.byPath + fromPls.byTags, 5);

        // A Latin-1 file with no #EXTINF and Windows separators in a relative path.
        QVERIFY(writeFile(lists + "/old.m3u", QByteArray("..\\music\\Summer Mix\\01 Heatwave.flac\r\n") +
                                                   QByteArray("caf\xE9.mp3\r\n")));
        const PlaylistFiles::Parsed latin = PlaylistFiles::read(lists + "/old.m3u");
        QCOMPARE(latin.entries.size(), 2);
        QCOMPARE(latin.entries[0].path, l.path("Heatwave"));
        QCOMPARE(latin.entries[1].path, lists + QString::fromUtf8("/caf\u00e9.mp3"));
        QVERIFY(!PlaylistFiles::import(lib, lists + "/none.m3u").ok());

        // A folder of playlists becomes a folder of playlists.
        const QString tree = l.dir.filePath("tree");
        QVERIFY(writeFile(tree + "/A.m3u", "../music/Summer Mix/01 Heatwave.flac\n"));
        QVERIFY(writeFile(tree + "/sub/B.pls", "[playlist]\nFile1=../../music/Summer Mix/03 Tidal.flac\n"));
        QVERIFY(writeFile(tree + "/empty/readme.txt", "nothing"));
        qint64 folderId = 0;
        const QList<PlaylistFiles::ImportResult> all =
            PlaylistFiles::importFolder(lib, tree, 0, {}, &folderId, &error);
        QCOMPARE(all.size(), 2);
        QVERIFY(folderId);
        const QList<Playlists::Playlist> kids = Playlists::children(lib, folderId);
        QCOMPARE(kids.size(), 2); // A and the sub folder; "empty" holds no playlists
        QCOMPARE(Playlists::resolve(lib, "tree/sub/B"), all[1].playlistId);
        QCOMPARE(Playlists::trackIds(lib, all[1].playlistId), QList<qint64>{l.track("Tidal")});
    }

    // ---- Play statistics ---------------------------------------------------

    void playRules_data() {
        QTest::addColumn<qint64>("played");
        QTest::addColumn<qint64>("duration");
        QTest::addColumn<bool>("skipped");
        QTest::addColumn<QString>("outcome");
        QTest::newRow("half") << 100000LL << 200000LL << false << "counted";
        QTest::newRow("just under half") << 99999LL << 200000LL << false << "ignored";
        QTest::newRow("four minutes of an hour") << 240000LL << 3600000LL << false << "counted";
        QTest::newRow("under a tenth") << 19999LL << 200000LL << false << "skipped";
        QTest::newRow("skipped at a third") << 70000LL << 200000LL << true << "skipped";
        QTest::newRow("skipped after half still counts") << 150000LL << 200000LL << true << "counted";
        QTest::newRow("unknown length, short") << 60000LL << 0LL << false << "ignored";
        QTest::newRow("unknown length, user skip") << 60000LL << 0LL << true << "skipped";
    }

    void playRules() {
        QFETCH(qint64, played);
        QFETCH(qint64, duration);
        QFETCH(bool, skipped);
        QFETCH(QString, outcome);
        Stats::PlayResult r;
        r.outcome = Stats::classify(played, duration, skipped);
        QCOMPARE(r.outcomeName(), outcome);
    }

    void playStatistics() {
        Lib l;
        Library &lib = *l.library;
        const qint64 go = l.track("Go West"); // 290 s, never played
        const qint64 now = Fixture::now.toSecsSinceEpoch();
        QCOMPARE(Stats::trackIdForPath(lib, l.path("Go West")), go);
        QCOMPARE(Stats::trackIdForPath(lib, l.root + "/Go/nothing.flac"), 0);
        QCOMPARE(Stats::trackIdForPath(lib, l.root + "x/Go/01 Go West.flac"), 0);
        // Counted: heard 3 minutes of 4:50.
        Stats::PlayResult r = Stats::recordPlay(lib, go, 180000, 0, false, now - 60);
        QCOMPARE(r.outcomeName(), QString("counted"));
        QVERIFY(r.historyId);
        QCOMPARE(l.field(go, "play_count").toInt(), 1);
        QCOMPARE(l.field(go, "last_played").toLongLong(), now - 60);
        // An older play doesn't move last_played back.
        Stats::recordPlay(lib, go, 290000, 0, false, now - 3600);
        QCOMPARE(l.field(go, "play_count").toInt(), 2);
        QCOMPARE(l.field(go, "last_played").toLongLong(), now - 60);
        // Skipped, then ignored.
        r = Stats::recordPlay(lib, go, 5000, 0, false, now);
        QCOMPARE(r.outcomeName(), QString("skipped"));
        QCOMPARE(l.field(go, "skip_count").toInt(), 1);
        r = Stats::recordPlay(lib, go, 60000, 0, false, now);
        QCOMPARE(r.outcomeName(), QString("ignored"));
        QCOMPARE(r.historyId, 0);
        QCOMPARE(value(lib, QString("SELECT count(*) FROM play_history WHERE track_id = %1").arg(go)).toInt(), 3);
        QVERIFY(!Stats::recordPlay(lib, 999999, 1000).ok());

        // Most played of all time comes from the counters.
        Stats::TopOptions top;
        top.limit = 3;
        QList<Stats::Row> rows = Stats::top(lib, top);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].name, QString("Love Song"));
        QCOMPARE(rows[0].plays, 50);
        QCOMPARE(rows[1].name, QString("Coastline"));
        top.by = Stats::By::Artists;
        top.limit = 0;
        rows = Stats::top(lib, top);
        QCOMPARE(rows[0].name, QString("The Lovers"));
        QCOMPARE(rows[0].plays, 52);
        QCOMPARE(rows[0].tracks, 2);
        QCOMPARE(rows[1].name, QString("Neon Coast"));
        QCOMPARE(rows[1].plays, 42);
        top.by = Stats::By::Albums;
        rows = Stats::top(lib, top);
        QCOMPARE(rows[0].name, QString("Live at Home"));
        QCOMPARE(rows[0].detail, QString("The Lovers"));
        top.by = Stats::By::Years;
        rows = Stats::top(lib, top);
        QCOMPARE(rows[0].name, QString("1985"));
        top.by = Stats::By::Genres;
        rows = Stats::top(lib, top);
        QCOMPARE(rows[0].name, QString("Rock"));

        // Since a time: only play_history counts, so only Go West.
        top.by = Stats::By::Tracks;
        top.since = now - 7 * 86400;
        rows = Stats::top(lib, top);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].id, go);
        QCOMPARE(rows[0].plays, 2);
        QCOMPARE(rows[0].skips, 1);
        QCOMPARE(rows[0].listenedMs, 470000);
        top.by = Stats::By::Artists;
        QCOMPARE(Stats::top(lib, top).first().name, QString("Pet Dogs"));

        const QList<Stats::Played> recent = Stats::recent(lib, 10);
        QCOMPARE(recent.size(), 2);
        QCOMPARE(recent[0].playedAt, now - 60);
        QCOMPARE(Stats::recent(lib, 10, true).size(), 3);
        QVERIFY(!Stats::neverPlayed(lib, 0).contains(go));
        QCOMPARE(Stats::neverPlayed(lib, 0).size(), 4); // Afterglow, Snow Day, untagged, 夜の歌
        const Stats::Summary s = Stats::summary(lib, now - 86400);
        QCOMPARE(s.plays, 2);
        QCOMPARE(s.skips, 1);
        QCOMPARE(s.neverPlayed, 4);

        QCOMPARE(Stats::parseSince("30d", now), now - 30 * 86400);
        QCOMPARE(Stats::parseSince("2w", now), now - 14 * 86400);
        QCOMPARE(Stats::parseSince("1y", now), Fixture::now.addYears(-1).toSecsSinceEpoch());
        QCOMPARE(Stats::parseSince("2026-01-01", now), QDateTime(QDate(2026, 1, 1), QTime(0, 0)).toSecsSinceEpoch());
        QCOMPARE(Stats::parseSince("soon", now), -1);
    }

    // ---- Auto-DJ -----------------------------------------------------------

    void autoDj() {
        Lib l;
        Library &lib = *l.library;
        AutoDj::Settings settings;
        settings.skipPlayedWithinDays = 0;
        settings.artistSeparation = 2;
        // Repeatable with a seed.
        AutoDj::Dj a(lib, settings, 42), b(lib, settings, 42);
        const QList<AutoDj::Pick> first = a.pick(8), second = b.pick(8);
        QCOMPARE(first.size(), 8);
        QList<qint64> ids1, ids2;
        for (int i = 0; i < 8; ++i) {
            ids1 << first[i].trackId;
            ids2 << second[i].trackId;
        }
        QCOMPARE(ids1, ids2);
        // Audiobooks are never picked.
        QVERIFY(!ids1.contains(l.track("Chapter One")));
        QCOMPARE(a.candidates(), 11);
        // No artist twice within two tracks, while the rule can hold.
        for (int i = 1; i < first.size(); ++i)
            if (first[i].relaxed.isEmpty())
                for (int back = 1; back <= 2 && i - back >= 0; ++back)
                    QVERIFY2(first[i].artist.isEmpty() || first[i].artist != first[i - back].artist,
                             qPrintable(first[i].artist));
        // No track twice while there are others.
        QCOMPARE(QSet<qint64>(ids1.begin(), ids1.end()).size(), 8);

        // Played in the last week: Midnight Highway (3 days ago) and Tidal (1 day).
        settings.skipPlayedWithinDays = 7;
        AutoDj::Dj recent(lib, settings, 7);
        recent.setNow(Fixture::now.toSecsSinceEpoch());
        const QList<AutoDj::Pick> fresh = recent.pick(9);
        for (const AutoDj::Pick &p : fresh)
            if (p.relaxed.isEmpty() || !p.relaxed.contains("recent")) {
                QVERIFY(p.trackId != l.track("Midnight Highway"));
                QVERIFY(p.trackId != l.track("Tidal"));
            }

        // A source of two tracks by one artist still plays, relaxing the rules.
        settings.source = AutoDj::Source::parse("query:album:\"live at home\"");
        AutoDj::Dj tiny(lib, settings, 3);
        const QList<AutoDj::Pick> loop = tiny.pick(4);
        QCOMPARE(loop.size(), 4);
        QVERIFY(loop[1].relaxed.contains("artist"));
        QCOMPARE(loop[2].trackId, loop[0].trackId); // the two take turns
        QVERIFY(loop[2].relaxed.contains("artist"));

        // Refill tops the queue up to `upcoming`.
        settings.source = AutoDj::Source::parse("library");
        settings.upcoming = 5;
        AutoDj::Dj filler(lib, settings, 9);
        QCOMPARE(filler.refill({l.track("Coastline")}, 2).size(), 3);
        QCOMPARE(filler.refill({}, 5).size(), 0);

        // Sources: a collection and a playlist.
        const qint64 list = Playlists::create(lib, "Two");
        Playlists::addTracks(lib, list, {l.track("Heatwave"), l.track("Go West"), l.track("Heatwave")});
        QCOMPARE(AutoDj::sourceIds(lib, AutoDj::Source::parse("playlist:Two")).size(), 3);
        settings.source = AutoDj::Source::parse("playlist:Two");
        AutoDj::Dj fromList(lib, settings, 1);
        QCOMPARE(fromList.candidates(), 2);
        QCOMPARE(AutoDj::sourceIds(lib, AutoDj::Source::parse("collection:Audiobooks")).size(), 1);
        settings.source = AutoDj::Source::parse("collection:Audiobooks");
        AutoDj::Dj books(lib, settings, 1);
        QVERIFY(books.pick(1).isEmpty()); // audiobooks are excluded by type
        QVERIFY(!books.error().isEmpty());
        QCOMPARE(AutoDj::Source::parse("rating:4..").toString(), QString("query:rating:4.."));

        // Ratings weigh: five stars come up far more often than no stars.
        AutoDj::Settings weighted;
        weighted.source = AutoDj::Source::parse("query:title:=Coastline OR title:=\"Glove Box\"");
        weighted.artistSeparation = 0;
        weighted.skipPlayedWithinDays = 0;
        AutoDj::Dj w(lib, weighted, 11);
        int coast = 0, glove = 0;
        QList<qint64> none;
        for (int i = 0; i < 400; ++i) {
            const QList<AutoDj::Pick> one = w.pick(1, none);
            (one.first().trackId == l.track("Coastline") ? coast : glove)++;
        }
        QVERIFY2(coast > glove * 5, qPrintable(QString("%1 vs %2").arg(coast).arg(glove)));
        // Never played tracks get a boost.
        weighted.source = AutoDj::Source::parse("query:title:=Afterglow OR title:=\"Go West\" OR title:=Heatwave");
        weighted.weightByRating = false;
        weighted.neverPlayedBoost = 4;
        AutoDj::Dj boost(lib, weighted, 5);
        int never = 0;
        for (int i = 0; i < 300; ++i)
            never += boost.pick(1).first().trackId != l.track("Heatwave");
        QVERIFY(never > 240);

        // Settings round-trip through the library.
        settings.upcoming = 12;
        settings.save(lib);
        const AutoDj::Settings loaded = AutoDj::Settings::load(lib);
        QCOMPARE(loaded.upcoming, 12);
        QCOMPARE(loaded.source.toString(), QString("collection:Audiobooks"));
    }

    void partyMode() {
        Lib l;
        Library &lib = *l.library;
        PartyMode::Settings s = PartyMode::Settings::load(lib);
        QVERIFY(!s.enabled);
        QVERIFY(PartyMode::allowed(s, PartyMode::Action::EditTags));
        QVERIFY(PartyMode::enable(lib, "jukebox", "1234"));
        s = PartyMode::Settings::load(lib);
        QVERIFY(s.enabled);
        QVERIFY(s.hasPin());
        QVERIFY(!s.pinHash.contains("1234"));
        QVERIFY(!lib.setting("party_mode").toString().contains("1234"));
        QVERIFY(PartyMode::allowed(s, PartyMode::Action::AddToQueue));
        QVERIFY(!PartyMode::allowed(s, PartyMode::Action::Skip));
        QVERIFY(!PartyMode::allowed(s, PartyMode::Action::EditTags));
        QVERIFY(!PartyMode::allowed(s, PartyMode::Action::Quit));
        QVERIFY(PartyMode::checkPin(s, "1234"));
        QVERIFY(!PartyMode::checkPin(s, "1235"));
        QVERIFY(!PartyMode::disable(lib, "0000"));
        QVERIFY(PartyMode::Settings::load(lib).enabled);
        QVERIFY(PartyMode::disable(lib, "1234"));
        QVERIFY(!PartyMode::Settings::load(lib).enabled);
        // Presets.
        QVERIFY(PartyMode::presetActions("dj").contains(PartyMode::Action::Skip));
        QVERIFY(PartyMode::presetActions("display").isEmpty());
        QVERIFY(!PartyMode::enable(lib, "rave"));
        // Custom actions survive a save.
        PartyMode::Settings custom;
        custom.enabled = true;
        custom.allowed = {PartyMode::Action::Volume};
        custom.save(lib);
        const PartyMode::Settings back = PartyMode::Settings::load(lib);
        QVERIFY(PartyMode::allowed(back, PartyMode::Action::Volume));
        QVERIFY(!PartyMode::allowed(back, PartyMode::Action::Play));
        QVERIFY(PartyMode::disable(lib)); // no PIN set
        for (PartyMode::Action a : PartyMode::allActions()) {
            PartyMode::Action parsed;
            QVERIFY(PartyMode::parseAction(PartyMode::actionName(a), &parsed));
            QCOMPARE(parsed, a);
        }
    }

    // ---- MediaMonkey -------------------------------------------------------

    void mmHelpers() {
        using MmImport::PathMap;
        QList<PathMap> maps = {{"D:\\Music", "/mnt/nas/music"}, {"\\\\nas\\share", "/mnt/share"}, {"E:", "/mnt/e"}};
        bool mapped = false;
        QCOMPARE(MmImport::mapPath("D:\\Music\\A\\b.mp3", maps, &mapped), QString("/mnt/nas/music/A/b.mp3"));
        QVERIFY(mapped);
        QCOMPARE(MmImport::mapPath("d:\\music\\A\\b.mp3", maps), QString("/mnt/nas/music/A/b.mp3"));
        QCOMPARE(MmImport::mapPath("D:\\Musical\\b.mp3", maps, &mapped), QString("D:/Musical/b.mp3"));
        QVERIFY(!mapped);
        QCOMPARE(MmImport::mapPath("\\\\NAS\\share\\x.flac", maps), QString("/mnt/share/x.flac"));
        QCOMPARE(MmImport::mapPath("E:\\x.flac", maps), QString("/mnt/e/x.flac"));
        // The longest prefix wins.
        maps << PathMap{"D:\\Music\\Classical", "/mnt/classical"};
        QCOMPARE(MmImport::mapPath("D:\\Music\\Classical\\x.flac", maps), QString("/mnt/classical/x.flac"));
        PathMap parsed;
        QVERIFY(PathMap::parse("D:\\Music=/mnt/music", &parsed));
        QCOMPARE(parsed.from, QString("D:\\Music"));
        QVERIFY(!PathMap::parse("D:\\Music", &parsed));
        QVERIFY(!PathMap::parse("D:\\Music=relative", &parsed));

        QCOMPARE(MmImport::oleToUnix(25569.0 + 20000), qint64(20000) * 86400);
        QCOMPARE(MmImport::oleToUnix(0), 0);
        QCOMPARE(MmImport::oleToUnix(-1), 0);

        QString why;
        QCOMPARE(MmImport::translateCriteria("Rating >= 80 AND Genre LIKE '%Synth%' ORDER BY Rating DESC LIMIT 10", &why),
                 QString("rating:>=4 genre:Synth sort:-rating limit:10"));
        QCOMPARE(MmImport::translateCriteria("WHERE Songs.Artist = 'Neon Coast' OR Songs.Artist = 'The Lovers' "
                                             "AND PlayCounter > 3",
                                             &why),
                 QString("artist:=\"Neon Coast\" OR (artist:=\"The Lovers\" plays:>3)"));
        QCOMPARE(MmImport::translateCriteria("LastTimePlayed < Date() - 365 AND NOT (Genre = 'Christmas')", &why),
                 QString("played:>365d -(genre:=Christmas)"));
        QCOMPARE(MmImport::translateCriteria("Rating = -1 ORDER BY Random()", &why), QString("rating:none sort:random"));
        QCOMPARE(MmImport::translateCriteria("SongLength > 600000 AND Year BETWEEN 19900000 AND 19991231", &why),
                 QString("length:>600s year:1990..1999"));
        QCOMPARE(MmImport::translateCriteria("Title LIKE 'Live%'", &why), QString("title:/^Live/"));
        QCOMPARE(MmImport::translateCriteria("[Basic]\nMatchAny=1\nCondition0=Genre = 'Rock'\nCondition1=Genre = 'Metal'\n"
                                             "MaxTracks=50\n",
                                             &why),
                 QString("(genre:=Rock) OR (genre:=Metal) limit:50"));
        for (const char *bad : {"SongPath LIKE 'D:%'", "Rating >= ", "Mood = 'x'", "Artist LIKE '%a%b%'", "(Rating > 1"}) {
            why.clear();
            QVERIFY2(MmImport::translateCriteria(bad, &why).isEmpty(), bad);
            QVERIFY2(!why.isEmpty(), bad);
        }
    }

    void mediaMonkeyImport() {
        Lib l;
        Library &lib = *l.library;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 lastPlayed = now - 3600; // later than any fixture play
        const qint64 added = QDateTime(QDate(2010, 1, 1), QTime(12, 0), QTimeZone::UTC).toSecsSinceEpoch();
        const QString mmPath = l.dir.filePath("mm/MM.DB");
        QDir().mkpath(l.dir.filePath("mm"));
        QVERIFY(makeMm4(mmPath, lastPlayed, added));
        const QByteArray before = sha(mmPath);

        // Without the collations, MediaMonkey's database can't even be sorted.
        {
            sqlite3 *raw = nullptr;
            QCOMPARE(sqlite3_open_v2(QFile::encodeName(mmPath).constData(), &raw, SQLITE_OPEN_READONLY, nullptr),
                     SQLITE_OK);
            sqlite3_stmt *st = nullptr;
            QVERIFY(sqlite3_prepare_v2(raw, "SELECT ID FROM Songs ORDER BY Artist", -1, &st, nullptr) != SQLITE_OK);
            sqlite3_finalize(st);
            sqlite3_close(raw);
        }

        MmImport::Options options;
        options.maps = {{"D:\\Music", l.root}, {"\\\\nas\\music", l.root}};
        const qint64 midnight = l.track("Midnight Highway"), go = l.track("Go West"), heat = l.track("Heatwave");
        const QString tracksBefore =
            value(lib, "SELECT group_concat(id || ':' || coalesce(rating, '-') || ':' || play_count || ':' || "
                       "skip_count || ':' || coalesce(last_played, '-') || ':' || added, ',') FROM tracks")
                .toString();

        // Dry run: a full report, nothing written.
        MmImport::Report r = MmImport::run(lib, mmPath, options);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.applied);
        QCOMPARE(r.schema, QString("MM4"));
        QCOMPARE(r.mmTracks, 5);
        QCOMPARE(r.matchedByPath, 2);
        QCOMPARE(r.matchedBySuffix, 1);
        QCOMPARE(r.matchedByTags, 1);
        QCOMPARE(r.unmatched, 1);
        QCOMPARE(r.unmatchedSample, QStringList{"D:\\Music\\Nobody\\Ghost.mp3"});
        QCOMPARE(r.tracksChanged, 2);
        QCOMPARE(r.fieldChanges, 7);
        QCOMPARE(r.changesByField.value("play_count"), 2);
        QCOMPARE(r.changesByField.value("rating"), 1); // Go West had none; Midnight Highway keeps its own
        QCOMPARE(r.historyRows, 3);
        QCOMPARE(r.playlists.size(), 4);
        QCOMPARE(r.suggestedMaps.size(), 1);
        QVERIFY2(r.suggestedMaps.first().startsWith("E:\\Old=" + l.root), qPrintable(r.suggestedMaps.first()));
        QVERIFY(r.missingColumns.isEmpty());
        QHash<QString, MmImport::PlaylistReport> lists;
        for (const MmImport::PlaylistReport &p : r.playlists)
            lists.insert(p.name, p);
        QCOMPARE(lists["Road"].kind, QString("folder"));
        QCOMPARE(lists["Summer"].kind, QString("static"));
        QCOMPARE(lists["Summer"].parent, QString("Road"));
        QCOMPARE(lists["Summer"].entries, 4);
        QCOMPARE(lists["Summer"].matched, 3);
        QCOMPARE(lists["Best synth"].kind, QString("auto"));
        QCOMPARE(lists["Best synth"].query, QString("rating:>=4 genre:Synth sort:-rating limit:10"));
        QCOMPARE(lists["Weird"].kind, QString("snapshot"));
        QVERIFY(lists["Weird"].reason.contains("SongPath"));
        QCOMPARE(value(lib, "SELECT count(*) FROM playlists").toInt(), 0);
        QCOMPARE(value(lib, "SELECT count(*) FROM play_history").toInt(), 0);
        QCOMPARE(value(lib, "SELECT count(*) FROM import_journal").toInt(), 0);
        const QString tracksAfterDryRun =
            value(lib, "SELECT group_concat(id || ':' || coalesce(rating, '-') || ':' || play_count || ':' || "
                       "skip_count || ':' || coalesce(last_played, '-') || ':' || added, ',') FROM tracks")
                .toString();
        QCOMPARE(tracksAfterDryRun, tracksBefore);

        // Apply.
        options.apply = true;
        r = MmImport::run(lib, mmPath, options);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.applied);
        QVERIFY(MmImport::isImportBatch(r.batch));
        QCOMPARE(l.field(midnight, "play_count").toInt(), 40);
        QCOMPARE(l.field(midnight, "skip_count").toInt(), 2);
        QCOMPARE(l.field(midnight, "rating").toInt(), 80); // not overwritten
        QVERIFY(qAbs(l.field(midnight, "last_played").toLongLong() - lastPlayed) <= 1);
        QVERIFY(qAbs(l.field(midnight, "added").toLongLong() - added) <= 1);
        QCOMPARE(l.field(midnight, "custom1").toString(), QString("Vinyl rip"));
        QCOMPARE(l.field(go, "rating").toInt(), 80);
        QCOMPARE(l.field(go, "play_count").toInt(), 7);
        QCOMPARE(l.field(heat, "play_count").toInt(), 3); // Thrum's 3 beats MediaMonkey's 1
        QCOMPARE(value(lib, "SELECT count(*) FROM play_history").toInt(), 3);
        const qint64 folder = Playlists::resolve(lib, "MediaMonkey");
        QVERIFY(folder);
        const qint64 summer = Playlists::resolve(lib, "MediaMonkey/Road/Summer");
        QVERIFY(summer);
        const QList<Playlists::Item> items = Playlists::items(lib, summer);
        QCOMPARE(items.size(), 4);
        QCOMPARE(items[0].trackId, go);
        QCOMPARE(items[1].trackId, midnight);
        QCOMPARE(items[2].availability, QString("unmatched"));
        QCOMPARE(items[2].title, QString("Ghost"));
        QCOMPARE(items[2].path, l.root + "/Nobody/Ghost.mp3");
        QCOMPARE(items[3].trackId, go);
        const qint64 synth = Playlists::resolve(lib, "MediaMonkey/Best synth");
        QCOMPARE(titles(Playlists::items(lib, synth)), (QStringList{"Coastline", "Midnight Highway"}));
        Playlists::Playlist weird;
        QVERIFY(Playlists::get(lib, Playlists::resolve(lib, "Weird"), &weird));
        QCOMPARE(weird.kind, QString("static"));
        QVERIFY(weird.note.contains("SongPath LIKE"));
        QCOMPARE(Playlists::trackIds(lib, weird.id), QList<qint64>{heat});
        // The original was never touched.
        QCOMPARE(sha(mmPath), before);
        QCOMPARE(QDir(l.dir.filePath("mm")).entryList(QDir::Files), QStringList{"MM.DB"});

        // Importing again changes nothing and adds no playlists or history.
        const int playlistsAfter = value(lib, "SELECT count(*) FROM playlists").toInt();
        MmImport::Report again = MmImport::run(lib, mmPath, options);
        QVERIFY(again.ok);
        QCOMPARE(again.fieldChanges, 0);
        QCOMPARE(again.historyRows, 0);
        QCOMPARE(again.historyDuplicates, 3);
        QCOMPARE(value(lib, "SELECT count(*) FROM playlists").toInt(), playlistsAfter);
        QVERIFY(again.batch.isEmpty()); // nothing was written, so there is nothing to undo
        QCOMPARE(MmImport::lastBatch(lib), r.batch);

        // A play after the import survives the undo; everything else goes back.
        Stats::recordPlay(lib, midnight, 245000, 0, false, now);
        QCOMPARE(l.field(midnight, "play_count").toInt(), 41);
        const MmImport::UndoResult u = MmImport::undo(lib, r.batch);
        QVERIFY2(u.ok, qPrintable(u.error));
        QCOMPARE(u.fields, 6); // last_played moved on with the new play, so it stays
        QCOMPARE(u.historyRows, 3);
        QVERIFY(u.playlists >= 1);
        QCOMPARE(l.field(midnight, "play_count").toInt(), 13);
        QCOMPARE(l.field(midnight, "skip_count").toInt(), 0);
        QVERIFY(l.field(midnight, "custom1").isNull());
        QCOMPARE(l.field(midnight, "last_played").toLongLong(), now);
        QVERIFY(l.field(go, "rating").isNull());
        QCOMPARE(l.field(go, "play_count").toInt(), 0);
        QCOMPARE(value(lib, "SELECT count(*) FROM playlists").toInt(), 0);
        QCOMPARE(value(lib, "SELECT count(*) FROM play_history").toInt(), 1);
        QVERIFY(!MmImport::undo(lib, r.batch).ok); // already undone
        QVERIFY(MmImport::lastBatch(lib).isEmpty());

        // Overwrite replaces Thrum's rating with MediaMonkey's.
        options.overwrite = true;
        r = MmImport::run(lib, mmPath, options);
        QVERIFY(r.ok);
        QCOMPARE(l.field(midnight, "rating").toInt(), 100);
    }

    void mediaMonkey5AndBadInput() {
        Lib l;
        Library &lib = *l.library;
        const QString path = l.dir.filePath("MM5.DB");
        {
            MmBuilder mm(path);
            QVERIFY(mm.exec("CREATE TABLE Songs (ID INTEGER PRIMARY KEY, SongPath TEXT COLLATE IUNICODE, "
                            "SongTitle TEXT COLLATE IUNICODE, Artist TEXT COLLATE IUNICODE, Album TEXT, "
                            "SongLength INTEGER, Rating INTEGER, PlayCounter INTEGER, LastTimePlayed REAL, "
                            "DateAdded REAL, Custom1 TEXT, Custom2 TEXT, Custom3 TEXT, Custom4 TEXT, Custom5 TEXT, "
                            "Custom6 TEXT)"));
            QVERIFY(mm.exec("CREATE TABLE Collections (ID INTEGER PRIMARY KEY, Name TEXT COLLATE IUNICODE)"));
            QVERIFY(mm.exec("INSERT INTO Songs (ID, SongPath, SongTitle, Artist, SongLength, Rating, PlayCounter) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?)",
                            {1, "M:\\Music\\Summer Mix\\02 Snow Day.flac", "Snow Day", "Frost Club", 150000, 90, 4}));
        }
        MmImport::Options options;
        options.maps = {{"M:\\Music", l.root}};
        options.apply = true;
        const MmImport::Report r = MmImport::run(lib, path, options);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.schema, QString("MM5"));
        QVERIFY(r.missingColumns.contains("Songs.SkipCount"));
        QVERIFY(r.missingColumns.contains("Songs.IDMedia"));
        QCOMPARE(r.matchedByPath, 1);
        QCOMPARE(l.field(l.track("Snow Day"), "rating").toInt(), 40); // Thrum's own rating stays
        QCOMPARE(l.field(l.track("Snow Day"), "play_count").toInt(), 4);
        QVERIFY(r.notes.join(' ').contains("Played"));

        // Not a MediaMonkey database, and not a file.
        QVERIFY(writeFile(l.dir.filePath("junk.db"), "this is not sqlite"));
        QVERIFY(!MmImport::run(lib, l.dir.filePath("junk.db"), options).ok);
        QVERIFY(!MmImport::run(lib, l.dir.filePath("nothing.db"), options).ok);
        {
            MmBuilder other(l.dir.filePath("other.db"));
            other.exec("CREATE TABLE Tracks (ID INTEGER)");
        }
        const MmImport::Report notMm = MmImport::run(lib, l.dir.filePath("other.db"), options);
        QVERIFY(!notMm.ok);
        QVERIFY(notMm.error.contains("Songs"));
    }

    // ---- Commands ----------------------------------------------------------

    void commands() {
        Lib l;
        const QString db = l.db();
        QJsonObject j;
        QString errors;
        QVERIFY(Cli::isCommand("playlist") && Cli::isCommand("autodj") && Cli::isCommand("import"));

        QCOMPARE(cli({"playlist", "create", "Road", "--folder", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("playlist").toObject().value("kind").toString(), QString("folder"));
        QCOMPARE(cli({"playlist", "create", "Mix", "--parent", "Road", "--json", "--db", db}, &j), 0);
        const qint64 mix = j.value("playlist").toObject().value("id").toInteger();
        QCOMPARE(cli({"playlist", "add", "Road/Mix", "genre:rock", "sort:title", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("added").toInt(), 3);
        QCOMPARE(cli({"playlist", "add", "Mix", "--ids", QString::number(l.track("Coastline")), "--at", "1", "--json",
                      "--db", db},
                     &j),
                 0);
        QCOMPARE(cli({"playlist", "show", "Mix", "--json", "--db", db}, &j), 0);
        QJsonArray items = j.value("items").toArray();
        QCOMPARE(items.size(), 4);
        QCOMPARE(items[0].toObject().value("title").toString(), QString("Coastline"));
        QCOMPARE(cli({"playlist", "move", "Mix", "1", "4", "--json", "--db", db}, &j), 0);
        QCOMPARE(cli({"playlist", "remove", "Mix", "1", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("removed").toInt(), 1);
        QCOMPARE(cli({"playlist", "show", "Mix", "--json", "--db", db}, &j), 0);
        items = j.value("items").toArray();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[2].toObject().value("title").toString(), QString("Coastline"));
        QCOMPARE(cli({"playlist", "remove", "Mix", "9", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"playlist", "show", "Nope", "--db", db}, nullptr, &errors), 1);
        QVERIFY(errors.contains("No playlist"));

        const QString file = l.dir.filePath("out/Mix.m3u8");
        QDir().mkpath(l.dir.filePath("out"));
        QCOMPARE(cli({"playlist", "export", "Mix", file, "--relative", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("entries").toInt(), 3);
        QCOMPARE(cli({"playlist", "import", file, "--name", "Mix copy", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("imported").toArray().first().toObject().value("matched_by_path").toInt(), 3);
        QCOMPARE(cli({"playlist", "list", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("playlists").toArray().size(), 3);
        QCOMPARE(cli({"playlist", "move", "Mix", "--parent", "/", "--json", "--db", db}, &j), 0);
        QCOMPARE(cli({"playlist", "delete", QString::number(mix), "--json", "--db", db}, &j), 0);

        QCOMPARE(cli({"autoplaylist", "create", "Loved", "rating:4..", "sort:-rating", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("tracks").toInt(), 3);
        QCOMPARE(cli({"autoplaylist", "create", "Bad", "artst:x", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"playlist", "reroll", "Loved", "--seed", "5", "--json", "--db", db}, &j), 0);
        QCOMPARE(cli({"collection", "list", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("collections").toArray().size(), 4);
        QCOMPARE(cli({"collection", "create", "Rock", "genre:rock", "--json", "--db", db}, &j), 0);

        QCOMPARE(cli({"autodj", "--count", "5", "--seed", "77", "--json", "--db", db}, &j), 0);
        const QJsonArray picks = j.value("tracks").toArray();
        QCOMPARE(picks.size(), 5);
        QCOMPARE(j.value("seed").toInt(), 77);
        QCOMPARE(cli({"autodj", "--count", "5", "--seed", "77", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("tracks").toArray(), picks);
        QCOMPARE(cli({"autodj", "--source", "collection:Rock", "--count", "2", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("candidates").toInt(), 3);

        QCOMPARE(cli({"stats", "--top", "artists", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("rows").toArray().first().toObject().value("name").toString(), QString("The Lovers"));
        QCOMPARE(cli({"stats", "--top", "albums", "--since", "30d", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("rows").toArray().size(), 0);
        QCOMPARE(cli({"stats", "--top", "moods", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"stats", "--top", "tracks", "--since", "soon", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"stats", "--never", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("total").toInt(), 5);
        QCOMPARE(cli({"stats", "--top", "tracks", "--limit", "2", "--db", db}, nullptr, &errors), 0);
        QVERIFY(errors.contains("Love Song"));

        // MediaMonkey: dry run, apply, undo through `thrum undo`.
        const QString mm = l.dir.filePath("MM.DB");
        QVERIFY(makeMm4(mm, QDateTime::currentSecsSinceEpoch() - 60, 1262347200));
        const QString map = "D:\\Music=" + l.root;
        QCOMPARE(cli({"import", "mediamonkey", mm, "--map", map, "--dry-run", "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("dry_run").toBool(), true);
        QCOMPARE(j.value("matched_by_path").toInt(), 1);
        QCOMPARE(cli({"import", "mediamonkey", mm, "--map", "D:\\Music", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"import", "mediamonkey", mm, "--dry-run", "--apply", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"import", "mediamonkey", mm, "--map", map, "--apply", "--json", "--db", db}, &j), 0);
        const QString first = j.value("batch").toString();
        QVERIFY(first.startsWith("mmimport-"));
        QCOMPARE(cli({"undo", first, "--json", "--db", db}, &j), 0);
        QCOMPARE(j.value("batch").toString(), first);
        QCOMPARE(cli({"import", "mediamonkey", mm, "--map", map, "--apply", "--db", db}, nullptr, &errors), 0);
        QVERIFY(errors.contains("undo it with: thrum import undo mmimport-"));
        QCOMPARE(cli({"import", "undo", "--json", "--db", db}, &j), 0);
        QVERIFY(j.value("fields").toInt() > 0);
        QCOMPARE(cli({"import", "undo", "--db", db}, nullptr, &errors), 1);
        QCOMPARE(cli({"import", "itunes", mm, "--db", db}, nullptr, &errors), 1);
    }
};

int runPlaylistTests(int argc, char **argv) {
    PlaylistTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "playlist_tests.moc"
