// Performance tests for the library core, without a window.
//
//   omamp_bench generate <library.db> [--tracks N]   a synthetic library (rows only, no files)
//   omamp_bench run <library.db> [--runs N]          time opening, search and queries
//   omamp_bench files <folder> <template-file> [--files N] [--db path] [--latency-us U] [--walk-workers W]
//                                                     scan and rescan N hard links of one file; with
//                                                     --latency-us the rescans pay U microseconds a round trip
//   omamp_bench nas [--files N] [--latency-us U] [--workers 1,4,8,16]
//                                                     the network rescan cost model: walk a tree of N files
//                                                     held in memory, with U microseconds per listing and stat
//                                                     scan and rescan N hard links of one file
//
// The generator streams: rows go in through prepared statements and are
// committed every few thousand, so memory stays flat however many tracks.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include <sys/resource.h>
#include <unistd.h>

#include "library.h"
#include "palette.h"
#include "query.h"
#include "scanner.h"
#include "volumes.h"
#include "walker.h"

static QTextStream out(stdout), err(stderr);

static const QStringList adjectives = {
    "Neon", "Silver", "Midnight", "Velvet", "Electric", "Golden", "Broken", "Crystal", "Hollow", "Wild",
    "Quiet", "Burning", "Frozen", "Distant", "Lunar", "Solar", "Paper", "Iron", "Scarlet", "Northern",
    "Southern", "Eastern", "Western", "Lost", "Secret", "Little", "Great", "Last", "First", "Endless",
    "Restless", "Sleepy", "Shiny", "Rusty", "Copper", "Amber", "Blue", "Black", "White", "Green"};
static const QStringList nouns = {
    "Coast", "Highway", "Garden", "Engine", "River", "Mountain", "Harbour", "Signal", "Mirror", "Echo",
    "Forest", "Desert", "Ocean", "Machine", "Parade", "Theory", "Kingdom", "Station", "Satellite", "Lantern",
    "Horizon", "Canyon", "Valley", "Island", "Avenue", "Circus", "Orchestra", "Choir", "Collective", "Brothers",
    "Sisters", "Society", "Club", "Band", "Project", "Union", "Riders", "Drifters", "Dreamers", "Wolves"};
static const QStringList words = {
    "love", "night", "drive", "heart", "light", "rain", "fire", "summer", "winter", "dance", "dream", "road",
    "home", "city", "stars", "moon", "sun", "shadow", "girl", "boy", "time", "world", "blue", "gold", "wave",
    "run", "fall", "rise", "song", "storm", "sky", "sea", "train", "radio", "telephone", "window", "door",
    "morning", "evening", "tonight", "forever", "never", "again", "goodbye", "hello", "yesterday", "tomorrow",
    "electric", "magnetic", "paradise", "highway", "memory", "kiss", "echo", "river", "garden", "silence"};
static const QStringList genres = {
    "Rock", "Pop", "Jazz", "Blues", "Metal", "Electronic", "Synthwave", "Hip Hop", "Soul", "Funk", "Country",
    "Folk", "Reggae", "Punk", "Indie", "Ambient", "Classical", "Soundtrack", "Disco", "House", "Techno",
    "Trance", "Drum and Bass", "R&B", "Gospel", "Latin", "World", "New Age", "Christmas", "Alternative",
    "Grunge", "Ska", "Swing", "Bluegrass", "Opera", "Chillout", "Lo-fi", "Shoegaze", "Post-rock", "Dub"};
static const QStringList formats = {"flac", "flac", "mp3", "mp3", "mp3", "m4a", "ogg", "opus"};

static qint64 peakRssMb() {
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

static bool check(QSqlQuery &q) {
    if (q.exec())
        return true;
    err << "SQL error: " << q.lastError().text() << Qt::endl;
    return false;
}

static int generate(const QString &path, int trackCount) {
    QFile::remove(path);
    QFile::remove(path + "-wal");
    QFile::remove(path + "-shm");
    Library library(path);
    QString error;
    if (!library.open(&error)) {
        err << error << Qt::endl;
        return 1;
    }
    QSqlDatabase db = library.db();
    QSqlQuery(db).exec("PRAGMA synchronous = OFF");
    QRandomGenerator rng(20261019);
    auto pick = [&](const QStringList &list) { return list[rng.bounded(list.size())]; };
    auto title = [&] {
        QStringList t;
        const int n = 1 + rng.bounded(4);
        for (int i = 0; i < n; ++i)
            t << pick(words);
        t[0][0] = t[0][0].toUpper();
        return t.join(' ');
    };

    QElapsedTimer timer;
    timer.start();
    db.transaction();
    QSqlQuery q(db);
    q.exec("INSERT INTO volumes (key, kind, label, fs_type, source, root_path, online, last_seen, added) "
           "VALUES ('uuid:BENCH-0000', 'local', 'bench', 'ext4', '/dev/bench', '/bench', 1, 0, 0)");

    // Artists: every adjective-noun pair, "Neon Coast" among them.
    QStringList artistNames;
    for (const QString &a : adjectives)
        for (const QString &n : nouns)
            artistNames << a + ' ' + n;
    QSqlQuery addArtist(db);
    addArtist.prepare("INSERT INTO artists (id, name) VALUES (?, ?)");
    for (int i = 0; i < artistNames.size(); ++i) {
        addArtist.addBindValue(i + 1);
        addArtist.addBindValue(artistNames[i]);
        check(addArtist);
    }
    QSqlQuery addGenre(db);
    addGenre.prepare("INSERT INTO genres (id, name) VALUES (?, ?)");
    for (int i = 0; i < genres.size(); ++i) {
        addGenre.addBindValue(i + 1);
        addGenre.addBindValue(genres[i]);
        check(addGenre);
    }

    QSqlQuery addFolder(db), addAlbum(db), addTrack(db), linkArtist(db), linkGenre(db);
    addFolder.prepare("INSERT INTO folders (id, volume_id, rel_path, is_root, added) VALUES (?, 1, ?, 0, 0)");
    addAlbum.prepare("INSERT INTO albums (id, title, album_artist, artist_key, title_key, year, compilation) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?)");
    addTrack.prepare(
        "INSERT INTO tracks (id, volume_id, folder_id, rel_path, file_name, format, size, mtime_ns, title, artist, "
        "album_artist, sort_artist, album, album_id, genre, year, track_no, track_total, disc_no, disc_total, "
        "compilation, duration_ms, bitrate, sample_rate, channels, rating, play_count, last_played, added, "
        "content_type) VALUES (?, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 44100, 2, ?, ?, ?, ?, ?)");
    linkArtist.prepare("INSERT OR IGNORE INTO track_artists (track_id, artist_id, role, position) VALUES (?, ?, ?, ?)");
    linkGenre.prepare("INSERT OR IGNORE INTO track_genres (track_id, genre_id, position) VALUES (?, ?, ?)");
    QSqlQuery(db).exec("INSERT INTO folders (id, volume_id, rel_path, is_root, added) VALUES (1, 1, '', 1, 0)");

    int trackId = 0, albumId = 0;
    const qint64 now = 1792000000;
    while (trackId < trackCount) {
        ++albumId;
        // A skewed pick, so some artists have many albums and most have few.
        const int artistIndex = int(std::pow(rng.generateDouble(), 2.5) * artistNames.size());
        const QString albumArtist = artistNames[artistIndex];
        const bool compilation = rng.bounded(100) < 5;
        const QString album = title();
        const int year = 1960 + rng.bounded(66);
        const int discs = rng.bounded(100) < 10 ? 2 : 1;
        const int perDisc = 6 + rng.bounded(9);
        const QString genre1 = pick(genres), genre2 = rng.bounded(100) < 20 ? pick(genres) : QString();
        const QString genre = genre2.isEmpty() || genre2 == genre1 ? genre1 : genre1 + "; " + genre2;
        const QString shownArtist = compilation ? QString("Various Artists") : albumArtist;
        // Random titles repeat, so the album number keeps folders apart.
        const QString folder = QString("%1/%2 (%3) %4").arg(shownArtist, album).arg(year).arg(albumId);

        addFolder.addBindValue(albumId + 1);
        addFolder.addBindValue(folder);
        check(addFolder);
        addAlbum.addBindValue(albumId);
        addAlbum.addBindValue(album);
        addAlbum.addBindValue(shownArtist);
        addAlbum.addBindValue(shownArtist.toLower());
        addAlbum.addBindValue(album.toLower() + QString(" #%1").arg(albumId)); // titles repeat; keep albums apart
        addAlbum.addBindValue(year);
        addAlbum.addBindValue(compilation ? 1 : 0);
        check(addAlbum);

        for (int disc = 1; disc <= discs && trackId < trackCount; ++disc) {
            for (int n = 1; n <= perDisc && trackId < trackCount; ++n) {
                ++trackId;
                QStringList artists{compilation ? artistNames[rng.bounded(artistNames.size())] : albumArtist};
                if (rng.bounded(100) < 8)
                    artists << artistNames[rng.bounded(artistNames.size())];
                const QString trackTitle = title();
                const QString format = pick(formats);
                const QString fileName = QString("%1-%2 %3").arg(disc).arg(n, 2, 10, QChar('0')).arg(trackTitle);
                const bool rated = rng.bounded(100) < 40;
                const int plays = rng.bounded(100) < 50 ? 0 : rng.bounded(200);
                const QList<QVariant> values = {
                    trackId, albumId + 1, folder + '/' + fileName + '.' + format, fileName, format,
                    qint64(3000000 + rng.bounded(40000000)), qint64(now - rng.bounded(300000000)) * 1000000000LL,
                    trackTitle, artists.join("; "), compilation ? QString() : albumArtist, shownArtist, album,
                    albumId, genre, year, n, perDisc, disc, discs, compilation ? 1 : 0,
                    120000 + rng.bounded(300000), format == "flac" ? 900 + rng.bounded(300) : 128 + rng.bounded(192),
                    rated ? QVariant(20 * (1 + rng.bounded(5))) : QVariant(QMetaType(QMetaType::Int)), plays,
                    plays ? QVariant(now - rng.bounded(100000000)) : QVariant(QMetaType(QMetaType::LongLong)),
                    now - rng.bounded(300000000), genre1 == "Classical" ? "classical" : "music"};
                for (const QVariant &v : values)
                    addTrack.addBindValue(v);
                if (!check(addTrack))
                    return 1;
                for (int a = 0; a < artists.size(); ++a) {
                    linkArtist.addBindValue(trackId);
                    linkArtist.addBindValue(artistNames.indexOf(artists[a]) + 1);
                    linkArtist.addBindValue("artist");
                    linkArtist.addBindValue(a);
                    check(linkArtist);
                }
                if (!compilation) {
                    linkArtist.addBindValue(trackId);
                    linkArtist.addBindValue(artistIndex + 1);
                    linkArtist.addBindValue("album_artist");
                    linkArtist.addBindValue(0);
                    check(linkArtist);
                }
                linkGenre.addBindValue(trackId);
                linkGenre.addBindValue(genres.indexOf(genre1) + 1);
                linkGenre.addBindValue(0);
                check(linkGenre);
                if (!genre2.isEmpty() && genre2 != genre1) {
                    linkGenre.addBindValue(trackId);
                    linkGenre.addBindValue(genres.indexOf(genre2) + 1);
                    linkGenre.addBindValue(1);
                    check(linkGenre);
                }
                if (trackId % 5000 == 0) {
                    db.commit();
                    db.transaction();
                    err << "\r" << trackId << " tracks" << Qt::flush;
                }
            }
        }
    }
    db.commit();
    QSqlQuery(db).exec("ANALYZE");
    err << "\r";
    out << QString("Generated %1 tracks, %2 albums, %3 artists in %4 s (peak memory %5 MB)")
               .arg(trackId)
               .arg(albumId)
               .arg(artistNames.size())
               .arg(timer.elapsed() / 1000.0, 0, 'f', 1)
               .arg(peakRssMb())
        << Qt::endl;
    return 0;
}

// Median of several runs, in milliseconds.
template <typename F> static double timeIt(int runs, F &&body) {
    QList<double> times;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer t;
        t.start();
        body();
        times << t.nsecsElapsed() / 1e6;
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

static int runBench(const QString &path, int runs) {
    if (!QFile::exists(path)) {
        err << path << " does not exist; run generate first" << Qt::endl;
        return 1;
    }
    out << "| What | Median of " << runs << " (ms) | Rows |\n|---|---:|---:|\n";
    auto row = [&](const QString &what, double ms, qint64 rows) {
        out << "| " << what << " | " << QString::number(ms, 'f', 1) << " | " << rows << " |\n" << Qt::flush;
    };

    // Opening: connect, pragmas and the schema check, as the window does first.
    row("Open the library", timeIt(runs, [&] {
            Library l(path);
            l.open();
        }), 0);

    Library library(path);
    if (!library.open())
        return 1;
    qint64 rows = 0;
    // What a track list needs to draw its first screen: the count and one page.
    // (Timed before row() is called: the order arguments are evaluated in is
    // unspecified, so rows could be read before the timing fills it.)
    double ms = timeIt(runs, [&] {
        Query::Options o;
        o.limit = 100;
        rows = Query::run(library, "", o).total;
    });
    row("First page (count + 100 sorted rows)", ms, rows);

    const QList<QPair<QString, QString>> queries = {
        {"Word search (FTS): coast", "coast"},
        {"Two words (FTS): midnight highway", "midnight highway"},
        {"Short word (no index): go", "go"},
        {"artist:\"neon coast\" year:1997..2001", "artist:\"neon coast\" year:1997..2001"},
        {"rating:4.. -genre:christmas", "rating:4.. -genre:christmas"},
        {"genre:rock OR genre:metal", "genre:rock OR genre:metal"},
        {"artist:=\"Neon Coast\" (multi-value table)", "artist:=\"Neon Coast\""},
        {"title:/^love/ (regex)", "title:/^love/"},
        // Phase 1B: the rest of the language.
        {"\"midnight highway\" (phrase)", "\"midnight highway\""},
        {"+love (word start, index + pattern)", "+love"},
        {"+go (short word start, no index)", "+go"},
        {"aa:\"neon coast\" (alias)", "aa:\"neon coast\""},
        {"added:<30d", "added:<30d"},
        {"added:2024", "added:2024"},
        {"added:2024-01..2024-06", "added:2024-01..2024-06"},
        {"played:>1y", "played:>1y"},
        {"played:never", "played:never"},
        {"lastplayed:<7d", "lastplayed:<7d"},
        {"plays:0", "plays:0"},
        {"plays:10..", "plays:10.."},
        {"length:>6m", "length:>6m"},
        {"length:3:00..5:00", "length:3:00..5:00"},
        {"album:none", "album:none"},
        {"genre:any", "genre:any"},
        {"-genre:none", "-genre:none"},
        {"sort:-rating,artist limit:50", "sort:-rating,artist limit:50"},
        {"sort:random limit:50", "sort:random limit:50"},
        {"sort:random (every row shuffled)", "sort:random"},
        {"sort:randomalbum limit:100", "sort:randomalbum limit:100"},
        {"limit:2h (library order)", "limit:2h"},
        {"limit:700mb (library order)", "limit:700mb"},
        {"rating:4.. sort:random limit:2h", "rating:4.. sort:random limit:2h"},
        {"added:<1y plays:0 sort:-added limit:2h", "added:<1y plays:0 sort:-added limit:2h"},
        {"genre:rock played:>1y sort:-plays limit:50", "genre:rock played:>1y sort:-plays limit:50"},
    };
    for (const auto &q : queries) {
        Query::Options o;
        o.limit = 100;
        qint64 total = 0;
        const double ms = timeIt(runs, [&] {
            const Query::Result r = Query::run(library, q.second, o);
            if (!r.ok())
                err << q.second << ": " << r.error << Qt::endl;
            total = r.total;
        });
        row("query " + q.first + " (page of 100 + count)", ms, total);
    }
    // The worst case: every row, sorted, loaded into memory.
    ms = timeIt(std::min(runs, 3), [&] { rows = Query::run(library, "").tracks.size(); });
    row("query \"\" with no limit (all rows, sorted)", ms, rows);
    row("stats", timeIt(runs, [&] { library.stats(); }), 0);

    // The command palette: one search per keystroke, on the window's thread.
    {
        Palette palette;
        palette.addAction("rescan", "Rescan library", {"scan"});
        palette.addAction("play-pause", "Play/Pause", {"pause"}, "Space");
        QElapsedTimer first;
        first.start();
        palette.setLibrary(&library);
        palette.setText("coast");
        row("palette: first search, loading artist and album names", first.nsecsElapsed() / 1e6, palette.count());
        for (const QString &text : {"coast", "neon night", "lov", "re", "midnight highway", "artist:neon sort:-plays"}) {
            int count = 0;
            const double ms = timeIt(runs, [&] {
                palette.setText(QString());
                palette.setText(text);
                count = palette.count();
            });
            row("palette: " + text, ms, count);
        }
    }
    out << "\nPeak memory: " << peakRssMb() << " MB\n"
        << "Database size: " << QString::number(QFileInfo(path).size() / 1e6, 'f', 0) << " MB\n"
        << Qt::flush;
    return 0;
}

// A stat-only rescan needs real files. Hard links of one small tagged file
// give as many as wanted without the disk space.
static int files(const QString &folder, const QString &templateFile, int count, const QString &dbPath,
                 int latencyUs, int walkWorkers) {
    QDir().mkpath(folder);
    QDir().mkpath(folder + "-templates");
    const QString suffix = QFileInfo(templateFile).suffix();
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < count; ++i) {
        const QString dir = QString("%1/artist%2/album%3").arg(folder).arg(i / 12 / 80).arg(i / 12);
        const QString file = QString("%1/%2.%3").arg(dir).arg(i % 12 + 1, 2, 10, QChar('0')).arg(suffix);
        if (QFileInfo::exists(file))
            continue;
        if (i % 12 == 0)
            QDir().mkpath(dir); // one new album folder every 12 files
        // ext4 allows 65,000 links to one file, so start a fresh copy every 50,000.
        const QString source = QString("%1-templates/template-%2.%3").arg(folder).arg(i / 50000).arg(suffix);
        if (!QFileInfo::exists(source) && !QFile::copy(templateFile, source)) {
            err << "Can't copy " << templateFile << Qt::endl;
            return 1;
        }
        if (link(QFile::encodeName(source).constData(), QFile::encodeName(file).constData()) != 0) {
            err << "Can't link " << file << Qt::endl;
            return 1;
        }
    }
    out << "Made " << count << " links in " << timer.elapsed() / 1000.0 << " s\n";
    QFile::remove(dbPath);
    Library library(dbPath);
    if (!library.open())
        return 1;
    VolumeResolver resolver;
    Scanner scanner(library, resolver);
    ScanResult first = scanner.scan({folder});
    if (!first.ok) {
        err << first.error << Qt::endl;
        return 1;
    }
    const qint64 firstPeak = peakRssMb();
    out << "| What | ms | Files |\n|---|---:|---:|\n"
        << "| First scan (TagLib on " << Scanner::defaultWorkers() << " threads) | " << first.elapsedMs << " | "
        << first.files << " |\n";
    std::shared_ptr<LatencyFileSystem> slow;
    if (walkWorkers > 0)
        scanner.setWalkWorkers(walkWorkers);
    if (latencyUs > 0) {
        // The rescans go through a share that costs latencyUs a round trip.
        slow = std::make_shared<LatencyFileSystem>(FileSystem::posix(), latencyUs);
        scanner.setFileSystem(slow);
        out << "| Rescans below: " << latencyUs << " µs per listing and stat, "
            << (walkWorkers > 0 ? walkWorkers : Volumes::defaultLocalWorkers) << " walkers | | |\n";
    }
    ScanResult again = scanner.scan({folder});
    out << "| Rescan, nothing changed (stat only) | " << again.elapsedMs << " | " << again.unchanged << " |\n";
    ScanResult quick = scanner.scan({folder}, Scanner::Mode::ChangedFolders);
    out << "| Quick rescan by folder time (" << quick.foldersSkipped << " folders skipped) | " << quick.elapsedMs
        << " | " << quick.unchanged << " |\n"
        << (slow ? QString("\nRound trips over both rescans: %1 listings, %2 stats\n").arg(slow->listings()).arg(slow->stats())
                 : QString())
        << "\nPeak memory after the first scan: " << firstPeak << " MB\n"
        << "Peak memory: " << peakRssMb() << " MB\n"
        << Qt::flush;
    return 0;
}

// What a rescan of a big library costs over a network, measured with real
// waits: a tree of artist/album/track folders held in memory, every listing
// and every stat paying the round trip. Full = every file stat'ed (the
// ordinary rescan); quick = folders whose modification time is unchanged
// are not stat'ed inside (Scanner::Mode::ChangedFolders).
static int nas(int count, int latencyUs, const QList<int> &workers) {
    auto memory = std::make_shared<MemoryFileSystem>();
    const int perAlbum = 10, perArtist = 10;
    int folders = 1;
    for (int i = 0; i < count; ++i) {
        const int album = i / perAlbum, artist = album / perArtist;
        if (i % perAlbum == 0)
            folders += album % perArtist == 0 ? 2 : 1;
        memory->addFile(QString("/nas/music/artist%1/album%2/%3.flac").arg(artist).arg(album).arg(i % perAlbum + 1).toUtf8(),
                        30000000 + i, 1700000000000000000LL);
    }
    WalkOptions base;
    base.isAudio = [](const QString &name) { return name.endsWith(".flac"); };
    // Folder times as a scan would have stored them, for the quick walks.
    WalkOutput reference;
    {
        WalkOptions o = base;
        o.fs = memory;
        o.workers = 8;
        reference = Walker::walk("/nas/music", "", 1, true, o);
    }
    out << "Tree: " << reference.files.size() << " files in " << folders << " folders; " << latencyUs
        << " µs per round trip\n\n"
        << "| Walk | Walkers | Listings | Stats | Measured s | Model s |\n|---|---:|---:|---:|---:|---:|\n";
    for (const bool quick : {false, true}) {
        for (int w : workers) {
            auto slow = std::make_shared<LatencyFileSystem>(memory, latencyUs);
            WalkOptions o = base;
            o.fs = slow;
            o.workers = w;
            if (quick)
                o.known = &reference.folders;
            QElapsedTimer timer;
            timer.start();
            const WalkOutput result = Walker::walk("/nas/music", "", 1, true, o);
            const double seconds = timer.elapsed() / 1000.0;
            // The model: every round trip waits once, spread over the walkers.
            const double model = double(slow->listings() + slow->stats()) * latencyUs / 1e6 / w;
            out << "| " << (quick ? "quick (folder times)" : "full (stat every file)") << " | " << w << " | "
                << slow->listings() << " | " << slow->stats() << " | " << QString::number(seconds, 'f', 2) << " | "
                << QString::number(model, 'f', 2) << " |\n"
                << Qt::flush;
            if (!quick && result.files.size() != reference.files.size())
                return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    auto option = [&](const QString &name, int fallback) {
        const int at = args.indexOf(name);
        if (at < 0 || at + 1 >= args.size())
            return fallback;
        const int value = args[at + 1].toInt();
        args.remove(at, 2);
        return value;
    };
    QString dbOption;
    if (const int at = args.indexOf("--db"); at >= 0 && at + 1 < args.size()) {
        dbOption = args[at + 1];
        args.remove(at, 2);
    }
    const int tracks = option("--tracks", 200000);
    const int runs = option("--runs", 7);
    const int fileCount = option("--files", 100000);
    const int latencyUs = option("--latency-us", 0);
    const int walkWorkers = option("--walk-workers", 0);
    QList<int> workerList{1, 4, 8, 16};
    if (const int at = args.indexOf("--workers"); at >= 0 && at + 1 < args.size()) {
        workerList.clear();
        for (const QString &w : args[at + 1].split(','))
            workerList << qMax(1, w.toInt());
        args.remove(at, 2);
    }
    if (args.value(0) == "nas" && args.size() == 1)
        return nas(fileCount, latencyUs > 0 ? latencyUs : 500, workerList);
    if (args.value(0) == "generate" && args.size() == 2)
        return generate(args[1], tracks);
    if (args.value(0) == "run" && args.size() == 2)
        return runBench(args[1], runs);
    if (args.value(0) == "files" && args.size() == 3)
        return files(args[1], args[2], fileCount, dbOption.isEmpty() ? args[1] + "/../files-library.db" : dbOption,
                     latencyUs, walkWorkers);
    err << "usage: omamp_bench generate <db> [--tracks N] | run <db> [--runs N] | "
           "files <folder> <template> [--files N] [--db path] [--latency-us U] [--walk-workers W] | "
           "nas [--files N] [--latency-us U] [--workers 1,4,8,16]"
        << Qt::endl;
    return 1;
}
