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
                               "tag-from-filename", "audit", "duplicates"};

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
                 file), custom1..custom5. An empty value clears the field.
  --set-mask f=m tag: set a field from a mask, per track: --set-mask 'album=<Folder:1>'
  --apply        organise: move the files
  --mask <mask>  organise: where files go (see docs/masks.md)
  --to <folder>  organise: under this folder instead of each track's library folder
  --keep-folders organise: leave emptied folders
  --pattern <p>  tag-from-filename: what file names look like
  --by <how>     duplicates: tags (default), content or fingerprint
  --any-length   duplicates: match tags whatever the lengths

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

int undo(Library &library, const Options &o, const VolumeResolver &resolver, QTextStream &out, QTextStream &err) {
    library.refreshVolumes(resolver);
    const QString batch = o.rest.isEmpty() ? Journal::lastUndoable(library) : o.rest.first();
    if (batch.isEmpty())
        return failWith(o, out, err, "There is nothing to undo");
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
        else if (a == "--count")
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
