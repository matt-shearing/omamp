// Phase 1C: folder watching, quick rescans, moved files, tag extras, the
// artwork cache and the schema 3 migration. Every library, music folder and
// picture cache lives in a temporary directory; nothing is played.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/popularimeterframe.h>
#include <taglib/unsynchronizedlyricsframe.h>

#include <sys/stat.h>
#include <utime.h>

#include "artprovider.h"
#include "artwork.h"
#include "library.h"
#include "scanner.h"
#include "tagreader.h"
#include "watcher.h"

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
bool tone(const QString &file, double seconds, const QStringList &extra) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds),
                        "-af", "volume=0.05", "-map_metadata", "-1"};
    return run("ffmpeg", args + extra + QStringList{file});
}

bool picture(const QString &file, const QString &colour, const QString &size) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    return run("ffmpeg", {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                          QString("color=c=%1:s=%2").arg(colour, size), "-frames:v", "1", file});
}

// An MP3 with an embedded front cover.
bool toneWithCover(const QString &file, const QString &cover, const QStringList &extra) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        "sine=frequency=440:sample_rate=22050:duration=1", "-i", cover,
                        "-map", "0:a", "-map", "1:v", "-c:a", "libmp3lame", "-b:a", "64k", "-c:v", "copy",
                        "-id3v2_version", "3", "-disposition:v", "attached_pic",
                        "-metadata:s:v", "comment=Cover (front)", "-map_metadata", "-1"};
    return run("ffmpeg", args + extra + QStringList{file});
}

void touch(const QString &path, int secondsLater) {
    struct stat st {};
    stat(QFile::encodeName(path).constData(), &st);
    struct utimbuf times {st.st_atime, st.st_mtime + secondsLater};
    utime(QFile::encodeName(path).constData(), &times);
}

qint64 trackId(Library &library, const QString &relEnding) {
    QSqlQuery q(library.db());
    q.prepare("SELECT id FROM tracks WHERE rel_path LIKE ?");
    q.addBindValue("%" + relEnding);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

} // namespace

class WatcherTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_fixtures;
    bool m_have = false;
    QString fixture(const QString &name) const { return m_fixtures.filePath(name); }

    // A copy of one fixture file into a folder.
    static bool put(const QString &from, const QString &to) {
        QDir().mkpath(QFileInfo(to).absolutePath());
        return QFile::copy(from, to);
    }

private slots:
    void initTestCase() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is needed to make the test files");
        QVERIFY(tone(fixture("plain.flac"), 1,
                     {"-metadata", "title=Plain Song", "-metadata", "artist=Neon Coast", "-metadata", "album=Night Drive"}));
        QVERIFY(tone(fixture("plain2.flac"), 2,
                     {"-metadata", "title=Second Song", "-metadata", "artist=Neon Coast", "-metadata", "album=Night Drive"}));
        QVERIFY(tone(fixture("untagged.mp3"), 1, {"-c:a", "libmp3lame", "-b:a", "64k"}));
        QVERIFY(tone(fixture("extras.flac"), 1,
                     {"-metadata", "title=Extras", "-metadata", "artist=Lena Park", "-metadata", "album=Tags",
                      "-metadata", "FMPS_RATING=0.8", "-metadata", "FMPS_PLAYCOUNT=7",
                      "-metadata", "REPLAYGAIN_TRACK_GAIN=-6.50 dB", "-metadata", "REPLAYGAIN_TRACK_PEAK=0.988",
                      "-metadata", "REPLAYGAIN_ALBUM_GAIN=-7.25 dB",
                      "-metadata", "MUSICBRAINZ_TRACKID=11111111-1111-1111-1111-111111111111",
                      "-metadata", "MUSICBRAINZ_ALBUMID=22222222-2222-2222-2222-222222222222",
                      "-metadata", "MUSICBRAINZ_RELEASEGROUPID=33333333-3333-3333-3333-333333333333",
                      "-metadata", "MUSICBRAINZ_ARTISTID=44444444-4444-4444-4444-444444444444",
                      "-metadata", "MUSICBRAINZ_ALBUMARTISTID=55555555-5555-5555-5555-555555555555",
                      "-metadata", "LYRICS=Drive all night", "-metadata", "BPM=120",
                      "-metadata", "COMPILATION=1"}));
        // An MP3 rated in POPM (MediaMonkey/WMP 4 stars) with a play count and
        // unsynchronised lyrics, written with TagLib as players do.
        QVERIFY(tone(fixture("popm.mp3"), 1,
                     {"-c:a", "libmp3lame", "-b:a", "64k", "-metadata", "title=Popular", "-metadata", "artist=Rater"}));
        {
            TagLib::MPEG::File file(QFile::encodeName(fixture("popm.mp3")).constData());
            QVERIFY(file.isValid());
            TagLib::ID3v2::Tag *tag = file.ID3v2Tag(true);
            auto *popm = new TagLib::ID3v2::PopularimeterFrame;
            popm->setEmail("no@email");
            popm->setRating(196);
            popm->setCounter(12);
            tag->addFrame(popm);
            auto *lyrics = new TagLib::ID3v2::UnsynchronizedLyricsFrame;
            lyrics->setText("Words to sing");
            tag->addFrame(lyrics);
            QVERIFY(file.save());
        }
        QVERIFY(picture(fixture("red.jpg"), "red", "800x800"));
        QVERIFY(picture(fixture("blue.png"), "blue", "500x300"));
        QVERIFY(toneWithCover(fixture("embedded.mp3"), fixture("blue.png"),
                              {"-metadata", "title=Embedded", "-metadata", "artist=Painter", "-metadata", "album=Blue"}));
        m_have = true;
    }

    // ---- Migration ---------------------------------------------------------

    void phase0DatabaseMigratesToSchema3() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("plain.flac"), music + "/a/plain.flac"));
        const QString db = dir.filePath("library.db");
        {
            // A Phase 0 library: schema 2, scanned by the Phase 0 reader.
            Library v2(db);
            QVERIFY(v2.open(nullptr, 2));
            QCOMPARE(v2.schemaVersion(), 2);
            QSqlQuery q(v2.db());
            QVERIFY(q.exec("INSERT INTO volumes (key, kind, root_path, added) VALUES ('mount:x', 'local', '/m', 0)"));
            QVERIFY(q.exec("INSERT INTO albums (title, artist_key, title_key) VALUES ('A', 'a', 'a')"));
            QVERIFY(q.exec("INSERT INTO tracks (volume_id, rel_path, file_name, size, mtime_ns, title, added, rating, "
                           "play_count, album_id) VALUES (1, 'a.flac', 'a', 1, 1, 'Old Song', 0, 60, 4, 1)"));
        }
        Library library(db);
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QCOMPARE(Library::latestVersion(), 3);
        QVERIFY(library.lastBackup().contains(".v2-"));
        QVERIFY(QFileInfo::exists(library.lastBackup()));
        // The old row keeps its stats and is marked as read by an older reader.
        QCOMPARE(value(library, "SELECT rating FROM tracks WHERE title = 'Old Song'").toInt(), 60);
        QCOMPARE(value(library, "SELECT play_count FROM tracks WHERE title = 'Old Song'").toInt(), 4);
        QCOMPARE(value(library, "SELECT tag_version FROM tracks WHERE title = 'Old Song'").toInt(), 0);
        QCOMPARE(value(library, "SELECT has_embedded_art FROM tracks").toInt(), 0);
        QVERIFY(value(library, "SELECT art_checked FROM albums").isNull());
        for (const char *index : {"tracks_size", "tracks_quick_hash"})
            QCOMPARE(value(library, QString("SELECT count(*) FROM sqlite_master WHERE name = '%1'").arg(index)).toInt(), 1);
        // Search still works across the migration.
        QCOMPARE(value(library, "SELECT count(*) FROM tracks_fts WHERE tracks_fts MATCH 'old'").toInt(), 1);
    }

    void olderReaderRowsAreReadAgainOnce() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("extras.flac"), music + "/extras.flac"));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        QCOMPARE(scanner.scan({music}).added, 1);
        // Pretend the row came from Phase 0's reader.
        QSqlQuery(library.db()).exec("UPDATE tracks SET tag_version = 0, mb_release_group_id = NULL");
        ScanResult again = scanner.scan({music});
        QCOMPARE(again.updated, 1);
        QCOMPARE(value(library, "SELECT mb_release_group_id FROM tracks").toString(),
                 QString("33333333-3333-3333-3333-333333333333"));
        // And then it is stat-only again.
        again = scanner.scan({music});
        QCOMPARE(again.unchanged, 1);
        QCOMPARE(again.updated, 0);
    }

    // ---- Tag extras --------------------------------------------------------

    void ratingConventions() {
        QCOMPARE(TagReader::ratingFromPopm(0), -1);
        QCOMPARE(TagReader::ratingFromPopm(1), 20);
        QCOMPARE(TagReader::ratingFromPopm(64), 40);
        QCOMPARE(TagReader::ratingFromPopm(128), 60);
        QCOMPARE(TagReader::ratingFromPopm(196), 80);
        QCOMPARE(TagReader::ratingFromPopm(255), 100);
        QCOMPARE(TagReader::ratingFromPopm(118), 50); // MediaMonkey's 2.5 stars
        QCOMPARE(TagReader::ratingFromText("FMPS_RATING", "0.8"), 80);
        QCOMPARE(TagReader::ratingFromText("FMPS_RATING", "1.5"), -1);
        QCOMPARE(TagReader::ratingFromText("RATING", "4"), 80);
        QCOMPARE(TagReader::ratingFromText("RATING", "70"), 70);
        QCOMPARE(TagReader::ratingFromText("RATING", "0"), -1);
        QCOMPARE(TagReader::ratingFromText("RATING", "abc"), -1);
    }

    void tagExtrasAreStored() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("extras.flac"), music + "/x/extras.flac"));
        QVERIFY(put(fixture("popm.mp3"), music + "/x/popm.mp3"));
        QVERIFY(put(fixture("embedded.mp3"), music + "/y/embedded.mp3"));
        QVERIFY(put(fixture("plain.flac"), music + "/y/plain.flac"));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        QCOMPARE(scanner.scan({music}).added, 4);

        QSqlQuery q(library.db());
        QVERIFY(q.exec("SELECT rating, play_count, tag_rating, tag_play_count, rg_track_gain, rg_track_peak, "
                       "rg_album_gain, mb_track_id, mb_release_id, mb_release_group_id, mb_artist_id, "
                       "mb_album_artist_id, has_lyrics, bpm, compilation, has_embedded_art, tag_version, quick_hash "
                       "FROM tracks WHERE title = 'Extras'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 80);
        QCOMPARE(q.value(1).toInt(), 7);
        QCOMPARE(q.value(2).toInt(), 80);
        QCOMPARE(q.value(3).toInt(), 7);
        QCOMPARE(q.value(4).toDouble(), -6.5);
        QCOMPARE(q.value(5).toDouble(), 0.988);
        QCOMPARE(q.value(6).toDouble(), -7.25);
        QCOMPARE(q.value(7).toString(), QString("11111111-1111-1111-1111-111111111111"));
        QCOMPARE(q.value(8).toString(), QString("22222222-2222-2222-2222-222222222222"));
        QCOMPARE(q.value(9).toString(), QString("33333333-3333-3333-3333-333333333333"));
        QCOMPARE(q.value(10).toString(), QString("44444444-4444-4444-4444-444444444444"));
        QCOMPARE(q.value(11).toString(), QString("55555555-5555-5555-5555-555555555555"));
        QCOMPARE(q.value(12).toInt(), 1);
        QCOMPARE(q.value(13).toInt(), 120);
        QCOMPARE(q.value(14).toInt(), 1);
        QCOMPARE(q.value(15).toInt(), 0);
        QCOMPARE(q.value(16).toInt(), TagReader::version);
        QCOMPARE(q.value(17).toString().size(), 16);
        QCOMPARE(value(library, "SELECT mb_release_group_id FROM albums WHERE title = 'Tags'").toString(),
                 QString("33333333-3333-3333-3333-333333333333"));

        QVERIFY(q.exec("SELECT rating, play_count, has_lyrics FROM tracks WHERE title = 'Popular'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 80);
        QCOMPARE(q.value(1).toInt(), 12);
        QCOMPARE(q.value(2).toInt(), 1);

        QCOMPARE(value(library, "SELECT has_embedded_art FROM tracks WHERE title = 'Embedded'").toInt(), 1);
        QCOMPARE(value(library, "SELECT has_lyrics FROM tracks WHERE title = 'Plain Song'").toInt(), 0);
        QVERIFY(value(library, "SELECT rating FROM tracks WHERE title = 'Plain Song'").isNull());
        QCOMPARE(value(library, "SELECT play_count FROM tracks WHERE title = 'Plain Song'").toInt(), 0);
    }

    void libraryStatsWinOverTags() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        const QString file = music + "/extras.flac";
        QVERIFY(put(fixture("extras.flac"), file));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        QCOMPARE(scanner.scan({music}).added, 1);

        // The user rates it 5 stars and has played it 30 times in Omamp.
        QSqlQuery(library.db()).exec("UPDATE tracks SET rating = 100, play_count = 30");
        touch(file, 10);
        QCOMPARE(scanner.scan({music}).updated, 1);
        QCOMPARE(value(library, "SELECT rating FROM tracks").toInt(), 100);
        QCOMPARE(value(library, "SELECT play_count FROM tracks").toInt(), 30);

        // The user clears the rating: a rescan must not bring the file's back.
        QSqlQuery(library.db()).exec("UPDATE tracks SET rating = NULL");
        touch(file, 20);
        QCOMPARE(scanner.scan({music}).updated, 1);
        QVERIFY(value(library, "SELECT rating FROM tracks").isNull());
        QCOMPARE(value(library, "SELECT tag_rating FROM tracks").toInt(), 80);
    }

    // ---- Moved files -------------------------------------------------------

    void movedFilesKeepTheirTracks() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("plain.flac"), music + "/old/plain.flac"));
        QVERIFY(put(fixture("plain2.flac"), music + "/old/plain2.flac"));
        QVERIFY(put(fixture("untagged.mp3"), music + "/old/untagged.mp3"));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        scanner.setInFlightLimit(1); // feed the pool one file at a time
        QCOMPARE(scanner.scan({music}).added, 3);
        const qint64 plain = trackId(library, "old/plain.flac");
        const qint64 untagged = trackId(library, "old/untagged.mp3");
        QVERIFY(plain && untagged);

        QSqlQuery q(library.db());
        QVERIFY(q.exec(QString("UPDATE tracks SET rating = 80, play_count = 9 WHERE id = %1").arg(plain)));
        QVERIFY(q.exec(QString("INSERT INTO play_history (track_id, played_at) VALUES (%1, 100)").arg(plain)));
        QVERIFY(q.exec("INSERT INTO playlists (name, created) VALUES ('Road', 0)"));
        QVERIFY(q.exec(QString("INSERT INTO playlist_items (playlist_id, track_id, position) VALUES (1, %1, 0)").arg(plain)));

        // Move one tagged file and the untagged one (found by its content
        // hash and file name) to another folder, and rename the tagged one.
        QDir().mkpath(music + "/new/sub");
        QVERIFY(QFile::rename(music + "/old/plain.flac", music + "/new/sub/renamed.flac"));
        QVERIFY(QFile::rename(music + "/old/untagged.mp3", music + "/new/untagged.mp3"));
        const ScanResult r = scanner.scan({music});
        QCOMPARE(r.moved, 2);
        QCOMPARE(r.added, 0);
        QCOMPARE(r.missing, 2); // marked, then found again
        QCOMPARE(trackId(library, "new/sub/renamed.flac"), plain);
        QCOMPARE(trackId(library, "new/untagged.mp3"), untagged);
        QCOMPARE(value(library, QString("SELECT availability FROM tracks WHERE id = %1").arg(plain)).toString(),
                 QString("online"));
        QCOMPARE(value(library, QString("SELECT rating FROM tracks WHERE id = %1").arg(plain)).toInt(), 80);
        QCOMPARE(value(library, QString("SELECT play_count FROM tracks WHERE id = %1").arg(plain)).toInt(), 9);
        QCOMPARE(value(library, QString("SELECT count(*) FROM play_history WHERE track_id = %1").arg(plain)).toInt(), 1);
        QCOMPARE(value(library, QString("SELECT count(*) FROM playlist_items WHERE track_id = %1").arg(plain)).toInt(), 1);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks").toInt(), 3);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);
        QCOMPARE(library.absolutePath(plain), QFileInfo(music + "/new/sub/renamed.flac").canonicalFilePath());
    }

    void ambiguousCopiesAreNotGuessed() {
        // Two identical untagged files go missing; a third copy with yet
        // another name appears. Which one it was can't be told, so it is new.
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("untagged.mp3"), music + "/a/one.mp3"));
        QVERIFY(put(fixture("untagged.mp3"), music + "/b/two.mp3"));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        QCOMPARE(scanner.scan({music}).added, 2);
        QVERIFY(QFile::remove(music + "/a/one.mp3"));
        QVERIFY(QFile::rename(music + "/b/two.mp3", music + "/c-three.mp3"));
        const ScanResult r = scanner.scan({music});
        QCOMPARE(r.moved, 0);
        QCOMPARE(r.added, 1);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 2);
    }

    void locateFindsTracksOutsideTheLibrary() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("plain.flac"), music + "/plain.flac"));
        QVERIFY(put(fixture("plain2.flac"), music + "/plain2.flac"));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        QCOMPARE(scanner.scan({music}).added, 2);
        const qint64 id = trackId(library, "plain2.flac");
        QSqlQuery(library.db()).exec(QString("UPDATE tracks SET rating = 40 WHERE id = %1").arg(id));

        // Moved out of the library folder altogether: a rescan sees it gone.
        const QString elsewhere = dir.filePath("elsewhere");
        QDir().mkpath(elsewhere + "/deep");
        QVERIFY(QFile::rename(music + "/plain2.flac", elsewhere + "/deep/plain2.flac"));
        QVERIFY(put(fixture("untagged.mp3"), elsewhere + "/other.mp3"));
        QCOMPARE(scanner.scan({music}).missing, 1);

        const LocateResult r = scanner.locate(elsewhere);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.missingBefore, 1);
        QCOMPARE(r.located.size(), 1);
        QCOMPARE(r.missingAfter, 0);
        QCOMPARE(r.searched, 2);
        QCOMPARE(library.absolutePath(id), QFileInfo(elsewhere + "/deep/plain2.flac").canonicalFilePath());
        QCOMPARE(value(library, QString("SELECT rating FROM tracks WHERE id = %1").arg(id)).toInt(), 40);
        // Nothing else was added.
        QCOMPARE(value(library, "SELECT count(*) FROM tracks").toInt(), 2);
    }

    // ---- Quick rescans by folder time --------------------------------------

    void quickRescanSkipsUnchangedFolders() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("plain.flac"), music + "/a/plain.flac"));
        QVERIFY(put(fixture("plain2.flac"), music + "/b/plain2.flac"));
        QVERIFY(put(fixture("untagged.mp3"), music + "/c/untagged.mp3"));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        Scanner scanner(library, VolumeResolver());
        QCOMPARE(scanner.scan({music}).added, 3);

        ScanResult r = scanner.scan({music}, Scanner::Mode::ChangedFolders);
        QCOMPARE(r.foldersSkipped, 3);
        QCOMPARE(r.foldersListed, 1); // the root, which holds no audio and so has no stored time
        QCOMPARE(r.unchanged, 3);
        QCOMPARE(r.missing, 0);

        // A new file in b and a deleted one in c change those folders only.
        QVERIFY(put(fixture("extras.flac"), music + "/b/extras.flac"));
        QVERIFY(QFile::remove(music + "/c/untagged.mp3"));
        r = scanner.scan({music}, Scanner::Mode::ChangedFolders);
        QCOMPARE(r.foldersSkipped, 1);
        QCOMPARE(r.added, 1);
        QCOMPARE(r.missing, 1);
        QCOMPARE(r.unchanged, 2);
    }

    // ---- Watching ----------------------------------------------------------

    void watcherFollowsChanges() {
        QTemporaryDir dir;
        const QString music = QDir(dir.path()).canonicalPath() + "/music";
        QVERIFY(put(fixture("plain.flac"), music + "/album/plain.flac"));
        QDir().mkpath(music + "/empty");
        const QString db = dir.filePath("library.db");
        {
            Library library(db);
            QVERIFY(library.open());
            QCOMPARE(Scanner(library, VolumeResolver()).scan({music}).added, 1);
        }

        Watcher watcher(db);
        watcher.setDebounce(100);
        watcher.setMaxDelay(1000);
        QVERIFY(watcher.start());
        QCOMPARE(watcher.localRoots(), QStringList{music});
        QCOMPARE(watcher.watchedFolders(), 3);
        QVERIFY(!watcher.isDegraded());
        QSignalSpy finished(&watcher, &Watcher::rescanFinished);
        QSignalSpy changed(&watcher, &Watcher::libraryChanged);
        auto next = [&]() -> QJsonObject {
            if (finished.isEmpty() && !finished.wait(10000))
                return {};
            return finished.takeFirst().at(0).toJsonObject();
        };

        // Create.
        QVERIFY(put(fixture("plain2.flac"), music + "/album/plain2.flac"));
        QJsonObject r = next();
        QCOMPARE(r.value("kind").toString(), QString("changes"));
        QCOMPARE(r.value("added").toInt(), 1);
        QVERIFY(changed.size() >= 1);
        // Reading the new file must not look like a change to it: TagLib
        // opens read-only, so no close-after-write wakes the watcher again.
        QVERIFY(!finished.wait(800));

        // Modify: new contents, closed after writing.
        {
            QFile f(music + "/album/plain2.flac");
            QVERIFY(f.open(QIODevice::Append));
            f.write(QByteArray(16, '\0'));
        }
        r = next();
        QCOMPARE(r.value("updated").toInt(), 1);

        Library library(db);
        QVERIFY(library.open());
        const qint64 id = trackId(library, "album/plain2.flac");
        QVERIFY(id);

        // Move into another watched folder: the track keeps its row.
        QVERIFY(QFile::rename(music + "/album/plain2.flac", music + "/empty/moved.flac"));
        r = next();
        QCOMPARE(r.value("moved").toInt(), 1);
        QCOMPARE(trackId(library, "empty/moved.flac"), id);

        // A folder moved in from outside the library, with a file in it.
        const QString outside = dir.filePath("outside/New Album");
        QVERIFY(put(fixture("untagged.mp3"), outside + "/track.mp3"));
        QVERIFY(QFile::rename(outside, music + "/New Album"));
        r = next();
        QCOMPARE(r.value("added").toInt(), 1);
        QCOMPARE(watcher.watchedFolders(), 4);
        // ... and a file created in it later is seen, so the new folder is watched.
        QVERIFY(put(fixture("extras.flac"), music + "/New Album/extras.flac"));
        r = next();
        QCOMPARE(r.value("added").toInt(), 1);

        // Delete a file, then a whole folder.
        QVERIFY(QFile::remove(music + "/empty/moved.flac"));
        r = next();
        QCOMPARE(r.value("missing").toInt(), 1);
        QVERIFY(QDir(music + "/New Album").removeRecursively());
        r = next();
        QCOMPARE(r.value("missing").toInt(), 2);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 3);
        QCOMPARE(watcher.watchedFolders(), 3);

        // Files the scanner ignores don't wake it.
        QVERIFY(put(fixture("red.jpg"), music + "/album/notes.jpg"));
        QVERIFY(!finished.wait(600));
        watcher.stop();
        QVERIFY(!watcher.isRunning());
    }

    void watchLimitFallsBackToTheSchedule() {
        QTemporaryDir dir;
        const QString music = QDir(dir.path()).canonicalPath() + "/music";
        for (int i = 0; i < 5; ++i)
            QDir().mkpath(QString("%1/f%2").arg(music).arg(i));
        QVERIFY(put(fixture("plain.flac"), music + "/f0/plain.flac"));
        const QString db = dir.filePath("library.db");
        {
            Library library(db);
            QVERIFY(library.open());
            Scanner(library, VolumeResolver()).scan({music});
        }
        Watcher watcher(db);
        watcher.setMaxWatches(3);
        QSignalSpy limit(&watcher, &Watcher::watchLimitReached);
        watcher.start();
        QCOMPARE(limit.size(), 1);
        QVERIFY(watcher.isDegraded());
        QCOMPARE(watcher.watchedFolders(), 3);
        QCOMPARE(watcher.scheduledRoots(), QStringList{music});

        // The scheduled rescan lists only folders that changed.
        QSignalSpy finished(&watcher, &Watcher::rescanFinished);
        QVERIFY(put(fixture("plain2.flac"), music + "/f4/plain2.flac"));
        watcher.rescanScheduledNow();
        QVERIFY(finished.wait(10000));
        const QJsonObject r = finished.takeFirst().at(0).toJsonObject();
        QCOMPARE(r.value("kind").toString(), QString("scheduled"));
        QCOMPARE(r.value("added").toInt(), 1);
        QCOMPARE(r.value("folders_skipped").toInt(), 1); // f0, unchanged
    }

    void networkRootsAreNeverWatched() {
        // A fake machine where the music folder is a CIFS share.
        QTemporaryDir dir;
        const QString music = QDir(dir.path()).canonicalPath() + "/share";
        QVERIFY(put(fixture("plain.flac"), music + "/a/plain.flac"));
        QFile mountInfo(dir.filePath("mountinfo"));
        QVERIFY(mountInfo.open(QIODevice::WriteOnly));
        mountInfo.write(QString("1 0 0:1 / / rw - rootfs rootfs rw\n"
                                "2 1 0:2 / %1 rw,relatime shared:1 - cifs //nas/music rw\n")
                            .arg(music)
                            .toUtf8());
        mountInfo.close();
        QDir().mkpath(dir.filePath("by-uuid"));
        const VolumeResolver resolver(dir.filePath("mountinfo"), dir.filePath("by-uuid"));
        const QString db = dir.filePath("library.db");
        {
            Library library(db);
            QVERIFY(library.open());
            const ScanResult r = Scanner(library, resolver).scan({music});
            QVERIFY2(r.ok, qPrintable(r.error));
            QCOMPARE(value(library, "SELECT kind FROM volumes").toString(), QString("network"));
        }
        Watcher watcher(db, resolver);
        watcher.setNetworkInterval(3600);
        watcher.start();
        QCOMPARE(watcher.networkRoots(), QStringList{music});
        QVERIFY(watcher.localRoots().isEmpty());
        QCOMPARE(watcher.watchedFolders(), 0);
        QCOMPARE(watcher.scheduledRoots(), QStringList{music});
    }

    // ---- Artwork -----------------------------------------------------------

    void artworkComesFromFoldersAndTags() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        // A folder cover in upper case, an embedded cover, and no cover at all.
        QVERIFY(put(fixture("plain.flac"), music + "/Night Drive/plain.flac"));
        QVERIFY(put(fixture("plain2.flac"), music + "/Night Drive/plain2.flac"));
        QVERIFY(put(fixture("red.jpg"), music + "/Night Drive/Cover.JPG"));
        QVERIFY(put(fixture("blue.png"), music + "/Night Drive/back.png")); // not a cover name
        QVERIFY(put(fixture("embedded.mp3"), music + "/Blue/embedded.mp3"));
        QVERIFY(put(fixture("extras.flac"), music + "/Tags/extras.flac"));
        const QString db = dir.filePath("library.db");
        Library library(db);
        QVERIFY(library.open());
        QCOMPARE(Scanner(library, VolumeResolver()).scan({music}).added, 4);
        const qint64 nightDrive = value(library, "SELECT id FROM albums WHERE title = 'Night Drive'").toLongLong();
        const qint64 blue = value(library, "SELECT id FROM albums WHERE title = 'Blue'").toLongLong();
        const qint64 tags = value(library, "SELECT id FROM albums WHERE title = 'Tags'").toLongLong();
        QVERIFY(nightDrive && blue && tags);

        const QString cacheDir = dir.filePath("cache/art");
        ArtworkCache cache(library, cacheDir);
        const QString path = cache.albumArtwork(nightDrive, 200);
        QVERIFY2(!path.isEmpty(), qPrintable(cache.lastError()));
        QVERIFY(path.startsWith(cacheDir + '/'));
        QVERIFY(path.endsWith("-256.jpg"));
        QImage image(path);
        QCOMPARE(image.size(), QSize(256, 256));
        QVERIFY(qRed(image.pixel(128, 128)) > 200 && qBlue(image.pixel(128, 128)) < 60);
        // Every standard size is made from the one decode.
        QCOMPARE(QDir(cacheDir).entryList(QDir::Files).size(), 3);
        QCOMPARE(QImage(cache.albumArtwork(nightDrive, 96)).size(), QSize(96, 96));
        QCOMPARE(QImage(cache.albumArtwork(nightDrive, 4000)).size(), QSize(600, 600));
        QCOMPARE(value(library, "SELECT count(*) FROM artwork").toInt(), 1);
        QCOMPARE(value(library, QString("SELECT width FROM artwork")).toInt(), 800);
        QVERIFY(value(library, QString("SELECT source FROM artwork")).toString().endsWith("/Cover.JPG"));
        QCOMPARE(value(library, QString("SELECT count(*) FROM albums WHERE artwork_id IS NOT NULL")).toInt(), 1);

        // Embedded: 500x300 fits 256 wide.
        const QString embedded = cache.albumArtwork(blue, 256);
        QVERIFY2(!embedded.isEmpty(), qPrintable(cache.lastError()));
        QCOMPARE(QImage(embedded).size(), QSize(500, 300).scaled(256, 256, Qt::KeepAspectRatio));
        const QImage e(embedded);
        QVERIFY(qBlue(e.pixel(100, 70)) > 200 && qRed(e.pixel(100, 70)) < 60);
        // The track's own picture is the same image, so the same files.
        const qint64 track = value(library, "SELECT id FROM tracks WHERE title = 'Embedded'").toLongLong();
        QCOMPARE(cache.trackArtwork(track, 256), embedded);
        // A track without one gets its album's.
        const qint64 plain = value(library, "SELECT id FROM tracks WHERE title = 'Plain Song'").toLongLong();
        QCOMPARE(cache.trackArtwork(plain, 256), path);

        // No picture: empty, and the album is not searched again.
        QVERIFY(cache.albumArtwork(tags, 256).isEmpty());
        QVERIFY(!value(library, QString("SELECT art_checked FROM albums WHERE id = %1").arg(tags)).isNull());

        // A cleared cache is made again from the recorded source.
        QDir(cacheDir).removeRecursively();
        QCOMPARE(QImage(cache.albumArtwork(nightDrive, 256)).size(), QSize(256, 256));

        // The image provider serves the same files to QML.
        ArtImageProvider provider(db, cacheDir);
        QString kind;
        qint64 number = 0;
        int size = 0;
        QVERIFY(ArtImageProvider::parseId("album/12/96?v=2", &kind, &number, &size));
        QCOMPARE(kind, QString("album"));
        QCOMPARE(number, 12);
        QCOMPARE(size, 96);
        QVERIFY(!ArtImageProvider::parseId("artist/1", &kind, &number, &size));
        {
            std::unique_ptr<QQuickImageResponse> response(
                provider.requestImageResponse(QString("album/%1/96").arg(blue), QSize()));
            QSignalSpy done(response.get(), &QQuickImageResponse::finished);
            QVERIFY(done.wait(10000) || done.size() == 1);
            QVERIFY(response->errorString().isEmpty());
            std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
            QCOMPARE(texture->image().size(), QSize(500, 300).scaled(96, 96, Qt::KeepAspectRatio));
        }
        {
            std::unique_ptr<QQuickImageResponse> response(
                provider.requestImageResponse(QString("album/%1/96").arg(tags), QSize(50, 50)));
            QSignalSpy done(response.get(), &QQuickImageResponse::finished);
            QVERIFY(done.wait(10000) || done.size() == 1);
            QVERIFY(!response->errorString().isEmpty());
        }
    }

    void prefetchMakesTheRestInTheBackground() {
        QTemporaryDir dir;
        const QString music = dir.filePath("music");
        QVERIFY(put(fixture("plain.flac"), music + "/One/plain.flac"));
        QVERIFY(put(fixture("red.jpg"), music + "/One/folder.jpg"));
        QVERIFY(put(fixture("embedded.mp3"), music + "/Two/embedded.mp3"));
        QVERIFY(put(fixture("extras.flac"), music + "/Three/extras.flac"));
        const QString db = dir.filePath("library.db");
        {
            Library library(db);
            QVERIFY(library.open());
            QCOMPARE(Scanner(library, VolumeResolver()).scan({music}).added, 3);
        }
        ArtworkPrefetch prefetch(db, dir.filePath("art"));
        QSignalSpy finished(&prefetch, &ArtworkPrefetch::finished);
        prefetch.start(256);
        QVERIFY(finished.wait(20000));
        QCOMPARE(finished.first().at(0).toInt(), 2);
        Library library(db);
        QVERIFY(library.open());
        QCOMPARE(value(library, "SELECT count(*) FROM albums WHERE art_checked IS NULL").toInt(), 0);

        // A rescan that changes an album's tracks makes it look again.
        touch(music + "/Three/extras.flac", 5);
        QCOMPARE(Scanner(library, VolumeResolver()).scan({music}).updated, 1);
        QCOMPARE(value(library, "SELECT count(*) FROM albums WHERE art_checked IS NULL").toInt(), 1);
    }
};

int runWatcherTests(int argc, char **argv) {
    WatcherTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "watcher_tests.moc"
