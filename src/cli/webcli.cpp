#include "webcli.h"

#include "acoustid.h"
#include "coverart.h"
#include "http.h"
#include "identify.h"
#include "job.h"
#include "library.h"
#include "lyrics.h"
#include "query.h"
#include "scrobble.h"
#include "volumes.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>

namespace WebCli {

const char *usage = R"(       omamp identify <query> [--dry-run | --apply] [--release <mbid>] [--min-score 0.8]
                                         match tracks to MusicBrainz releases (Picard's way)
                                         and show or write the tags and front cover
       omamp fetch-art <query> | --missing [--embed] [--save-cover] [--dry-run]
                                         front covers from the Cover Art Archive
       omamp lyrics <query> [--fetch] [--write lrc|tag] [--dry-run]
                                         lyrics on disk, cached, or from LRCLIB
       omamp scrobble status | flush | login listenbrainz --token <t> | login lastfm [--complete]
                       | logout listenbrainz|lastfm
                                         the ListenBrainz and Last.fm queue
       omamp net status | set <key> <value> | unset <key> | clear-cache
                                         web settings, keys and the response cache
)";

namespace {

const QStringList commandNames = {"identify", "fetch-art", "lyrics", "scrobble", "net"};

// Settings `omamp net set` may change. Secrets are never printed back.
const QStringList settablePrefixes = {"acoustid.", "identify.", "coverart.", "lyrics.", "scrobble.",
                                      "listenbrainz.", "lastfm.", "net."};
const QStringList secretKeys = {"acoustid.key", "listenbrainz.token", "lastfm.api_secret", "lastfm.session_key",
                                "lastfm.pending_token"};

struct Options {
    bool json = false, dryRun = false, apply = false, missing = false, fetch = false, refresh = false;
    bool embed = false, saveCover = false, noAcoustId = false, noSearch = false, noArt = false, genres = false;
    bool coverFile = false, syncedInTag = false, complete = false, show = false;
    QString db, release, write, token;
    double minScore = 0.80;
    int size = 0;
    QStringList rest;
};

QString json(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

int failWith(const Options &o, QTextStream &out, QTextStream &err, const QString &message) {
    if (o.json)
        out << json({{"ok", false}, {"error", message}}) << Qt::flush;
    else
        err << "omamp: " << message << Qt::endl;
    return 1;
}

QString shown(const QString &field, const QString &value) {
    if (value.isEmpty())
        return "(empty)";
    if (field == "artwork")
        return "(picture)";
    QString v = value;
    v.replace('\n', " / ");
    return v.size() > 60 ? v.left(57) + "..." : v;
}

void printChanges(const TagBatchResult &r, QTextStream &out, QTextStream &err, const QString &indent) {
    for (const TagFileResult &f : r.files) {
        if (!f.ok) {
            err << "omamp: " << (f.path.isEmpty() ? QString("track %1").arg(f.trackId) : f.path) << ": " << f.error
                << Qt::endl;
            continue;
        }
        if (f.changes.isEmpty())
            continue;
        out << indent << QFileInfo(f.path).fileName() << '\n';
        for (const TagFieldChange &c : f.changes)
            out << indent << "  " << c.field << ": " << shown(c.field, c.oldValue) << " -> "
                << shown(c.field, c.newValue) << '\n';
    }
}

QList<qint64> idsFor(Library &library, const Options &o, const VolumeResolver &resolver, QString *error) {
    library.refreshVolumes(resolver);
    return Query::ids(library, o.rest.join(' '), {}, error);
}

QString releaseLine(const Identify::Candidate &c) {
    const MusicBrainz::Release &r = c.release;
    QStringList facts;
    if (!r.date.isEmpty())
        facts << r.date;
    if (!r.country.isEmpty())
        facts << r.country;
    if (!r.formats().isEmpty())
        facts << r.formats();
    facts << QString("%1 tracks").arg(r.trackCount());
    if (!r.status.isEmpty() && r.status != "Official")
        facts << r.status;
    return QString("%1 %2  %3 – %4 (%5) [%6]")
        .arg(c.score, 0, 'f', 2)
        .arg(c.confidence(), -6)
        .arg(MusicBrainz::creditText(r.credits), r.title, facts.join(", "), c.sources.join(", "));
}

// --- identify -------------------------------------------------------------

int identify(Library &library, Net::HttpClient &http, const Options &o, const VolumeResolver &resolver,
             QTextStream &out, QTextStream &err) {
    if (o.rest.join(' ').trimmed().isEmpty())
        return failWith(o, out, err, "identify needs a query, for example: omamp identify 'album:\"night drive\"'");
    if (o.dryRun && o.apply)
        return failWith(o, out, err, "Choose --dry-run or --apply, not both");
    QString error;
    const QList<qint64> ids = idsFor(library, o, resolver, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);
    if (ids.isEmpty())
        return failWith(o, out, err, "The query found no tracks");

    Identify::Options options = Identify::Options::load(library);
    options.useAcoustId = !o.noAcoustId;
    options.useSearch = !o.noSearch;
    options.forceRelease = o.release;
    if (!o.release.isEmpty() && Identify::cluster(library, ids).size() > 1)
        return failWith(o, out, err,
                        "--release fits one album at a time, and the query found tracks from several. Narrow it to "
                        "one album.");
    Identify::ApplyOptions applyOptions = Identify::ApplyOptions::load(library);
    if (o.noArt)
        applyOptions.coverEmbed = applyOptions.coverFile = false;
    if (o.coverFile)
        applyOptions.coverFile = true;
    if (o.genres)
        applyOptions.writeGenres = true;
    if (o.size > 0)
        applyOptions.coverSize = o.size;

    if (!o.json)
        err << "omamp: identifying " << ids.size() << " tracks (MusicBrainz allows one request a second)..."
            << Qt::endl;
    Job job("identify");
    http.setJob(&job);
    Identify::Identifier identifier(library, http, &job);
    const Identify::Result result = identifier.identify(ids, options);

    QJsonArray clustersJson;
    bool ok = true;
    int applied = 0, skipped = 0;
    for (const Identify::ClusterResult &c : result.clusters) {
        QJsonObject cj = c.toJson();
        const Identify::Candidate *best = c.best();
        if (!o.json) {
            const QString name = c.cluster.album.isEmpty() ? QString("(no album)") : c.cluster.album;
            out << QString("%1 – %2: %3 files").arg(c.cluster.artist.isEmpty() ? QString("(various)") : c.cluster.artist,
                                                     name)
                       .arg(c.cluster.files.size());
            if (!c.cluster.folder.isEmpty())
                out << " in " << c.cluster.folder;
            out << '\n';
            for (int i = 0; i < c.candidates.size() && i < 3; ++i)
                out << "  " << (i + 1) << ". " << releaseLine(c.candidates[i]) << '\n'
                    << "     https://musicbrainz.org/release/" << c.candidates[i].release.id << '\n';
            for (const QString &n : c.notes)
                out << "  note: " << n << '\n';
            if (!c.error.isEmpty())
                out << "  " << c.error << '\n';
        }
        if (!best) {
            ok = false;
            clustersJson << cj;
            continue;
        }
        if (!o.json && !best->unmatched.isEmpty())
            out << "  " << best->unmatched.size() << " files match no track of the best release and are left alone\n";
        const bool goodEnough = best->score >= o.minScore || !o.release.isEmpty();
        if (o.apply && !goodEnough) {
            ++skipped;
            const QString why = QString("Not written: the best score %1 is under %2. Check the release and pass "
                                        "--release %3 to use it anyway.")
                                    .arg(best->score, 0, 'f', 2)
                                    .arg(o.minScore, 0, 'f', 2)
                                    .arg(best->release.id);
            cj["skipped"] = why;
            if (!o.json)
                out << "  " << why << '\n';
            clustersJson << cj;
            continue;
        }
        Identify::ApplyOptions a = applyOptions;
        a.dryRun = !o.apply;
        const Identify::ApplyResult r = Identify::apply(library, http, *best, a, &job);
        cj[o.apply ? "applied" : "changes"] = r.toJson();
        ok = ok && r.tags.ok && r.error.isEmpty();
        if (o.apply && !r.tags.batch.isEmpty())
            ++applied;
        if (!o.json) {
            out << (o.apply ? "  Written:\n" : "  Would change (dry run; --apply writes):\n");
            printChanges(r.tags, out, err, "    ");
            if (o.apply) {
                out << QString("  %1 changed, %2 already matched, %3 failed").arg(r.tags.changed).arg(r.tags.unchanged).arg(r.tags.failed);
                if (!r.cover.isEmpty())
                    out << "; cover: " << r.cover;
                if (!r.tags.batch.isEmpty())
                    out << "; undo with: omamp undo " << r.tags.batch;
                out << '\n';
            } else if (applyOptions.coverEmbed || applyOptions.coverFile) {
                out << "    (the front cover is fetched from the Cover Art Archive on --apply)\n";
            }
            if (!r.error.isEmpty())
                err << "omamp: " << r.error << Qt::endl;
        }
        clustersJson << cj;
    }
    if (o.json) {
        QJsonObject j = result.toJson();
        j["clusters"] = clustersJson;
        j["ok"] = ok;
        j["dry_run"] = !o.apply;
        j["applied"] = applied;
        j["skipped"] = skipped;
        out << json(j) << Qt::flush;
    } else {
        for (const QString &n : result.notes)
            out << "Note: " << n << '\n';
        out << Qt::flush;
    }
    return ok && !result.offline ? 0 : 1;
}

// --- fetch-art ------------------------------------------------------------

int fetchArt(Library &library, Net::HttpClient &http, const Options &o, const VolumeResolver &resolver,
             QTextStream &out, QTextStream &err) {
    QList<qint64> albums;
    if (o.missing) {
        library.refreshVolumes(resolver);
        albums = CoverArt::albumsMissingArt(library);
    } else {
        if (o.rest.join(' ').trimmed().isEmpty())
            return failWith(o, out, err, "fetch-art needs a query or --missing");
        QString error;
        const QList<qint64> ids = idsFor(library, o, resolver, &error);
        if (!error.isEmpty())
            return failWith(o, out, err, error);
        QSqlQuery q(library.db());
        q.prepare("SELECT album_id FROM tracks WHERE id = ?");
        for (qint64 id : ids) {
            q.addBindValue(id);
            if (q.exec() && q.next() && !q.value(0).isNull() && !albums.contains(q.value(0).toLongLong()))
                albums << q.value(0).toLongLong();
        }
    }
    CoverArt::AlbumOptions options;
    options.size = o.size > 0 ? o.size : library.setting("coverart.size", 500).toInt();
    options.embed = o.embed;
    options.saveFile = o.saveCover;
    options.search = !o.noSearch;
    options.dryRun = o.dryRun;
    Job job("fetch-art");
    http.setJob(&job);
    QJsonArray list;
    QMap<QString, int> counts;
    bool offline = false;
    for (int i = 0; i < albums.size(); ++i) {
        const CoverArt::AlbumResult r = CoverArt::fetchForAlbum(library, http, albums[i], options, &job);
        list << r.toJson();
        ++counts[r.status];
        if (!o.json) {
            out << QString("%1 – %2: %3").arg(r.artist, r.album, r.status);
            if (!r.error.isEmpty())
                out << " (" << r.error << ")";
            if (!r.coverFile.isEmpty())
                out << "; saved " << r.coverFile;
            out << '\n' << Qt::flush;
        }
        if (r.status == "offline") {
            offline = true;
            break;
        }
    }
    if (o.json) {
        QJsonObject c;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            c[it.key()] = it.value();
        out << json({{"ok", !offline}, {"albums", list}, {"counts", c}, {"dry_run", o.dryRun}}) << Qt::flush;
    } else {
        QStringList parts;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            parts << QString("%1 %2").arg(it.value()).arg(it.key());
        out << albums.size() << " albums: " << (parts.isEmpty() ? QString("nothing to do") : parts.join(", ")) << '\n'
            << Qt::flush;
    }
    return offline ? 1 : 0;
}

// --- lyrics ---------------------------------------------------------------

int lyrics(Library &library, Net::HttpClient &http, const Options &o, const VolumeResolver &resolver,
           QTextStream &out, QTextStream &err) {
    if (o.rest.join(' ').trimmed().isEmpty())
        return failWith(o, out, err, "lyrics needs a query, for example: omamp lyrics 'title:\"coastline\"' --fetch");
    if (!o.write.isEmpty() && o.write != "lrc" && o.write != "tag")
        return failWith(o, out, err, "--write takes lrc (a .lrc file beside the track) or tag (inside the file)");
    QString error;
    const QList<qint64> ids = idsFor(library, o, resolver, &error);
    if (!error.isEmpty())
        return failWith(o, out, err, error);
    Lyrics::Options options;
    options.fetch = o.fetch;
    options.refresh = o.refresh;
    QList<Lyrics::Entry> entries;
    QJsonArray list;
    int found = 0;
    for (qint64 id : ids) {
        const Lyrics::Entry e = Lyrics::forTrack(library, &http, id, options);
        entries << e;
        found += e.found() ? 1 : 0;
        QJsonObject j = e.toJson();
        j["path"] = library.absolutePath(id);
        list << j;
        if (!o.json) {
            const Lyrics::Query q = Lyrics::queryFor(library, id);
            out << QString("%1 – %2: ").arg(q.artist, q.title);
            if (e.instrumental)
                out << "instrumental";
            else if (e.found())
                out << (e.synced.isEmpty() ? "plain" : "synced") << " lyrics from " << e.source;
            else
                out << (e.error.isEmpty() ? QString("none") : e.error);
            out << '\n';
            if (e.found() && (o.show || ids.size() == 1) && o.write.isEmpty()) {
                const QString text = e.synced.isEmpty() ? e.plain : e.synced;
                for (const QString &line : text.split('\n'))
                    out << "  " << line << '\n';
            }
        }
    }
    QJsonObject writeJson;
    bool ok = true;
    if (!o.write.isEmpty()) {
        const Lyrics::WriteResult w = Lyrics::write(library, entries, o.write, o.syncedInTag, o.dryRun);
        if (!w.error.isEmpty())
            return failWith(o, out, err, w.error);
        writeJson = w.tags.toJson();
        QJsonArray skipped;
        for (qint64 id : w.skipped)
            skipped << id;
        writeJson["skipped"] = skipped;
        ok = w.tags.ok;
        if (!o.json) {
            printChanges(w.tags, out, err, "  ");
            out << QString("%1: %2 tracks %3, %4 already matched, %5 failed, %6 left out (nothing suitable to write)")
                       .arg(o.dryRun ? "Dry run" : "Written")
                       .arg(w.tags.changed)
                       .arg(o.dryRun ? "would change" : "changed")
                       .arg(w.tags.unchanged)
                       .arg(w.tags.failed)
                       .arg(w.skipped.size());
            if (!w.tags.batch.isEmpty())
                out << "; undo with: omamp undo " << w.tags.batch;
            out << '\n';
        }
    }
    if (o.json) {
        QJsonObject j{{"ok", ok}, {"tracks", list}, {"found", found}};
        if (!o.write.isEmpty())
            j["write"] = writeJson;
        out << json(j) << Qt::flush;
    } else {
        out << found << " of " << ids.size() << " tracks have lyrics\n" << Qt::flush;
    }
    return ok ? 0 : 1;
}

// --- scrobble -------------------------------------------------------------

int scrobble(Library &library, Net::HttpClient &http, const Options &o, QTextStream &out, QTextStream &err) {
    const QString action = o.rest.value(0);
    Scrobble::Scrobbler scrobbler(library, &http);
    if (action.isEmpty() || action == "status") {
        QJsonArray list;
        for (const Scrobble::ServiceStatus &s : scrobbler.status()) {
            list << s.toJson();
            if (!o.json) {
                out << QString("%1: %2%3, %4 waiting, %5 sent, %6 failed")
                           .arg(s.service, s.configured ? QString("logged in") : QString("not set up"),
                                s.user.isEmpty() ? QString() : " as " + s.user)
                           .arg(s.pending)
                           .arg(s.sent)
                           .arg(s.failed);
                if (s.configured && !s.enabled)
                    out << " (turned off)";
                if (!s.lastError.isEmpty())
                    out << "; last error: " << s.lastError;
                out << '\n';
            }
        }
        if (o.json)
            out << json({{"ok", true}, {"services", list}}) << Qt::flush;
        return 0;
    }
    if (action == "flush") {
        const Scrobble::FlushResult r = scrobbler.flush();
        if (o.json) {
            QJsonObject j = r.toJson();
            j["ok"] = r.errors.isEmpty();
            out << json(j) << Qt::flush;
        } else {
            out << QString("Sent %1, failed %2, %3 still waiting").arg(r.sent).arg(r.failed).arg(r.remaining) << '\n';
            for (const QString &e : r.errors)
                err << "omamp: " << e << Qt::endl;
        }
        return r.errors.isEmpty() ? 0 : 1;
    }
    if (action == "login" || action == "logout") {
        const QString service = o.rest.value(1);
        if (service != "listenbrainz" && service != "lastfm")
            return failWith(o, out, err, QString("%1 takes listenbrainz or lastfm").arg(action));
        if (action == "logout") {
            const QStringList keys = service == "listenbrainz"
                                         ? QStringList{"listenbrainz.token", "listenbrainz.user"}
                                         : QStringList{"lastfm.session_key", "lastfm.user", "lastfm.pending_token"};
            for (const QString &k : keys)
                library.setSetting(k, QVariant());
            if (o.json)
                out << json({{"ok", true}, {"service", service}, {"logged_in", false}}) << Qt::flush;
            else
                out << "Logged out of " << service << "; waiting listens stay queued.\n";
            return 0;
        }
        if (service == "listenbrainz") {
            if (o.token.isEmpty())
                return failWith(o, out, err,
                                "login listenbrainz needs --token <token>; copy it from https://listenbrainz.org/settings/");
            QString user, error;
            if (!Scrobble::ListenBrainz::validateToken(http, o.token, &user, &error))
                return failWith(o, out, err, error.isEmpty() ? QString("ListenBrainz did not accept the token") : error);
            library.setSetting("listenbrainz.token", o.token);
            library.setSetting("listenbrainz.user", user);
            if (o.json)
                out << json({{"ok", true}, {"service", service}, {"user", user}}) << Qt::flush;
            else
                out << "Logged in to ListenBrainz as " << user << ".\n";
            return 0;
        }
        // Last.fm's desktop flow: a request token, the user grants access in
        // a browser, then the token becomes a session key.
        const QString key = Scrobble::LastFm::apiKey(library), secret = Scrobble::LastFm::apiSecret(library);
        if (key.isEmpty() || secret.isEmpty())
            return failWith(o, out, err,
                            "Last.fm needs an API key and secret first (https://www.last.fm/api/account/create): "
                            "omamp net set lastfm.api_key <key> and omamp net set lastfm.api_secret <secret>");
        if (!o.complete) {
            QString token, error;
            if (!Scrobble::LastFm::getToken(http, key, secret, &token, &error))
                return failWith(o, out, err, error);
            library.setSetting("lastfm.pending_token", token);
            const QString url = Scrobble::LastFm::authUrl(key, token);
            if (o.json)
                out << json({{"ok", true}, {"service", service}, {"auth_url", url}}) << Qt::flush;
            else
                out << "Open this page, allow Omamp, then run `omamp scrobble login lastfm --complete`:\n  " << url
                    << '\n';
            return 0;
        }
        const QString token = library.setting("lastfm.pending_token").toString();
        if (token.isEmpty())
            return failWith(o, out, err, "Run `omamp scrobble login lastfm` first");
        QString session, user, error;
        if (!Scrobble::LastFm::getSession(http, key, secret, token, &session, &user, &error))
            return failWith(o, out, err, error);
        library.setSetting("lastfm.session_key", session);
        library.setSetting("lastfm.user", user);
        library.setSetting("lastfm.pending_token", QVariant());
        if (o.json)
            out << json({{"ok", true}, {"service", service}, {"user", user}}) << Qt::flush;
        else
            out << "Logged in to Last.fm as " << user << ".\n";
        return 0;
    }
    return failWith(o, out, err, "scrobble takes status, flush, login or logout");
}

// --- net ------------------------------------------------------------------

int net(Library &library, Net::HttpClient &http, const Options &o, QTextStream &out, QTextStream &err) {
    const QString action = o.rest.value(0);
    if (action.isEmpty() || action == "status") {
        QJsonArray services;
        for (Net::Service s : {Net::Service::MusicBrainz, Net::Service::AcoustId, Net::Service::CoverArt,
                               Net::Service::Lrclib, Net::Service::ListenBrainz, Net::Service::LastFm})
            services << QJsonObject{{"service", Net::serviceName(s)},
                                    {"url", Net::baseUrl(s, &library)},
                                    {"interval_ms", Net::Config::intervalMs(s)}};
        const bool acoustid = !AcoustId::applicationKey(&library).isEmpty();
        const QJsonObject j{{"ok", true},
                            {"user_agent", Net::userAgent()},
                            {"offline", Net::Config::forcedOffline(&library)},
                            {"cache", http.cache().dir()},
                            {"acoustid_key", acoustid},
                            {"services", services}};
        if (o.json) {
            out << json(j) << Qt::flush;
        } else {
            out << "User-Agent: " << Net::userAgent() << '\n'
                << "Network: " << (Net::Config::forcedOffline(&library) ? "turned off" : "on") << '\n'
                << "Response cache: " << http.cache().dir() << '\n'
                << "AcoustID key: " << (acoustid ? "set" : "not set (identify uses MusicBrainz alone)") << '\n';
            for (const QJsonValue &v : services) {
                const QJsonObject s = v.toObject();
                out << QString("  %1: %2, one request every %3 ms")
                           .arg(s["service"].toString(), -12)
                           .arg(s["url"].toString())
                           .arg(s["interval_ms"].toInt())
                    << '\n';
            }
        }
        return 0;
    }
    if (action == "set" || action == "unset") {
        const QString key = o.rest.value(1);
        bool allowed = false;
        for (const QString &p : settablePrefixes)
            allowed = allowed || key.startsWith(p);
        if (!allowed)
            return failWith(o, out, err,
                            QString("net %1 changes web settings only (keys starting %2)")
                                .arg(action, settablePrefixes.join(", ")));
        if (action == "set" && o.rest.size() < 3)
            return failWith(o, out, err, "net set needs a key and a value");
        library.setSetting(key, action == "set" ? QVariant(o.rest.mid(2).join(' ')) : QVariant());
        const bool secret = secretKeys.contains(key);
        if (o.json)
            out << json({{"ok", true}, {"key", key}, {"set", action == "set"}}) << Qt::flush;
        else
            out << key << (action == "set" ? (secret ? " is set" : " = " + o.rest.mid(2).join(' ')) : QString(" is cleared"))
                << '\n';
        return 0;
    }
    if (action == "clear-cache") {
        const int removed = http.cache().clear();
        if (o.json)
            out << json({{"ok", true}, {"removed", removed}}) << Qt::flush;
        else
            out << "Removed " << removed << " cached answers.\n";
        return 0;
    }
    return failWith(o, out, err, "net takes status, set, unset or clear-cache");
}

} // namespace

bool isCommand(const QString &first) { return commandNames.contains(first); }

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
        else if (a == "--dry-run")
            o.dryRun = true;
        else if (a == "--apply")
            o.apply = true;
        else if (a == "--missing")
            o.missing = true;
        else if (a == "--fetch")
            o.fetch = true;
        else if (a == "--refresh")
            o.refresh = o.fetch = true;
        else if (a == "--embed")
            o.embed = true;
        else if (a == "--save-cover")
            o.saveCover = true;
        else if (a == "--cover-file")
            o.coverFile = true;
        else if (a == "--no-acoustid")
            o.noAcoustId = true;
        else if (a == "--no-search")
            o.noSearch = true;
        else if (a == "--no-art")
            o.noArt = true;
        else if (a == "--genres")
            o.genres = true;
        else if (a == "--synced-in-tag")
            o.syncedInTag = true;
        else if (a == "--complete")
            o.complete = true;
        else if (a == "--show")
            o.show = true;
        else if (a == "--db" || a == "--release" || a == "--write" || a == "--token") {
            if (!value(&v))
                return failWith(o, out, err, a + " needs a value");
            if (a == "--db")
                o.db = v;
            else if (a == "--release")
                o.release = v;
            else if (a == "--write")
                o.write = v;
            else
                o.token = v;
        } else if (a == "--min-score") {
            bool ok = false;
            o.minScore = value(&v) ? v.toDouble(&ok) : 0;
            if (!ok || o.minScore < 0 || o.minScore > 1)
                return failWith(o, out, err, "--min-score takes a number from 0 to 1");
        } else if (a == "--size") {
            bool ok = false;
            o.size = value(&v) ? v.toInt(&ok) : 0;
            if (!ok || o.size < 0)
                return failWith(o, out, err, "--size needs a number");
        } else if (a == "--") {
            o.rest += arguments.mid(i + 1);
            break;
        } else {
            o.rest << a;
        }
    }

    Library library(o.db);
    QString error;
    if (!library.open(&error))
        return failWith(o, out, err, error);
    if (!library.lastBackup().isEmpty() && !o.json)
        err << "omamp: upgraded the library; the old one is saved as " << library.lastBackup() << Qt::endl;
    VolumeResolver system;
    const VolumeResolver &volumes = resolver ? *resolver : system;
    Net::HttpClient http(&library);
    if (command == "identify")
        return identify(library, http, o, volumes, out, err);
    if (command == "fetch-art")
        return fetchArt(library, http, o, volumes, out, err);
    if (command == "lyrics")
        return lyrics(library, http, o, volumes, out, err);
    if (command == "scrobble")
        return scrobble(library, http, o, out, err);
    if (command == "net")
        return net(library, http, o, out, err);
    return failWith(o, out, err, "Unknown command " + command);
}

}
