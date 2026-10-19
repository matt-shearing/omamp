#include "cli.h"

#include "job.h"
#include "library.h"
#include "query.h"
#include "scanner.h"
#include "volumes.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>

namespace Cli {

namespace {

const QStringList commands = {"scan", "query", "stats", "help", "--help", "-h", "version", "--version"};

const char *usage = R"(Usage: omamp [files or folders...]      open the player
       omamp open [files or folders...]  the same, for names that look like commands
       omamp scan [folder...]            add folders to the library and scan them;
                                         with no folder, rescan every library folder
       omamp query <text...>             search the library
       omamp stats                       count what is in the library

Options:
  --json         print JSON on stdout
  --db <path>    use this library file (default ~/.local/share/omamp/library.db)
  --limit <n>    query: print at most n tracks
  --workers <n>  scan: threads reading tags (default 4 to 8)

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
    const ScanResult r = o.rest.isEmpty() ? scanner.rescanAll() : scanner.scan(o.rest);
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
        } else if (a == "--limit" || a == "--workers") {
            bool ok = false;
            const int n = value(&v) ? v.toInt(&ok) : 0;
            if (!ok || n < 0)
                return failWith(o, out, err, a + " needs a number");
            (a == "--limit" ? o.limit : o.workers) = n;
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
    return failWith(o, out, err, "Unknown command " + command);
}

}
