#include "cli.h"

#include "job.h"
#include "library.h"
#include "query.h"
#include "scanner.h"
#include "volumes.h"
#include "artwork.h"
#include "watcher.h"

#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>

namespace Cli {

namespace {

const QStringList commands = {"scan", "query", "stats", "help", "--help", "-h", "version", "--version",
                               "watch", "locate", "art"};

const char *usage = R"(Usage: omamp [files or folders...]      open the player
       omamp open [files or folders...]  the same, for names that look like commands
       omamp scan [folder...]            add folders to the library and scan them;
                                         with no folder, rescan every library folder
       omamp query <text...>             search the library
       omamp stats                       count what is in the library
       omamp watch                       watch the library folders and rescan what changes
                                         (in the foreground; Ctrl+C stops)
       omamp locate <folder>             find missing tracks that moved into a folder
       omamp art album|track <id>        print the path of a picture (made if needed)
       omamp art all                     make pictures for every album not done yet

Options:
  --json         print JSON on stdout
  --db <path>    use this library file (default ~/.local/share/omamp/library.db)
  --limit <n>    query: print at most n tracks
  --workers <n>  scan: threads reading tags (default 4 to 8)
  --quick        scan: skip folders whose modification time hasn't changed
  --interval <s> watch: seconds between rescans of network folders (default 900)
  --dry-run      watch: print changes without rescanning
  --size <px>    art: 96, 256 or 600 (default 256)

Query examples:
  omamp query neon coast
  omamp query 'artist:"neon coast" year:1997..2001'
  omamp query 'rating:4.. -genre:christmas'
  omamp query 'genre:rock OR genre:metal'
)";

struct Options {
    bool json = false;
    QString db;
    int limit = 0;
    int workers = 0;
    int interval = 900;
    int size = 256;
    bool quick = false;
    bool dryRun = false;
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

int query(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    library.refreshVolumes(resolver);
    const QString text = o.rest.join(' ');
    Query::Options options;
    options.limit = o.limit;
    const Query::Result r = Query::run(library, text, options);
    if (!r.ok())
        return failWith(o, out, err, r.error);
    if (o.json) {
        QJsonArray tracks;
        for (const Query::Track &t : r.tracks)
            tracks << t.toJson();
        out << json({{"ok", true},
                     {"query", text},
                     {"total", r.total},
                     {"count", r.tracks.size()},
                     {"elapsed_ms", r.elapsedUs / 1000.0},
                     {"tracks", tracks}})
            << Qt::flush;
        return 0;
    }
    for (const Query::Track &t : r.tracks) {
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
    out << QString("%1 of %2 tracks").arg(r.tracks.size()).arg(r.total) << Qt::endl;
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
    QString error;
    if (!watcher.start(&error) && !error.isEmpty())
        err << "omamp: " << error << Qt::endl;
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

} // namespace

bool isCommand(const QString &first) { return commands.contains(first); }

int run(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver) {
    Options o;
    const QString command = arguments.value(0);
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
        else if (a == "--db") {
            if (!value(&o.db))
                return failWith(o, out, err, "--db needs a path");
        } else if (a == "--limit" || a == "--workers" || a == "--interval" || a == "--size") {
            bool ok = false;
            const int n = value(&v) ? v.toInt(&ok) : 0;
            if (!ok || n < 0)
                return failWith(o, out, err, a + " needs a number");
            (a == "--limit" ? o.limit : a == "--workers" ? o.workers : a == "--interval" ? o.interval : o.size) = n;
        } else if (a == "--quick") {
            o.quick = true;
        } else if (a == "--dry-run") {
            o.dryRun = true;
        } else if (a == "--") {
            o.rest += arguments.mid(i + 1);
            break;
        } else
            o.rest << a;
    }

    if (command == "help" || command == "--help" || command == "-h") {
        out << usage << Qt::flush;
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
        return stats(library, o, volumes, out);
    if (command == "locate")
        return locate(library, o, volumes, out, err);
    if (command == "art")
        return art(library, o, out, err);
    if (command == "watch") {
        // The watcher opens its own connections; this one isn't needed.
        library.close();
        return watch(o, volumes, out, err);
    }
    return failWith(o, out, err, "Unknown command " + command);
}

}
