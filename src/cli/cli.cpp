#include "cli.h"

#include "job.h"
#include "library.h"
#include "query.h"
#include "scanner.h"
#include "volumes.h"
#include "artwork.h"
#include "audit.h"
#include "duplicates.h"
#include "mask.h"
#include "organise.h"
#include "tagwriter.h"
#include "watcher.h"
#include "mounts.h"
#include "autodj.h"
#include "m3u.h"
#include "mmimport.h"
#include "playlists.h"
#include "stats.h"
#include "webcli.h"

#include <QDateTime>

#include <QFile>
#include <QFileInfo>
#include <QSqlQuery>

#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <unistd.h>

namespace Cli {

namespace {

const QStringList commands = {"scan", "query", "stats", "help", "--help", "-h", "version", "--version",
                               "watch", "locate", "art", "tag", "undo", "journal", "organise", "organize",
                               "tag-from-filename", "audit", "duplicates", "playlist", "autoplaylist",
                               "collection", "autodj", "import", "volume", "nas", "remote"};

const char *usage = R"(Usage: omamp [files or folders...]      open the player
       omamp open [files or folders...]  the same, for names that look like commands
       omamp mini [files or folders...]  open as the mini player (a 420x64 strip)
       omamp micro [files or folders...] open as the micro player (Winamp's windowshade)
       omamp scan [folder...]            add folders to the library and scan them;
                                         with no folder, rescan every library folder
       omamp query <text...>             search the library
       omamp stats                       count what is in the library
       omamp watch                       watch the library folders and rescan what changes
                                         (in the foreground; Ctrl+C stops)
       omamp locate <folder>             find missing tracks that moved into a folder
       omamp art album|track <id>        print the path of a picture (made if needed)
       omamp art all                     make pictures for every album not done yet
       omamp tag <query> --set field=value...
                                         write tags to every track the query finds
       omamp tag-from-filename <query> --pattern '<Track#> - <Title>'
                                         fill tags from file names and folders
       omamp organise <query> --mask '<Album Artist>/<Album>/<Track#:2> <Title>'
                       --dry-run | --apply
                                         rename and move files by a mask
       omamp undo [batch]                undo the last tag edit or organise (or a batch)
       omamp journal [batch]             list recent batches, or one batch's changes
       omamp audit [check]               count tracks needing work (Files to Edit),
                                         or list one check's tracks
       omamp duplicates [--by tags|content|fingerprint]
                                         find the same song twice; nothing is deleted
       omamp playlist list|show|create|add|remove|move|rename|delete|clear|reroll|import|export
                                         playlists, playlist folders and playlist files
       omamp autoplaylist create <name> '<query>'
                                         a playlist that is a saved query, always up to date
       omamp collection list|show|create|remove|restore-defaults
       omamp stats --top tracks|artists|albums|years|genres [--since 30d]
       omamp stats --recent | --never    what was played lately, or never
       omamp autodj [--source library|collection:<name>|playlist:<name>|query:<text>]
                    [--count 20] [--seed n]
                                         pick tracks the way Auto-DJ would
       omamp import mediamonkey <MM.DB> [--map 'D:\Music=/mnt/music'] --dry-run | --apply
                                         ratings, play counts, history and playlists
       omamp import undo [batch]         undo an import
       omamp volume list|show <v>        the volumes (disks and shares) and whether they are online
       omamp volume reassign <v> <folder>
                                         point a volume at its music's new place (re-assign drive)
       omamp volume forget <v> [--force] remove a volume and its tracks from the library
       omamp volume workers <v> <n>      folders listed at once when scanning it (0: default)
       omamp nas mount-guide --smb //nas/music --at /mnt/music [--write-to <folder>]
       omamp nas mount-guide --nfs nas:/export/music --at /mnt/music
                                         print systemd automount units and the sudo commands
       omamp remote add subsonic <url> --user <name> --password-stdin | --api-key-stdin [--name n]
       omamp remote list|sync [source|all] [--full]|remove <source> [--keep-tracks]
       omamp remote set <source> --max-bitrate <kbit/s> --format <raw|mp3|opus|...>
                                         Navidrome and other (Open)Subsonic servers as sources

Options:
  --json         print JSON on stdout
  --db <path>    use this library file (default ~/.local/share/omamp/library.db)
  --limit <n>    query: print at most n tracks (after the query's own limit:)
  --offset <n>   query: skip the first n tracks
  --count        query: print only how many tracks match
  --seed <n>     query: the seed for sort:random (default: a new one each run)
  --explain      query: print the parsed query and its SQL instead of running it
  --workers <n>  scan: threads reading tags (default 4 to 8)
  --quick        scan: skip folders whose modification time hasn't changed
  --interval <s> watch: seconds between rescans of network folders (default 900)
  --dry-run      watch: print changes without rescanning; tag, tag-from-filename,
                 organise: show the changes and write nothing
  --size <px>    art: 96, 256 or 600 (default 256)
  --set f=v      tag: set a field (repeat it); fields: title, artist, albumartist, album,
                 genre, composer, year, originalyear, track, tracktotal, disc, disctotal,
                 comment, bpm, compilation, lyrics, rating (stars), artwork (an image
                 file), custom1..custom5, isrc, label, catalognumber, releasecountry,
                 mbrecordingid, mbreleasetrackid, mbreleaseid, mbartistid, mbalbumartistid,
                 mbreleasegroupid, lrcfile (the .lrc beside the track). An empty value
                 clears the field.
  --set-mask f=m tag: set a field from a mask, per track: --set-mask 'album=<Folder:1>'
  --apply        organise: move the files
  --mask <mask>  organise: where files go (see docs/masks.md)
  --to <folder>  organise: under this folder instead of each track's library folder
  --keep-folders organise: leave emptied folders
  --pattern <p>  tag-from-filename: what file names look like
  --by <how>     duplicates: tags (default), content or fingerprint
  --any-length   duplicates: match tags whatever the lengths
  --parent <p>   playlist, autoplaylist: inside this playlist folder ("/" for the top)
  --folder       playlist create: make a folder for other playlists
  --ids <a,b>    playlist add: track ids; playlist remove: entry ids; autodj: what was played
  --at <n>       playlist add: insert at entry n; playlist move: the playlist's new place
  --name <n>     playlist import: the new playlist's name
  --relative     playlist export: paths relative to the playlist file
  --top <what>   stats: most played tracks, artists, albums, years or genres
  --since <age>  stats: only plays since then (30d, 2w, 6m, 1y, 2026-01-01)
  --recent       stats: the latest plays
  --never        stats: tracks never played
  --source <s>   autodj: library, collection:<name>, playlist:<name> or query:<text>
  --count <n>    autodj: how many tracks to pick
  --separation <n>  autodj: no artist twice within n tracks
  --days <n>     autodj: leave out tracks played in the last n days
  --map w=l      import mediamonkey: a Windows path prefix and the Linux folder it is now
  --overwrite    import mediamonkey: take MediaMonkey's values instead of merging

Tag and organise examples:
  omamp tag 'album:"night drive"' --set genre=Synthwave --set year=1997
  omamp tag-from-filename 'artist:none' --pattern '<Artist>/<Album>/<Track#> - <Title>' --dry-run
  omamp organise 'path:inbox' --mask '<Album Artist>/<Album> (<Year>)/<Track#:2> <Title>' --dry-run
  omamp undo

Query examples:
  omamp query neon coast
  omamp query 'artist:"neon coast" year:1997..2001'
  omamp query 'rating:4.. -genre:christmas'
  omamp query 'genre:rock OR genre:metal'
  omamp query 'added:<30d plays:0 sort:-added'
  omamp query 'rating:4.. played:>1y sort:random limit:2h'
  omamp query --count 'genre:none'
)";

struct Options {
    bool json = false;
    QString db;
    int limit = 0;
    int offset = 0;
    int workers = 0;
    bool count = false;
    bool explain = false;
    qint64 seed = -1;
    int interval = 900;
    int size = 256;
    bool quick = false;
    bool dryRun = false;
    bool apply = false;
    bool keepFolders = false;
    bool anyLength = false;
    QList<QPair<QString, QString>> sets, setMasks;
    QString mask, to, pattern, by;
    // Phase 3: playlists, statistics, Auto-DJ and imports.
    QString parent, name, ids, top, since, source;
    QStringList maps;
    bool folder = false, relative = false, overwrite = false, recent = false, never = false;
    int at = 0, countN = 0, separation = -1, days = -1;
    QStringList rest;
};

QString formatDuration(qint64 ms) {
    const qint64 s = ms / 1000;
    if (s >= 3600)
        return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString json(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

int failWith(const Options &o, QTextStream &out, QTextStream &err, const QString &message) {
    err << "omamp: " << message << Qt::endl;
    // Agents read stdout; give them the error there too, as JSON.
    if (o.json)
        out << json({{"ok", false}, {"error", message}}) << Qt::flush;
    return 1;
}

int scan(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    Job job("scan");
    // Progress goes to a terminal only, so logs and pipes stay clean.
    const bool progress = !o.json && isatty(STDERR_FILENO) && err.string() == nullptr;
    if (progress) {
        QObject::connect(&job, &Job::progressChanged, [&err](const QString &phase, qint64 done, qint64 total) {
            if (total > 0)
                err << "\r" << phase << ": " << done << " of " << total << "   " << Qt::flush;
            else
                err << "\r" << phase << ": " << done << " files   " << Qt::flush;
        });
    }
    Scanner scanner(library, resolver, &job);
    if (o.workers > 0)
        scanner.setWorkers(o.workers);
    const Scanner::Mode mode = o.quick ? Scanner::Mode::ChangedFolders : Scanner::Mode::Full;
    const ScanResult r = o.rest.isEmpty() ? scanner.rescanAll(mode) : scanner.scan(o.rest, mode);
    if (progress)
        err << "\r" << QString(60, ' ') << "\r" << Qt::flush;
    if (!r.ok)
        return failWith(o, out, err, r.error);
    if (o.json) {
        out << json(r.toJson()) << Qt::flush;
    } else {
        out << QString("Scanned %1 files in %2 s: %3 added, %4 updated, %5 unchanged, %6 newly missing")
                   .arg(r.files)
                   .arg(r.elapsedMs / 1000.0, 0, 'f', 1)
                   .arg(r.added)
                   .arg(r.updated)
                   .arg(r.unchanged)
                   .arg(r.missing)
            << Qt::endl;
        if (r.moved)
            out << QString("%1 moved files kept their tracks.").arg(r.moved) << Qt::endl;
        if (r.foldersSkipped)
            out << QString("%1 unchanged folders were not listed.").arg(r.foldersSkipped) << Qt::endl;
        if (r.failed)
            out << (r.failed == 1 ? QString("1 file could not be read; it was added by its file name.")
                                  : QString("%1 files could not be read; they were added by their file names.")
                                        .arg(r.failed))
                << Qt::endl;
        for (const QString &dir : r.unreadable)
            err << "omamp: could not list " << dir << Qt::endl;
        if (r.skippedMounts)
            out << QString("Skipped %1 other filesystems mounted inside; scan them as folders of their own.")
                       .arg(r.skippedMounts)
                << Qt::endl;
    }
    return 0;
}

QString formatBytes(qint64 bytes) {
    if (bytes >= 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
    return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
}

int explain(const Options &o, const QString &text, const Query::Context &context, QTextStream &out,
            QTextStream &err) {
    const Query::Compiled c = Query::compile(text, context);
    if (!c.ok())
        return failWith(o, out, err, c.error);
    QJsonArray params;
    for (const QVariant &v : c.params)
        params << QJsonValue::fromVariant(v);
    if (o.json) {
        out << json({{"ok", true},
                     {"query", text},
                     {"parsed", Query::parse(text).toJson()},
                     {"where", c.where},
                     {"order_by", c.order()},
                     {"limit", c.limit.toString()},
                     {"params", params},
                     {"seed", qint64(context.seed)}})
            << Qt::flush;
        return 0;
    }
    out << "WHERE    " << c.where << '\n' << "ORDER BY " << c.order() << '\n';
    if (c.limit.kind != Query::Limit::None)
        out << "LIMIT    " << c.limit.toString() << '\n';
    out << "PARAMS   " << QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact)) << Qt::endl;
    return 0;
}

int query(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    const QString text = o.rest.join(' ');
    Query::Options options;
    options.limit = o.limit;
    options.offset = o.offset;
    options.countOnly = o.count;
    // A new shuffle each run unless --seed asks for an old one. The JSON
    // says which seed was used, so a run can be repeated.
    options.context.seed = o.seed >= 0 ? quint32(o.seed) : QRandomGenerator::global()->bounded(1u << 31);
    if (o.explain)
        return explain(o, text, options.context, out, err);
    library.refreshVolumes(resolver);
    const Query::Result r = Query::run(library, text, options);
    if (!r.ok())
        return failWith(o, out, err, r.error);
    if (o.count) {
        if (o.json)
            out << json({{"ok", true}, {"query", text}, {"total", r.total}, {"elapsed_ms", r.elapsedUs / 1000.0}})
                << Qt::flush;
        else
            out << r.total << Qt::endl;
        return 0;
    }
    if (o.json) {
        QJsonArray tracks;
        for (const Query::Track &t : r.tracks)
            tracks << t.toJson();
        out << json({{"ok", true},
                     {"query", text},
                     {"total", r.total},
                     {"count", r.tracks.size()},
                     {"duration_ms", r.durationMs},
                     {"seed", qint64(options.context.seed)},
                     {"elapsed_ms", r.elapsedUs / 1000.0},
                     {"tracks", tracks}})
            << Qt::flush;
        return 0;
    }
    qint64 bytes = 0;
    for (const Query::Track &t : r.tracks) {
        bytes += t.size;
        const QString title = t.title.isEmpty() ? QString("(untitled)") : t.title;
        const QString artist = t.artist.isEmpty() ? QString("(unknown artist)") : t.artist;
        out << artist << " - " << title;
        if (!t.album.isEmpty())
            out << "  [" << t.album << "]";
        out << "  " << formatDuration(t.durationMs) << "  "
            << (t.path.isEmpty() ? QString("(offline)") : t.path);
        if (t.availability == "missing")
            out << "  (missing)";
        out << '\n';
    }
    out << QString("%1 of %2 tracks, %3, %4")
               .arg(r.tracks.size())
               .arg(r.total)
               .arg(formatDuration(r.durationMs), formatBytes(bytes))
        << Qt::endl;
    return 0;
}

int stats(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out) {
    library.refreshVolumes(resolver);
    const LibraryStats s = library.stats();
    const QStringList roots = Scanner::roots(library);
    if (o.json) {
        out << json({{"ok", true},
                     {"database", library.path()},
                     {"schema_version", library.schemaVersion()},
                     {"sqlite_version", library.sqliteVersion()},
                     {"tracks", s.tracks},
                     {"online", s.online},
                     {"offline", s.offline},
                     {"missing", s.missing},
                     {"volumes", s.volumes},
                     {"volumes_online", s.volumesOnline},
                     {"folders", s.folders},
                     {"roots", QJsonArray::fromStringList(roots)},
                     {"artists", s.artists},
                     {"albums", s.albums},
                     {"genres", s.genres},
                     {"playlists", s.playlists},
                     {"duration_ms", s.totalDurationMs},
                     {"bytes", s.totalBytes}})
            << Qt::flush;
        return 0;
    }
    out << "Library   " << library.path() << '\n'
        << "Tracks    " << s.tracks << " (" << s.online << " online, " << s.offline << " offline, " << s.missing
        << " missing)\n"
        << "Albums    " << s.albums << '\n'
        << "Artists   " << s.artists << '\n'
        << "Genres    " << s.genres << '\n'
        << "Volumes   " << s.volumes << " (" << s.volumesOnline << " online)\n"
        << "Playing   " << formatDuration(s.totalDurationMs) << '\n'
        << "Size      " << QString::number(s.totalBytes / 1e9, 'f', 2) << " GB\n";
    for (const QString &root : roots)
        out << "Folder    " << (root.isEmpty() ? QString("(offline)") : root) << '\n';
    out << Qt::flush;
    return 0;
}

// Ctrl+C and SIGTERM end `omamp watch` through the event loop, so the
// watcher stops cleanly and the library closes.
int signalPipe[2] = {-1, -1};

void onSignal(int) {
    const char c = 1;
    [[maybe_unused]] auto n = ::write(signalPipe[1], &c, 1);
}

QString summary(const QJsonObject &r) {
    QStringList parts;
    for (const char *key : {"added", "updated", "moved", "missing", "failed"})
        if (r.value(key).toInt())
            parts << QString("%1 %2").arg(r.value(key).toInt()).arg(key);
    if (parts.isEmpty())
        parts << "nothing changed";
    return parts.join(", ");
}

int watch(const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    if (!QCoreApplication::instance())
        return failWith(o, out, err, "watch needs an event loop");
    Watcher watcher(o.db, resolver);
    watcher.setNetworkInterval(o.interval);
    watcher.setAutoRescan(!o.dryRun);
    if (o.workers > 0)
        watcher.setWorkers(o.workers);
    auto line = [&](const QJsonObject &event) {
        out << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)) << Qt::endl;
    };
    QObject::connect(&watcher, &Watcher::foldersChanged, [&](const QStringList &folders) {
        if (o.json)
            line({{"event", "changed"}, {"folders", QJsonArray::fromStringList(folders)}});
        else
            for (const QString &folder : folders)
                out << "changed  " << folder << Qt::endl;
    });
    QObject::connect(&watcher, &Watcher::rescanFinished, [&](const QJsonObject &r) {
        if (o.json) {
            QJsonObject e = r;
            e["event"] = "rescanned";
            line(e);
            return;
        }
        out << "rescan   " << r.value("kind").toString() << ": " << summary(r) << " ("
            << r.value("elapsed_ms").toInt() << " ms)" << Qt::endl;
        for (const QJsonValue &m : r.value("moved_paths").toArray())
            out << "moved    " << m.toString() << Qt::endl;
        if (!r.value("ok").toBool(true))
            err << "omamp: " << r.value("error").toString() << Qt::endl;
    });
    QObject::connect(&watcher, &Watcher::watchLimitReached, [&](int watched, int limit) {
        err << "omamp: the watch limit (" << limit << ") ran out after " << watched
            << " folders; the rest are rescanned every " << o.interval << " s" << Qt::endl;
    });
    // Shares that go away and come back: the monitor marks their tracks
    // offline and online, and the watcher quick-rescans a share that returns.
    MountMonitor mounts(o.db, resolver);
    watcher.followMounts(&mounts);
    QObject::connect(&mounts, &MountMonitor::volumesWentOffline, [&](const QList<qint64> &ids) {
        for (qint64 id : ids) {
            if (o.json)
                line({{"event", "offline"}, {"volume", id}});
            else
                out << "offline  volume " << id << Qt::endl;
        }
    });
    QObject::connect(&mounts, &MountMonitor::volumesCameOnline, [&](const QList<qint64> &ids, const QStringList &roots) {
        if (o.json)
            line({{"event", "online"}, {"volumes", QJsonArray::fromVariantList([&] {
                      QVariantList v;
                      for (qint64 id : ids)
                          v << id;
                      return v;
                  }())}, {"folders", QJsonArray::fromStringList(roots)}});
        else
            for (const QString &root : roots)
                out << "online   " << root << Qt::endl;
    });
    QString error;
    if (!watcher.start(&error) && !error.isEmpty())
        err << "omamp: " << error << Qt::endl;
    mounts.start();
    if (o.json) {
        line({{"event", "watching"},
              {"folders", watcher.watchedFolders()},
              {"local_roots", QJsonArray::fromStringList(watcher.localRoots())},
              {"scheduled_roots", QJsonArray::fromStringList(watcher.scheduledRoots())},
              {"degraded", watcher.isDegraded()}});
    } else {
        out << "Watching " << watcher.watchedFolders() << " folders in " << watcher.localRoots().size()
            << " local library folders." << Qt::endl;
        for (const QString &root : watcher.scheduledRoots())
            out << "Rescanning every " << o.interval << " s (not watched): " << root << Qt::endl;
        if (watcher.localRoots().isEmpty() && watcher.scheduledRoots().isEmpty())
            out << "The library has no folders online. Add one with: omamp scan <folder>" << Qt::endl;
        out << "Press Ctrl+C to stop." << Qt::endl;
    }

    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signalPipe) != 0)
        return failWith(o, out, err, "can't set up the signal handler");
    QSocketNotifier stopper(signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&stopper, &QSocketNotifier::activated, QCoreApplication::instance(), &QCoreApplication::quit);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QCoreApplication::exec();
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    watcher.stop();
    ::close(signalPipe[0]);
    ::close(signalPipe[1]);
    if (!o.json)
        out << "Stopped." << Qt::endl;
    return 0;
}

int locate(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    if (o.rest.size() != 1)
        return failWith(o, out, err, "locate needs one folder, for example: omamp locate /mnt/music");
    Scanner scanner(library, resolver);
    const LocateResult r = scanner.locate(o.rest.first());
    if (!r.ok)
        return failWith(o, out, err, r.error);
    if (o.json) {
        out << json(r.toJson()) << Qt::flush;
        return 0;
    }
    for (const QString &m : r.located)
        out << "found    " << m << '\n';
    out << QString("Looked at %1 files: found %2 of %3 missing tracks; %4 still missing.")
               .arg(r.searched)
               .arg(r.located.size())
               .arg(r.missingBefore)
               .arg(r.missingAfter)
        << Qt::endl;
    return 0;
}

int art(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    ArtworkCache cache(library);
    const QString what = o.rest.value(0);
    if (what == "all") {
        const int made = cache.prefetch(o.size);
        if (o.json)
            out << json({{"ok", true}, {"made", made}, {"cache", cache.cacheDir()}}) << Qt::flush;
        else
            out << "Pictures for " << made << " albums are in " << cache.cacheDir() << Qt::endl;
        return 0;
    }
    bool ok = false;
    const qint64 id = o.rest.value(1).toLongLong(&ok);
    if ((what != "album" && what != "track") || !ok)
        return failWith(o, out, err, "art needs album <id>, track <id> or all");
    const QString path =
        cache.artworkFor(what == "album" ? ArtworkCache::Kind::Album : ArtworkCache::Kind::Track, id, o.size);
    if (path.isEmpty())
        return failWith(o, out, err,
                        cache.lastError().isEmpty() ? QString("%1 %2 has no picture").arg(what).arg(id)
                                                    : cache.lastError());
    if (o.json)
        out << json({{"ok", true}, {"path", path}, {"size", ArtworkCache::snapSize(o.size)}}) << Qt::flush;
    else
        out << path << Qt::endl;
    return 0;
}

// Progress for a long job on a terminal only, so logs and pipes stay clean.
void showProgress(Job &job, const Options &o, QTextStream &err) {
    if (o.json || !isatty(STDERR_FILENO) || err.string() != nullptr)
        return;
    QObject::connect(&job, &Job::progressChanged, [&err](const QString &phase, qint64 done, qint64 total) {
        err << "\r" << phase << ": " << done << " of " << total << "   " << Qt::flush;
    });
}

void endProgress(const Options &o, QTextStream &err) {
    if (!o.json && isatty(STDERR_FILENO) && err.string() == nullptr)
        err << "\r" << QString(60, ' ') << "\r" << Qt::flush;
}

QString shown(const QString &field, const QString &value) {
    if (value.isEmpty())
        return "(empty)";
    if (field == "rating")
        return QString("%1 stars").arg(value.toInt() / 20.0);
    if (field == "artwork")
        return "(picture)";
    QString v = value;
    v.replace('\n', " / ");
    return v.size() > 60 ? v.left(57) + "..." : v;
}

void printTagResult(const TagBatchResult &r, const Options &o, QTextStream &out, QTextStream &err) {
    if (o.json) {
        out << json(r.toJson()) << Qt::flush;
        return;
    }
    for (const TagFileResult &f : r.files) {
        if (!f.ok) {
            err << "omamp: " << (f.path.isEmpty() ? QString("track %1").arg(f.trackId) : f.path) << ": " << f.error
                << Qt::endl;
            continue;
        }
        if (f.changes.isEmpty())
            continue;
        out << f.path << '\n';
        for (const TagFieldChange &c : f.changes)
            out << "  " << c.field << ": " << shown(c.field, c.oldValue) << " -> " << shown(c.field, c.newValue)
                << '\n';
    }
    if (r.dryRun)
        out << QString("Dry run: %1 tracks would change, %2 already match, %3 failed.")
                   .arg(r.changed)
                   .arg(r.unchanged)
                   .arg(r.failed)
            << Qt::endl;
    else
        out << QString("Changed %1 tracks, %2 already matched, %3 failed.").arg(r.changed).arg(r.unchanged).arg(r.failed)
            << Qt::endl;
    if (!r.batch.isEmpty())
        out << "Undo with: omamp undo " << r.batch << Qt::endl;
}

// A rating on the command line is in stars, as in the query language.
bool ratingFromStars(const QString &text, QString *value) {
    if (text.trimmed().isEmpty()) {
        value->clear();
        return true;
    }
    bool ok = false;
    const double stars = text.trimmed().toDouble(&ok);
    if (!ok || stars < 0 || stars > 5)
        return false;
    *value = QString::number(qRound(stars * 20));
    return true;
}

int tag(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    const QString text = o.rest.join(' ');
    if (text.trimmed().isEmpty())
        return failWith(o, out, err, "tag needs a query, for example: omamp tag 'album:\"night drive\"' --set year=1997");
    if (o.sets.isEmpty() && o.setMasks.isEmpty())
        return failWith(o, out, err, "tag needs at least one --set field=value");
    library.refreshVolumes(resolver);
    QString error;
    const QList<qint64> ids = Query::ids(library, text, {}, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);

    QList<TagEdit> fixed;
    for (const auto &set : o.sets) {
        const QString field = TagWriter::canonicalField(set.first);
        if (field.isEmpty())
            return failWith(o, out, err,
                            QString("Unknown field \"%1\". Fields: %2").arg(set.first, TagWriter::fields().join(", ")));
        TagEdit e{0, field, set.second, {}, {}};
        if (field == "rating" && !ratingFromStars(set.second, &e.value))
            return failWith(o, out, err, "rating takes 0 to 5 stars, or nothing to clear it");
        if (field == "artwork" && !set.second.isEmpty()) {
            QFile picture(set.second);
            if (!picture.open(QIODevice::ReadOnly))
                return failWith(o, out, err, "Can't read the picture " + set.second);
            e.picture = picture.readAll();
            e.value.clear();
        }
        fixed << e;
    }
    QList<QPair<QString, Mask::Parsed>> masks;
    for (const auto &set : o.setMasks) {
        const QString field = TagWriter::canonicalField(set.first);
        if (field.isEmpty() || field == "artwork")
            return failWith(o, out, err, QString("--set-mask can't set \"%1\"").arg(set.first));
        const Mask::Parsed parsed = Mask::parse(set.second);
        if (!parsed.ok())
            return failWith(o, out, err,
                            QString("%1 (at character %2 of the mask)").arg(parsed.error).arg(parsed.position + 1));
        masks.append({field, parsed});
    }

    QList<TagEdit> edits;
    Mask::Context textMode;
    textMode.pathMode = false;
    for (int k = 0; k < ids.size(); ++k) {
        for (TagEdit e : std::as_const(fixed)) {
            e.trackId = ids[k];
            edits << e;
        }
        if (!masks.isEmpty()) {
            const Mask::Values values = Mask::fromTrack(library, ids[k]);
            textMode.autoNumber = k + 1;
            for (const auto &m : std::as_const(masks)) {
                QString value = Mask::evaluate(m.second, values, textMode);
                if (m.first == "rating" && !ratingFromStars(value, &value))
                    continue;
                edits << TagEdit{ids[k], m.first, value, {}, {}};
            }
        }
    }
    Job job("tag");
    showProgress(job, o, err);
    TagWriteOptions options;
    options.dryRun = o.dryRun;
    const TagBatchResult r = TagWriter(library, &job).apply(edits, options);
    endProgress(o, err);
    if (!r.error.isEmpty() && r.files.isEmpty())
        return failWith(o, out, err, r.error);
    printTagResult(r, o, out, err);
    return r.ok ? 0 : 1;
}

int tagFromFilename(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out,
                    QTextStream &err) {
    const QString text = o.rest.join(' ');
    if (text.trimmed().isEmpty() || o.pattern.isEmpty())
        return failWith(o, out, err,
                        "tag-from-filename needs a query and --pattern, for example: "
                        "omamp tag-from-filename 'title:none' --pattern '<Track#> - <Title>'");
    const Mask::FilenameMatch check = Mask::matchFilename(o.pattern, "/");
    if (check.position >= 0)
        return failWith(o, out, err,
                        QString("%1 (at character %2 of the pattern)").arg(check.error).arg(check.position + 1));
    library.refreshVolumes(resolver);
    QString error;
    const QList<qint64> ids = Query::ids(library, text, {}, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);
    QList<TagEdit> edits;
    QStringList unmatched;
    int matched = 0;
    for (qint64 id : ids) {
        const QString path = library.absolutePath(id);
        const Mask::FilenameMatch m = Mask::matchFilename(o.pattern, path);
        if (path.isEmpty() || !m.ok) {
            unmatched << (path.isEmpty() ? QString("track %1 (offline)").arg(id) : path);
            continue;
        }
        ++matched;
        for (auto it = m.values.constBegin(); it != m.values.constEnd(); ++it)
            edits << TagEdit{id, it.key(), it.value(), {}, {}};
    }
    Job job("tag");
    showProgress(job, o, err);
    TagWriteOptions options;
    options.dryRun = o.dryRun;
    options.source = "tag-from-filename";
    const TagBatchResult r = TagWriter(library, &job).apply(edits, options);
    endProgress(o, err);
    if (o.json) {
        QJsonObject j = r.toJson();
        j["matched"] = matched;
        j["unmatched"] = QJsonArray::fromStringList(unmatched);
        out << json(j) << Qt::flush;
        return r.ok ? 0 : 1;
    }
    printTagResult(r, o, out, err);
    if (!unmatched.isEmpty()) {
        out << unmatched.size() << " files don't match the pattern:" << '\n';
        for (const QString &u : std::as_const(unmatched))
            out << "  " << u << '\n';
        out << Qt::flush;
    }
    return r.ok ? 0 : 1;
}

int organise(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    const QString text = o.rest.join(' ');
    if (text.trimmed().isEmpty() || o.mask.isEmpty())
        return failWith(o, out, err,
                        "organise needs a query and --mask, for example: omamp organise 'path:inbox' "
                        "--mask '<Album Artist>/<Album>/<Track#:2> <Title>' --dry-run");
    if (o.dryRun == o.apply)
        return failWith(o, out, err, "organise needs --dry-run to preview or --apply to move the files");
    library.refreshVolumes(resolver);
    QString error;
    const QList<qint64> ids = Query::ids(library, text, {}, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);
    OrganiseOptions options;
    options.mask = o.mask;
    options.removeEmptyFolders = !o.keepFolders;
    if (!o.to.isEmpty())
        options.destination = QFileInfo(o.to).absoluteFilePath();
    Job job("organise");
    showProgress(job, o, err);
    Organiser organiser(library, &job);
    const OrganisePlan plan = organiser.plan(ids, options);
    if (!plan.ok)
        return failWith(o, out, err,
                        plan.errorPosition >= 0
                            ? QString("%1 (at character %2 of the mask)").arg(plan.error).arg(plan.errorPosition + 1)
                            : plan.error);
    if (o.dryRun) {
        if (o.json) {
            QJsonObject j = plan.toJson();
            j["dry_run"] = true;
            out << json(j) << Qt::flush;
            return 0;
        }
        for (const OrganiseItem &item : plan.items) {
            if (item.status == "unchanged")
                continue;
            out << item.oldPath << "\n  -> " << (item.newPath.isEmpty() ? QString("(not moved)") : item.newPath);
            if (item.status != "move")
                out << "  [" << item.status << (item.note.isEmpty() ? QString() : ": " + item.note) << "]";
            out << '\n';
            for (const CompanionMove &c : item.companions)
                out << "  with " << c.from << (c.skip ? QString(" (left: %1)").arg(c.note) : QString()) << '\n';
        }
        for (const CompanionMove &c : plan.folderCompanions)
            out << c.from << "\n  -> " << c.to << (c.skip ? QString(" (left: %1)").arg(c.note) : QString()) << '\n';
        out << QString("Dry run: %1 files would move (%2 renamed to avoid a clash), %3 already in place, %4 can't move.")
                   .arg(plan.moves)
                   .arg(plan.renamed)
                   .arg(plan.unchanged)
                   .arg(plan.problems)
            << Qt::endl;
        return 0;
    }
    const OrganiseResult r = organiser.apply(plan, options);
    endProgress(o, err);
    if (o.json) {
        out << json(r.toJson()) << Qt::flush;
        return r.ok ? 0 : 1;
    }
    for (const OrganiseItem &item : r.items)
        if (item.status == "failed")
            err << "omamp: " << item.oldPath << ": " << item.note << Qt::endl;
    out << QString("Moved %1 files (%2 copied across filesystems), %3 already in place, %4 failed; %5 extra files "
                   "moved, %6 empty folders removed.")
               .arg(r.moved)
               .arg(r.copied)
               .arg(r.unchanged)
               .arg(r.failed)
               .arg(r.companionsMoved)
               .arg(r.foldersRemoved)
        << Qt::endl;
    if (!r.batch.isEmpty())
        out << "Undo with: omamp undo " << r.batch << Qt::endl;
    return r.ok ? 0 : 1;
}

int importCommand(Library &library, const Options &o, QTextStream &out, QTextStream &err);

int undo(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    library.refreshVolumes(resolver);
    const QString batch = o.rest.isEmpty() ? Journal::lastUndoable(library) : o.rest.first();
    if (batch.isEmpty())
        return failWith(o, out, err, "There is nothing to undo");
    // A MediaMonkey import has its own journal; `omamp undo <batch>` takes it too.
    if (MmImport::isImportBatch(batch)) {
        Options importUndo = o;
        importUndo.rest = QStringList{"undo", batch};
        return importCommand(library, importUndo, out, err);
    }
    Job job("undo");
    showProgress(job, o, err);
    const QJsonObject r = undoBatch(library, batch, &job);
    endProgress(o, err);
    const bool ok = r.value("ok").toBool();
    if (o.json) {
        out << json(r) << Qt::flush;
        return ok ? 0 : 1;
    }
    if (!ok && r.value("files").toArray().isEmpty() && r.value("items").toArray().isEmpty())
        return failWith(o, out, err, r.value("error").toString());
    if (r.value("kind").toString() == "organise")
        out << QString("Moved %1 files back; %2 failed.").arg(r.value("moved").toInt()).arg(r.value("failed").toInt());
    else
        out << QString("Restored %1 tracks; %2 failed.").arg(r.value("changed").toInt()).arg(r.value("failed").toInt());
    out << Qt::endl;
    if (!r.value("batch").toString().isEmpty())
        out << "The undo is itself batch " << r.value("batch").toString() << "; undo that to redo." << Qt::endl;
    if (!ok && !r.value("error").toString().isEmpty())
        err << "omamp: " << r.value("error").toString() << Qt::endl;
    return ok ? 0 : 1;
}

int journal(Library &library, const Options &o, QTextStream &out, QTextStream &) {
    if (!o.rest.isEmpty()) {
        const QList<Journal::Entry> entries = Journal::entries(library, o.rest.first());
        if (o.json) {
            QJsonArray list;
            for (const Journal::Entry &e : entries)
                list << e.toJson();
            out << json({{"ok", true}, {"batch", o.rest.first()}, {"entries", list}}) << Qt::flush;
            return 0;
        }
        for (const Journal::Entry &e : entries) {
            const QJsonObject j = e.toJson();
            out << (e.trackId ? QString("track %1").arg(e.trackId) : QString("-")) << "  " << e.field << ": "
                << shown(e.field, j.value("old").toString()) << " -> " << shown(e.field, e.newValue)
                << (e.undone ? "  (undone)" : "") << '\n';
        }
        out << entries.size() << " changes" << Qt::endl;
        return 0;
    }
    const QList<Journal::Batch> batches = Journal::batches(library, o.limit > 0 ? o.limit : 20);
    if (o.json) {
        QJsonArray list;
        for (const Journal::Batch &b : batches)
            list << b.toJson();
        out << json({{"ok", true}, {"batches", list}, {"last_undoable", Journal::lastUndoable(library)}})
            << Qt::flush;
        return 0;
    }
    for (const Journal::Batch &b : batches)
        out << b.id << "  " << QDateTime::fromSecsSinceEpoch(b.changedAt).toString("yyyy-MM-dd HH:mm") << "  "
            << b.kind() << "  " << b.tracks << " tracks, " << b.entries << " changes"
            << (b.undone() ? "  (undone)" : "") << '\n';
    if (batches.isEmpty())
        out << "The journal is empty." << '\n';
    out << Qt::flush;
    return 0;
}

QList<Query::Track> tracksById(Library &library, const QList<qint64> &ids) {
    QList<Query::Track> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT t.id, v.root_path, v.online, t.rel_path, t.title, t.artist, t.album, t.availability "
              "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ?");
    for (qint64 id : ids) {
        q.addBindValue(id);
        if (!q.exec() || !q.next())
            continue;
        Query::Track t;
        t.id = id;
        if (q.value(2).toBool()) {
            const QString root = q.value(1).toString();
            t.path = (root == "/" ? QString() : root) + '/' + q.value(3).toString();
        }
        t.title = q.value(4).toString();
        t.artist = q.value(5).toString();
        t.album = q.value(6).toString();
        t.availability = q.value(7).toString();
        out << t;
        q.finish();
    }
    return out;
}

int audit(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    library.refreshVolumes(resolver);
    if (o.rest.isEmpty()) {
        const QList<Audits::Count> counts = Audits::counts(library);
        if (o.json) {
            QJsonArray list;
            for (const Audits::Count &c : counts) {
                QJsonObject j = c.audit.toJson();
                j["count"] = c.count;
                list << j;
            }
            out << json({{"ok", true}, {"audits", list}}) << Qt::flush;
            return 0;
        }
        for (const Audits::Count &c : counts)
            out << QString("%1  %2  (%3)").arg(c.count, 7).arg(c.audit.title, c.audit.id) << '\n';
        out << "List one with: omamp audit <check>" << Qt::endl;
        return 0;
    }
    const QString id = o.rest.first();
    if (!Audits::audit(id)) {
        QStringList known;
        for (const Audits::Audit &a : Audits::audits())
            known << a.id;
        return failWith(o, out, err, QString("No check called %1. Checks: %2").arg(id, known.join(", ")));
    }
    QString error;
    const qint64 total = Audits::count(library, id, &error);
    const QList<qint64> ids = Audits::ids(library, id, o.limit, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);
    const QList<Query::Track> tracks = tracksById(library, ids);
    if (o.json) {
        QJsonArray list;
        for (const Query::Track &t : tracks)
            list << QJsonObject{{"id", t.id},
                                {"path", t.path.isEmpty() ? QJsonValue() : QJsonValue(t.path)},
                                {"title", t.title},
                                {"artist", t.artist},
                                {"album", t.album},
                                {"availability", t.availability}};
        out << json({{"ok", true}, {"audit", Audits::audit(id)->toJson()}, {"total", total}, {"tracks", list}})
            << Qt::flush;
        return 0;
    }
    for (const Query::Track &t : tracks)
        out << (t.artist.isEmpty() ? QString("(unknown artist)") : t.artist) << " - "
            << (t.title.isEmpty() ? QString("(untitled)") : t.title) << "  "
            << (t.path.isEmpty() ? QString("(offline)") : t.path) << '\n';
    out << QString("%1 of %2 tracks: %3").arg(tracks.size()).arg(total).arg(Audits::audit(id)->title) << Qt::endl;
    return 0;
}

int duplicates(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out,
               QTextStream &err) {
    Duplicates::Options options;
    if (!o.by.isEmpty() && !Duplicates::parse(o.by, &options.by))
        return failWith(o, out, err, "--by takes tags, content or fingerprint");
    options.matchLength = !o.anyLength;
    library.refreshVolumes(resolver);
    if (options.by == Duplicates::By::Fingerprint) {
        Job job("fingerprint");
        showProgress(job, o, err);
        const Fingerprinter::Result f = Fingerprinter(library, &job).run();
        endProgress(o, err);
        if (!f.ok)
            return failWith(o, out, err, f.error);
        if (!o.json && f.total)
            err << "omamp: fingerprinted " << f.computed << " files (" << f.failed << " could not be read)" << Qt::endl;
    }
    const Duplicates::Report r = Duplicates::find(library, options);
    if (!r.ok)
        return failWith(o, out, err, r.error);
    if (o.json) {
        out << json(r.toJson()) << Qt::flush;
        return 0;
    }
    for (const Duplicates::Group &g : r.groups) {
        out << g.tracks.first().artist << " - " << g.tracks.first().title << "  (keep: " << g.reason << ")\n";
        for (const Duplicates::Track &t : g.tracks)
            out << (t.keeper ? "  keep  " : "        ") << (t.path.isEmpty() ? QString("(offline)") : t.path) << "  "
                << t.format << ' ' << t.bitrate << " kbps, " << t.plays << " plays\n";
    }
    out << QString("%1 groups, %2 tracks. Nothing was deleted.").arg(r.groups.size()).arg(r.tracks) << Qt::endl;
    return 0;
}

// ---- Phase 3: playlists, collections, statistics, Auto-DJ, imports --------

QString kindLabel(const Playlists::Playlist &p) {
    if (p.kind == "auto")
        return "auto: " + p.query;
    if (p.kind == "folder")
        return "folder";
    return p.items == 1 ? QString("1 track") : QString("%1 tracks").arg(p.items);
}

int depthOf(const QList<Playlists::Playlist> &all, const Playlists::Playlist &p) {
    int depth = 0;
    for (qint64 up = p.parentId; up && depth < 64; ++depth) {
        qint64 next = 0;
        for (const Playlists::Playlist &q : all)
            if (q.id == up)
                next = q.parentId;
        up = next;
    }
    return depth;
}

void printItems(const QList<Playlists::Item> &items, QTextStream &out) {
    for (const Playlists::Item &it : items) {
        out << QString("%1").arg(it.position + 1, 4) << "  "
            << (it.artist.isEmpty() ? QString("(unknown artist)") : it.artist) << " - "
            << (it.title.isEmpty() ? QString("(untitled)") : it.title) << "  " << formatDuration(it.durationMs) << "  "
            << (it.path.isEmpty() ? QString("(no path)") : it.path);
        if (it.availability != "online")
            out << "  (" << it.availability << ")";
        out << '\n';
    }
}

QList<qint64> parseIds(const QString &text, bool *ok) {
    QList<qint64> out;
    *ok = true;
    for (const QString &part : text.split(',', Qt::SkipEmptyParts)) {
        bool good = false;
        const qint64 n = part.trimmed().toLongLong(&good);
        if (!good) {
            *ok = false;
            return {};
        }
        out << n;
    }
    return out;
}

qint64 parentFor(Library &library, const Options &o, QString *error) {
    if (o.parent.isEmpty() || o.parent == "/")
        return 0;
    return Playlists::resolve(library, o.parent, error);
}

int playlist(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    const QString sub = o.rest.value(0);
    const QStringList args = o.rest.mid(1);
    QString error;
    auto done = [&](const QJsonObject &j, const QString &text) {
        if (o.json)
            out << json(j) << Qt::flush;
        else
            out << text << Qt::endl;
        return 0;
    };
    const bool badParent = !o.parent.isEmpty() && o.parent != "/" && !parentFor(library, o, &error);
    if (sub == "list" || sub.isEmpty()) {
        const QList<Playlists::Playlist> all = Playlists::list(library);
        if (o.json) {
            QJsonArray list;
            for (const Playlists::Playlist &p : all)
                list << p.toJson();
            out << json({{"ok", true}, {"playlists", list}}) << Qt::flush;
            return 0;
        }
        for (const Playlists::Playlist &p : all)
            out << QString(depthOf(all, p) * 2, ' ') << QString("%1  %2  (%3)").arg(p.id, 4).arg(p.name, kindLabel(p))
                << '\n';
        if (all.isEmpty())
            out << "No playlists yet. Make one with: omamp playlist create <name>" << '\n';
        out << Qt::flush;
        return 0;
    }
    if (badParent)
        return failWith(o, out, err, error);
    const qint64 parent = parentFor(library, o, &error);
    if (sub == "create") {
        if (args.isEmpty())
            return failWith(o, out, err, "playlist create needs a name");
        const qint64 id = o.folder ? Playlists::createFolder(library, args.join(' '), parent, &error)
                                   : Playlists::create(library, args.join(' '), parent, &error);
        if (!id)
            return failWith(o, out, err, error);
        Playlists::Playlist p;
        Playlists::get(library, id, &p);
        return done({{"ok", true}, {"playlist", p.toJson()}},
                    QString("Made %1 %2 (%3)").arg(o.folder ? "folder" : "playlist", p.name).arg(id));
    }
    if (sub == "import") {
        if (args.isEmpty())
            return failWith(o, out, err, "playlist import needs a playlist file or a folder of them");
        QList<PlaylistFiles::ImportResult> results;
        qint64 folderId = 0;
        if (QFileInfo(args.first()).isDir()) {
            results = PlaylistFiles::importFolder(library, args.first(), parent, o.name, &folderId, &error);
            if (!folderId)
                return failWith(o, out, err, error.isEmpty() ? QString("Nothing imported") : error);
        } else {
            results << PlaylistFiles::import(library, args.first(), parent, o.name);
        }
        bool ok = true;
        QJsonArray list;
        for (const PlaylistFiles::ImportResult &r : results) {
            ok = ok && r.ok();
            list << r.toJson();
        }
        if (o.json) {
            out << json({{"ok", ok}, {"folder_id", folderId ? QJsonValue(folderId) : QJsonValue()}, {"imported", list}})
                << Qt::flush;
            return ok ? 0 : 1;
        }
        for (const PlaylistFiles::ImportResult &r : results) {
            if (!r.ok()) {
                err << "omamp: " << r.error << Qt::endl;
                continue;
            }
            out << QString("%1 (%2): %3 entries, %4 matched by path, %5 by artist and title, %6 not in the library")
                       .arg(r.name)
                       .arg(r.playlistId)
                       .arg(r.entries)
                       .arg(r.byPath)
                       .arg(r.byTags)
                       .arg(r.unmatched)
                << '\n';
        }
        out << Qt::flush;
        return ok ? 0 : 1;
    }
    // Everything else names a playlist first.
    if (args.isEmpty())
        return failWith(o, out, err, QString("playlist %1 needs a playlist (its id, name or Folder/Name)").arg(sub));
    const qint64 id = Playlists::resolve(library, args.first(), &error);
    if (!id)
        return failWith(o, out, err, error);
    Playlists::Playlist p;
    Playlists::get(library, id, &p);
    if (sub == "show") {
        const QList<Playlists::Item> items = Playlists::items(library, id, &error);
        if (!error.isEmpty())
            return failWith(o, out, err, error);
        if (o.json) {
            QJsonArray list;
            for (const Playlists::Item &it : items)
                list << it.toJson();
            out << json({{"ok", true}, {"playlist", p.toJson()}, {"items", list}}) << Qt::flush;
            return 0;
        }
        printItems(items, out);
        out << QString("%1: %2 entries").arg(p.name).arg(items.size()) << Qt::endl;
        return 0;
    }
    if (sub == "add") {
        QList<qint64> ids;
        if (!o.ids.isEmpty()) {
            bool ok = false;
            ids = parseIds(o.ids, &ok);
            if (!ok)
                return failWith(o, out, err, "--ids takes track ids separated by commas");
        } else {
            const QString text = args.mid(1).join(' ');
            if (text.isEmpty())
                return failWith(o, out, err, "playlist add needs a query or --ids");
            Query::Context context;
            context.seed = o.seed >= 0 ? quint32(o.seed) : QRandomGenerator::global()->bounded(1u << 31);
            ids = Query::ids(library, text, context, &error);
            if (!error.isEmpty())
                return failWith(o, out, err, error);
        }
        const int added = Playlists::addTracks(library, id, ids, o.at > 0 ? o.at - 1 : -1, &error);
        if (added < 0)
            return failWith(o, out, err, error);
        return done({{"ok", true}, {"playlist_id", id}, {"added", added}},
                    QString("Added %1 tracks to %2").arg(added).arg(p.name));
    }
    if (sub == "remove") {
        int removed = 0;
        if (!o.ids.isEmpty()) {
            bool ok = false;
            const QList<qint64> items = parseIds(o.ids, &ok);
            if (!ok)
                return failWith(o, out, err, "--ids takes entry ids separated by commas");
            removed = Playlists::removeItems(library, id, items, &error);
        } else {
            QList<int> positions;
            for (const QString &a : args.mid(1)) {
                bool ok = false;
                const int n = a.toInt(&ok);
                if (!ok || n < 1)
                    return failWith(o, out, err, "playlist remove takes entry numbers, counting from 1");
                positions << n - 1;
            }
            if (positions.isEmpty())
                return failWith(o, out, err, "playlist remove needs entry numbers or --ids");
            removed = Playlists::removePositions(library, id, positions, &error);
        }
        if (removed < 0)
            return failWith(o, out, err, error);
        return done({{"ok", true}, {"playlist_id", id}, {"removed", removed}},
                    QString("Removed %1 entries from %2").arg(removed).arg(p.name));
    }
    if (sub == "move") {
        if (!o.parent.isEmpty() || o.at > 0) {
            // Moves the playlist itself: under --parent ("/" for the top), to place --at.
            const qint64 newParent = o.parent.isEmpty() ? p.parentId : parent;
            if (!Playlists::move(library, id, newParent, o.at > 0 ? o.at - 1 : -1, &error))
                return failWith(o, out, err, error);
            return done({{"ok", true}, {"playlist_id", id}, {"parent_id", newParent}},
                        QString("Moved %1").arg(p.name));
        }
        bool okFrom = false, okTo = false;
        const int from = args.value(1).toInt(&okFrom), to = args.value(2).toInt(&okTo);
        QList<qint64> order;
        for (const Playlists::Item &it : Playlists::items(library, id))
            order << it.id;
        if (!okFrom || !okTo || from < 1 || to < 1 || from > order.size())
            return failWith(o, out, err, "playlist move <playlist> <from> <to> takes entry numbers, counting from 1");
        if (!Playlists::moveItems(library, id, {order[from - 1]}, to - 1, &error))
            return failWith(o, out, err, error);
        return done({{"ok", true}, {"playlist_id", id}, {"from", from}, {"to", to}},
                    QString("Moved entry %1 to %2").arg(from).arg(to));
    }
    if (sub == "rename") {
        if (args.size() < 2 || !Playlists::rename(library, id, args.mid(1).join(' '), &error))
            return failWith(o, out, err, error.isEmpty() ? QString("playlist rename needs a new name") : error);
        return done({{"ok", true}, {"playlist_id", id}, {"name", args.mid(1).join(' ')}}, "Renamed");
    }
    if (sub == "delete") {
        if (!Playlists::remove(library, id, &error))
            return failWith(o, out, err, error);
        return done({{"ok", true}, {"deleted", id}}, QString("Deleted %1").arg(p.name));
    }
    if (sub == "clear") {
        if (!Playlists::clear(library, id, &error))
            return failWith(o, out, err, error);
        return done({{"ok", true}, {"playlist_id", id}}, QString("Emptied %1").arg(p.name));
    }
    if (sub == "reroll") {
        const qint64 seed = Playlists::reroll(library, id, o.seed);
        if (seed < 0)
            return failWith(o, out, err, QString("%1 is not an auto-playlist").arg(p.name));
        return done({{"ok", true}, {"playlist_id", id}, {"seed", seed}}, QString("New shuffle %1").arg(seed));
    }
    if (sub == "export") {
        if (args.size() < 2)
            return failWith(o, out, err, "playlist export needs a playlist and a file (.m3u8, .m3u or .pls)");
        PlaylistFiles::ExportOptions options;
        options.relative = o.relative;
        int written = 0;
        if (!PlaylistFiles::exportFile(library, id, args[1], options, &written, &error))
            return failWith(o, out, err, error);
        const QString file = QFileInfo(args[1]).absoluteFilePath();
        return done({{"ok", true}, {"playlist_id", id}, {"file", file}, {"entries", written}},
                    QString("Wrote %1 entries to %2").arg(written).arg(file));
    }
    return failWith(o, out, err,
                    QString("Unknown playlist command %1. Commands: list, show, create, add, remove, move, rename, "
                            "delete, clear, reroll, import, export")
                        .arg(sub));
}

int autoplaylist(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    const QString sub = o.rest.value(0);
    QString error;
    if (sub == "create") {
        if (o.rest.size() < 3)
            return failWith(o, out, err, "autoplaylist create <name> '<query>'");
        const qint64 parent = parentFor(library, o, &error);
        if (!o.parent.isEmpty() && o.parent != "/" && !parent)
            return failWith(o, out, err, error);
        const qint64 id = Playlists::createAuto(library, o.rest[1], o.rest.mid(2).join(' '), parent, &error);
        if (!id)
            return failWith(o, out, err, error);
        Playlists::Playlist p;
        Playlists::get(library, id, &p);
        const qsizetype count = Playlists::trackIds(library, id).size();
        if (o.json)
            out << json({{"ok", true}, {"playlist", p.toJson()}, {"tracks", count}}) << Qt::flush;
        else
            out << QString("Made auto-playlist %1 (%2): %3 tracks now").arg(p.name).arg(id).arg(count) << Qt::endl;
        return 0;
    }
    if (sub == "set") {
        if (o.rest.size() < 3)
            return failWith(o, out, err, "autoplaylist set <playlist> '<query>'");
        const qint64 id = Playlists::resolve(library, o.rest[1], &error);
        if (!id || !Playlists::setQuery(library, id, o.rest.mid(2).join(' '), &error))
            return failWith(o, out, err, error);
        if (o.json)
            out << json({{"ok", true}, {"playlist_id", id}, {"query", o.rest.mid(2).join(' ')}}) << Qt::flush;
        else
            out << "Saved" << Qt::endl;
        return 0;
    }
    return failWith(o, out, err, "autoplaylist create <name> '<query>' or autoplaylist set <playlist> '<query>'");
}

int collection(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    const QString sub = o.rest.value(0, "list");
    QString error;
    if (sub == "list") {
        const QList<Collections::Collection> all = Collections::list(library);
        if (o.json) {
            QJsonArray list;
            for (const Collections::Collection &c : all) {
                QJsonObject j = c.toJson();
                j["tracks"] = Collections::trackIds(library, c.id).size();
                list << j;
            }
            out << json({{"ok", true}, {"collections", list}}) << Qt::flush;
            return 0;
        }
        for (const Collections::Collection &c : all)
            out << QString("%1  %2  (%3; %4 tracks)")
                       .arg(c.id, 4)
                       .arg(c.name, c.query.isEmpty() ? QString("everything") : c.query)
                       .arg(Collections::trackIds(library, c.id).size())
                << '\n';
        out << Qt::flush;
        return 0;
    }
    if (sub == "create") {
        if (o.rest.size() < 2)
            return failWith(o, out, err, "collection create <name> '<query>'");
        Collections::Collection c;
        c.name = o.rest[1];
        c.query = o.rest.mid(2).join(' ');
        const qint64 id = Collections::create(library, c, &error);
        if (!id)
            return failWith(o, out, err, error);
        Collections::get(library, id, &c);
        if (o.json)
            out << json({{"ok", true}, {"collection", c.toJson()}}) << Qt::flush;
        else
            out << QString("Made collection %1 (%2)").arg(c.name).arg(id) << Qt::endl;
        return 0;
    }
    if (sub == "restore-defaults") {
        const int added = Collections::ensureDefaults(library);
        if (o.json)
            out << json({{"ok", true}, {"added", added}}) << Qt::flush;
        else
            out << QString("Put back %1 collections").arg(added) << Qt::endl;
        return 0;
    }
    if (o.rest.size() < 2)
        return failWith(o, out, err, "collection list|show|create|remove|restore-defaults");
    const qint64 id = Collections::resolve(library, o.rest[1], &error);
    if (!id)
        return failWith(o, out, err, error);
    if (sub == "show") {
        Collections::Collection c;
        Collections::get(library, id, &c);
        const QList<qint64> ids = Collections::trackIds(library, id, {}, &error);
        if (o.json)
            out << json({{"ok", true}, {"collection", c.toJson()}, {"tracks", ids.size()}}) << Qt::flush;
        else
            out << c.name << ": " << (c.query.isEmpty() ? QString("everything") : c.query) << ", " << ids.size()
                << " tracks" << Qt::endl;
        return 0;
    }
    if (sub == "remove") {
        if (!Collections::remove(library, id, &error))
            return failWith(o, out, err, error);
        if (o.json)
            out << json({{"ok", true}, {"deleted", id}}) << Qt::flush;
        else
            out << "Deleted" << Qt::endl;
        return 0;
    }
    return failWith(o, out, err, "collection list|show|create|remove|restore-defaults");
}

int topStats(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    qint64 since = 0;
    if (!o.since.isEmpty()) {
        since = Stats::parseSince(o.since);
        if (since < 0)
            return failWith(o, out, err, "--since takes an age (30d, 2w, 6m, 1y) or a date (2026-01-01)");
    }
    const int limit = o.limit > 0 ? o.limit : 25;
    if (o.recent) {
        const QList<Stats::Played> plays = Stats::recent(library, limit);
        if (o.json) {
            QJsonArray list;
            for (const Stats::Played &p : plays)
                list << p.toJson();
            out << json({{"ok", true}, {"recent", list}}) << Qt::flush;
            return 0;
        }
        for (const Stats::Played &p : plays)
            out << QDateTime::fromSecsSinceEpoch(p.playedAt).toString("yyyy-MM-dd HH:mm") << "  " << p.artist << " - "
                << p.title << '\n';
        out << Qt::flush;
        return 0;
    }
    if (o.never) {
        const QList<Query::Track> tracks = tracksById(library, Stats::neverPlayed(library, limit));
        if (o.json) {
            QJsonArray list;
            for (const Query::Track &t : tracks)
                list << QJsonObject{{"id", t.id}, {"title", t.title}, {"artist", t.artist}, {"album", t.album}};
            out << json({{"ok", true}, {"never_played", list}, {"total", Stats::summary(library).neverPlayed}})
                << Qt::flush;
            return 0;
        }
        for (const Query::Track &t : tracks)
            out << t.artist << " - " << t.title << '\n';
        out << Qt::flush;
        return 0;
    }
    Stats::TopOptions options;
    if (!Stats::parseBy(o.top, &options.by))
        return failWith(o, out, err, "--top takes tracks, artists, albums, years or genres");
    options.since = since;
    options.limit = limit;
    QString error;
    const QList<Stats::Row> rows = Stats::top(library, options, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);
    if (o.json) {
        QJsonArray list;
        for (const Stats::Row &r : rows)
            list << r.toJson();
        out << json({{"ok", true},
                     {"top", Stats::byName(options.by)},
                     {"since", since ? QJsonValue(since) : QJsonValue()},
                     {"summary", Stats::summary(library, since).toJson()},
                     {"rows", list}})
            << Qt::flush;
        return 0;
    }
    int rank = 0;
    for (const Stats::Row &r : rows)
        out << QString("%1. %2%3  %4 plays")
                   .arg(++rank, 3)
                   .arg(r.name, r.detail.isEmpty() ? QString() : " - " + r.detail)
                   .arg(r.plays)
            << '\n';
    if (rows.isEmpty())
        out << "Nothing played" << (since ? " in that time" : "") << " yet." << '\n';
    out << Qt::flush;
    return 0;
}

int autodj(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    AutoDj::Settings settings = AutoDj::Settings::load(library);
    if (!o.source.isEmpty())
        settings.source = AutoDj::Source::parse(o.source);
    if (o.separation >= 0)
        settings.artistSeparation = o.separation;
    if (o.days >= 0)
        settings.skipPlayedWithinDays = o.days;
    const int count = o.countN > 0 ? o.countN : settings.upcoming;
    AutoDj::Dj dj(library, settings, o.seed > 0 ? quint32(o.seed) : 0);
    QList<qint64> history;
    if (!o.ids.isEmpty()) {
        bool ok = false;
        history = parseIds(o.ids, &ok);
        if (!ok)
            return failWith(o, out, err, "--ids takes the track ids already played or queued, oldest first");
    }
    const QList<AutoDj::Pick> picks = dj.pick(count, history);
    if (!dj.error().isEmpty())
        return failWith(o, out, err, dj.error());
    if (o.json) {
        QJsonArray list;
        for (const AutoDj::Pick &p : picks)
            list << p.toJson();
        out << json({{"ok", true},
                     {"source", settings.source.toString()},
                     {"seed", qint64(dj.seed())},
                     {"candidates", dj.candidates()},
                     {"settings", settings.toJson()},
                     {"tracks", list}})
            << Qt::flush;
        return 0;
    }
    for (const AutoDj::Pick &p : picks)
        out << p.trackId << "  " << p.artist << " - " << p.title
            << (p.relaxed.isEmpty() ? QString() : "  (relaxed: " + p.relaxed.join(", ") + ")") << '\n';
    out << QString("%1 tracks from %2 (seed %3)").arg(picks.size()).arg(settings.source.toString()).arg(dj.seed())
        << Qt::endl;
    return 0;
}

int importCommand(Library &library, const Options &o, QTextStream &out, QTextStream &err) {
    const QString what = o.rest.value(0);
    if (what == "undo") {
        const QString batch = o.rest.size() > 1 ? o.rest[1] : MmImport::lastBatch(library);
        if (batch.isEmpty())
            return failWith(o, out, err, "There is no import to undo");
        const MmImport::UndoResult r = MmImport::undo(library, batch);
        if (!r.ok)
            return failWith(o, out, err, r.error);
        if (o.json)
            out << json(r.toJson()) << Qt::flush;
        else
            out << QString("Undid import %1: %2 fields on %3 tracks, %4 playlists, %5 history rows")
                       .arg(batch)
                       .arg(r.fields)
                       .arg(r.tracks)
                       .arg(r.playlists)
                       .arg(r.historyRows)
                << Qt::endl;
        return 0;
    }
    if (what != "mediamonkey" && what != "mm")
        return failWith(o, out, err,
                        "import mediamonkey <MM.DB or MM5.DB> --map 'D:\\Music=/mnt/music' [--dry-run | --apply], "
                        "or import undo [batch]");
    if (o.rest.size() < 2)
        return failWith(o, out, err, "import mediamonkey needs the path of MM.DB or MM5.DB");
    if (o.apply && o.dryRun)
        return failWith(o, out, err, "Choose --dry-run or --apply, not both");
    MmImport::Options options;
    for (const QString &m : o.maps) {
        MmImport::PathMap map;
        if (!MmImport::PathMap::parse(m, &map))
            return failWith(o, out, err,
                            QString("--map takes 'D:\\Music=/mnt/music' (a Windows prefix, =, a Linux folder), not %1")
                                .arg(m));
        options.maps << map;
    }
    options.apply = o.apply;
    options.overwrite = o.overwrite;
    Job job("mmimport");
    showProgress(job, o, err);
    const MmImport::Report r = MmImport::run(library, o.rest[1], options, &job);
    endProgress(o, err);
    if (!r.ok)
        return failWith(o, out, err, r.error);
    if (o.json) {
        out << json(r.toJson()) << Qt::flush;
        return 0;
    }
    out << QString("%1 (%2): %3 tracks").arg(r.database, r.schema).arg(r.mmTracks) << '\n'
        << QString("Matched %1 by path, %2 by folder and file name, %3 by artist, title and length; %4 not found")
               .arg(r.matchedByPath)
               .arg(r.matchedBySuffix)
               .arg(r.matchedByTags)
               .arg(r.unmatched)
        << '\n'
        << QString("%1 %2 changes on %3 tracks")
               .arg(r.applied ? "Made" : "Would make")
               .arg(r.fieldChanges)
               .arg(r.tracksChanged)
        << '\n';
    for (auto it = r.changesByField.constBegin(); it != r.changesByField.constEnd(); ++it)
        out << "  " << it.key() << ": " << it.value() << '\n';
    out << QString("%1 play history rows (%2 already there)").arg(r.historyRows).arg(r.historyDuplicates) << '\n';
    for (const MmImport::PlaylistReport &p : r.playlists) {
        out << "Playlist " << p.name << ": " << p.kind;
        if (p.kind == "auto")
            out << " (" << p.query << ")";
        else if (p.kind == "snapshot")
            out << " (" << p.reason << ")";
        else if (p.kind == "static")
            out << QString(" (%1 of %2 entries matched)").arg(p.matched).arg(p.entries);
        out << '\n';
    }
    for (const QString &m : r.suggestedMaps)
        out << "Suggested --map " << m << '\n';
    for (const QString &c : r.missingColumns)
        out << "Not in this database: " << c << '\n';
    for (const QString &n : r.notes)
        out << n << '\n';
    if (r.applied)
        out << "Imported as batch " << r.batch << "; undo it with: omamp import undo " << r.batch << '\n';
    else
        out << "Nothing was changed. Run again with --apply to import." << '\n';
    out << Qt::flush;
    return 0;
}

} // namespace

bool isCommand(const QString &first) { return commands.contains(first) || WebCli::isCommand(first); }

int run(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver) {
    Options o;
    const QString command = arguments.value(0);
    // The web commands (Phase 4) parse their own options.
    if (WebCli::isCommand(command))
        return WebCli::run(arguments, out, err, resolver);
    if (command == "volume" || command == "nas" || command == "remote")
        return runNas(arguments, out, err, resolver);
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        auto value = [&](QString *target) {
            if (i + 1 >= arguments.size())
                return false;
            *target = arguments[++i];
            return true;
        };
        QString v;
        if (a == "--json")
            o.json = true;
        else if (a == "--count" && command != "autodj")
            o.count = true;
        else if (a == "--explain")
            o.explain = true;
        else if (a == "--db") {
            if (!value(&o.db))
                return failWith(o, out, err, "--db needs a path");
        } else if (a == "--limit" || a == "--workers" || a == "--offset" || a == "--seed" || a == "--interval" || a == "--size") {
            bool ok = false;
            const qint64 n = value(&v) ? v.toLongLong(&ok) : 0;
            if (!ok || n < 0 || n > 2147483647LL)
                return failWith(o, out, err, a + " needs a number");
            if (a == "--limit")
                o.limit = int(n);
            else if (a == "--workers")
                o.workers = int(n);
            else if (a == "--offset")
                o.offset = int(n);
            else if (a == "--interval")
                o.interval = int(n);
            else if (a == "--size")
                o.size = int(n);
            else
                o.seed = n;
        } else if (a == "--quick") {
            o.quick = true;
        } else if (a == "--dry-run") {
            o.dryRun = true;
        } else if (a == "--apply") {
            o.apply = true;
        } else if (a == "--keep-folders") {
            o.keepFolders = true;
        } else if (a == "--any-length") {
            o.anyLength = true;
        } else if (a == "--set" || a == "--set-mask") {
            if (!value(&v) || !v.contains('='))
                return failWith(o, out, err, a + " needs field=value");
            (a == "--set" ? o.sets : o.setMasks).append({v.section('=', 0, 0), v.section('=', 1)});
        } else if (a == "--mask" || a == "--to" || a == "--pattern" || a == "--by") {
            if (!value(&v))
                return failWith(o, out, err, a + " needs a value");
            if (a == "--mask")
                o.mask = v;
            else if (a == "--to")
                o.to = v;
            else if (a == "--pattern")
                o.pattern = v;
            else
                o.by = v;
        } else if (a == "--parent" || a == "--name" || a == "--ids" || a == "--top" || a == "--since" ||
                   a == "--source" || a == "--map") {
            if (!value(&v))
                return failWith(o, out, err, a + " needs a value");
            if (a == "--parent")
                o.parent = v;
            else if (a == "--name")
                o.name = v;
            else if (a == "--ids")
                o.ids = v;
            else if (a == "--top")
                o.top = v;
            else if (a == "--since")
                o.since = v;
            else if (a == "--source")
                o.source = v;
            else
                o.maps << v;
        } else if (a == "--at" || a == "--separation" || a == "--days" || (a == "--count" && command == "autodj")) {
            bool ok = false;
            const int n = value(&v) ? v.toInt(&ok) : 0;
            if (!ok || n < 0)
                return failWith(o, out, err, a + " needs a number");
            if (a == "--at")
                o.at = n;
            else if (a == "--separation")
                o.separation = n;
            else if (a == "--days")
                o.days = n;
            else
                o.countN = n;
        } else if (a == "--folder") {
            o.folder = true;
        } else if (a == "--relative") {
            o.relative = true;
        } else if (a == "--overwrite") {
            o.overwrite = true;
        } else if (a == "--recent") {
            o.recent = true;
        } else if (a == "--never") {
            o.never = true;
        } else if (a == "--") {
            o.rest += arguments.mid(i + 1);
            break;
        } else
            o.rest << a;
    }

    if (command == "help" || command == "--help" || command == "-h") {
        // The web commands' lines go after the others, before the options.
        QString text = QString::fromUtf8(usage);
        const int options = text.indexOf("\nOptions:");
        text.insert(options, QString::fromUtf8(WebCli::usage));
        out << text << Qt::flush;
        return 0;
    }
    if (command == "version" || command == "--version") {
        const QString version = QCoreApplication::applicationVersion();
        if (o.json)
            out << json({{"version", version}, {"schema_version", Library::latestVersion()}}) << Qt::flush;
        else
            out << "omamp " << version << Qt::endl;
        return 0;
    }
    if (command == "query" && o.rest.isEmpty())
        return failWith(o, out, err, "query needs some text, for example: omamp query neon coast");

    Library library(o.db);
    QString error;
    if (!library.open(&error))
        return failWith(o, out, err, error);
    if (!library.lastBackup().isEmpty() && !o.json)
        err << "omamp: upgraded the library; the old one is saved as " << library.lastBackup() << Qt::endl;

    VolumeResolver system;
    const VolumeResolver &volumes = resolver ? *resolver : system;
    if (command == "scan")
        return scan(library, o, volumes, out, err);
    if (command == "query")
        return query(library, o, volumes, out, err);
    if (command == "stats")
        return !o.top.isEmpty() || o.recent || o.never ? topStats(library, o, out, err)
                                                       : stats(library, o, volumes, out);
    if (command == "playlist")
        return playlist(library, o, out, err);
    if (command == "autoplaylist")
        return autoplaylist(library, o, out, err);
    if (command == "collection")
        return collection(library, o, out, err);
    if (command == "autodj")
        return autodj(library, o, out, err);
    if (command == "import")
        return importCommand(library, o, out, err);
    if (command == "locate")
        return locate(library, o, volumes, out, err);
    if (command == "art")
        return art(library, o, out, err);
    if (command == "tag")
        return tag(library, o, volumes, out, err);
    if (command == "tag-from-filename")
        return tagFromFilename(library, o, volumes, out, err);
    if (command == "organise" || command == "organize")
        return organise(library, o, volumes, out, err);
    if (command == "undo")
        return undo(library, o, volumes, out, err);
    if (command == "journal")
        return journal(library, o, out, err);
    if (command == "audit")
        return audit(library, o, volumes, out, err);
    if (command == "duplicates")
        return duplicates(library, o, volumes, out, err);
    if (command == "watch") {
        // The watcher opens its own connections; this one isn't needed.
        library.close();
        return watch(o, volumes, out, err);
    }
    return failWith(o, out, err, "Unknown command " + command);
}

}
