#include "identify.h"

#include "acoustid.h"
#include "artwork.h"
#include "coverart.h"
#include "job.h"
#include "library.h"
#include "textmatch.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QSet>
#include <QSqlQuery>
#include <QStandardPaths>

namespace Identify {

namespace {

// MusicBrainz's special "Various Artists" artist.
const QString variousArtistsId = "89ad4ac3-39f7-470e-963a-56509c546377";

QStringList csv(const QString &text) {
    QStringList out;
    for (const QString &part : text.split(',', Qt::SkipEmptyParts))
        if (!part.trimmed().isEmpty())
            out << part.trimmed();
    return out;
}

bool on(const Library &library, const QString &key, bool fallback) {
    const QString v = library.setting(key, fallback ? "1" : "0").toString().toLower();
    return v == "1" || v == "true" || v == "yes";
}

// The value most files share, when at least half of them share it.
QString consensus(const QStringList &values) {
    QMap<QString, int> counts;
    int nonEmpty = 0;
    for (const QString &v : values) {
        if (v.isEmpty())
            continue;
        ++counts[v];
        ++nonEmpty;
    }
    QString best;
    int n = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        if (it.value() > n)
            best = it.key(), n = it.value();
    return n * 2 >= values.size() && nonEmpty ? best : QString();
}

QString titleCase(const QString &genre) {
    QStringList words = genre.split(' ');
    for (QString &w : words)
        if (!w.isEmpty())
            w[0] = w[0].toUpper();
    return words.join(' ');
}

int preferenceRank(const QStringList &preferred, const QString &value) {
    for (int i = 0; i < preferred.size(); ++i)
        if (preferred[i].compare(value, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

} // namespace

QJsonObject File::toJson() const {
    return {{"track_id", trackId}, {"path", path},     {"title", title},
            {"artist", artist},    {"album", album},   {"album_artist", albumArtist},
            {"track", track},      {"disc", disc},     {"year", year},
            {"length_ms", durationMs}, {"mb_recording_id", mbRecordingId}, {"mb_release_id", mbReleaseId}};
}

QJsonObject Cluster::toJson() const {
    QJsonArray list;
    for (const File &f : files)
        list << f.toJson();
    return {{"key", key},   {"album", album},   {"album_artist", albumArtist}, {"artist", artist},
            {"folder", folder}, {"year", year}, {"files", list}};
}

QList<Cluster> cluster(Library &library, const QList<qint64> &trackIds) {
    QList<Cluster> out;
    QHash<QString, int> byKey;
    QSqlQuery q(library.db());
    q.prepare("SELECT title, artist, album, album_artist, track_no, disc_no, year, duration_ms, mb_track_id, "
              "mb_release_id, folder_id, availability FROM tracks WHERE id = ?");
    for (qint64 id : trackIds) {
        q.addBindValue(id);
        if (!q.exec() || !q.next())
            continue;
        File f;
        f.trackId = id;
        f.title = q.value(0).toString();
        f.artist = q.value(1).toString();
        f.album = q.value(2).toString();
        f.albumArtist = q.value(3).toString();
        f.track = q.value(4).toInt();
        f.disc = q.value(5).toInt();
        f.year = q.value(6).toInt();
        f.durationMs = q.value(7).toLongLong();
        f.mbRecordingId = q.value(8).toString();
        f.mbReleaseId = q.value(9).toString();
        f.folderId = q.value(10).toLongLong();
        f.online = q.value(11).toString() == "online";
        q.finish();
        f.path = library.absolutePath(id);
        f.folder = f.path.isEmpty() ? QString() : QFileInfo(f.path).absolutePath();

        // A disc folder ("CD1") belongs with its siblings.
        QString folderKey = QString::number(f.folderId);
        static const QRegularExpression discFolder("^(cd|disc|disk)\\s*\\d+$",
                                                   QRegularExpression::CaseInsensitiveOption);
        if (!f.folder.isEmpty() && discFolder.match(QFileInfo(f.folder).fileName()).hasMatch())
            folderKey = QFileInfo(f.folder).absolutePath();
        const QString key = f.album.isEmpty()
                                ? QString("~|%1|%2").arg(TextMatch::normalise(f.artist), folderKey)
                                : QString("%1|%2|%3").arg(TextMatch::normalise(f.album),
                                                          TextMatch::normalise(f.albumArtist), folderKey);
        if (!byKey.contains(key)) {
            byKey.insert(key, int(out.size()));
            Cluster c;
            c.key = key;
            out << c;
        }
        out[byKey.value(key)].files << f;
    }
    for (Cluster &c : out) {
        std::stable_sort(c.files.begin(), c.files.end(), [](const File &a, const File &b) {
            if (a.disc != b.disc)
                return a.disc < b.disc;
            if (a.track != b.track)
                return a.track < b.track;
            return a.path < b.path;
        });
        QStringList albums, albumArtists, artists, folders, years;
        for (const File &f : std::as_const(c.files)) {
            albums << f.album;
            albumArtists << f.albumArtist;
            artists << f.artist;
            folders << f.folder;
            years << (f.year ? QString::number(f.year) : QString());
        }
        c.album = consensus(albums);
        c.albumArtist = consensus(albumArtists);
        c.artist = c.albumArtist.isEmpty() ? consensus(artists) : c.albumArtist;
        c.folder = consensus(folders);
        c.year = consensus(years).toInt();
    }
    return out;
}

QJsonObject TrackMatch::toJson() const {
    QJsonObject o = track.toJson();
    o["file_track_id"] = trackId;
    o["disc"] = disc;
    o["score"] = qRound(score * 1000) / 1000.0;
    o["via"] = via;
    return o;
}

QString Candidate::confidence() const { return score >= 0.85 ? "high" : score >= 0.65 ? "medium" : "low"; }

QJsonObject Candidate::toJson(bool withTracks) const {
    QJsonObject parts_;
    for (auto it = parts.constBegin(); it != parts.constEnd(); ++it)
        parts_[it.key()] = qRound(it.value() * 1000) / 1000.0;
    QJsonObject o{{"release", release.toJson(false)},
                  {"score", qRound(score * 1000) / 1000.0},
                  {"confidence", confidence()},
                  {"sources", QJsonArray::fromStringList(sources)},
                  {"parts", parts_}};
    QJsonArray unmatchedList;
    for (qint64 id : unmatched)
        unmatchedList << id;
    o["unmatched"] = unmatchedList;
    if (withTracks) {
        QJsonArray list;
        for (const TrackMatch &m : mapping)
            list << m.toJson();
        o["mapping"] = list;
    }
    return o;
}

QJsonObject ClusterResult::toJson() const {
    QJsonArray list;
    for (int i = 0; i < candidates.size(); ++i)
        list << candidates[i].toJson(i == 0);
    QJsonObject o{{"cluster", cluster.toJson()}, {"candidates", list}, {"notes", QJsonArray::fromStringList(notes)}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QJsonObject Result::toJson() const {
    QJsonArray list;
    for (const ClusterResult &c : clusters)
        list << c.toJson();
    return {{"clusters", list},
            {"notes", QJsonArray::fromStringList(notes)},
            {"acoustid_used", acoustIdUsed},
            {"offline", offline}};
}

Options Options::load(const Library &library) {
    Options o;
    o.countries = csv(library.setting("identify.countries").toString());
    o.formats = csv(library.setting("identify.formats").toString());
    return o;
}

ApplyOptions ApplyOptions::load(const Library &library) {
    ApplyOptions o;
    o.writeGenres = on(library, "identify.write_genres", false);
    o.writeLabel = on(library, "identify.write_label", true);
    o.coverEmbed = on(library, "identify.cover_embed", true);
    o.coverFile = on(library, "identify.cover_file", false);
    o.coverSize = library.setting("identify.cover_size", 500).toInt();
    return o;
}

Identifier::Identifier(Library &library, Net::HttpClient &http, Job *job)
    : m_library(library), m_http(http), m_job(job) {}

Candidate Identifier::score(const Cluster &cluster, const MusicBrainz::Release &release, const Options &options,
                            const QHash<qint64, QMap<QString, double>> &acoustid) {
    Candidate c;
    c.release = release;

    // Every file against every release track, then the best pairs first.
    struct Pair {
        int file;
        int medium, track;
        double score;
        QString via;
    };
    QList<Pair> pairs;
    for (int fi = 0; fi < cluster.files.size(); ++fi) {
        const File &f = cluster.files[fi];
        const QMap<QString, double> fingerprinted = acoustid.value(f.trackId);
        for (int mi = 0; mi < release.media.size(); ++mi) {
            const MusicBrainz::Medium &m = release.media[mi];
            for (int ti = 0; ti < m.tracks.size(); ++ti) {
                const MusicBrainz::Track &t = m.tracks[ti];
                double s = 0.40 * TextMatch::similarity(f.title, t.title) +
                           0.30 * TextMatch::durationSimilarity(f.durationMs, t.lengthMs);
                const QString credit = MusicBrainz::creditText(t.credits);
                s += 0.10 * (f.artist.isEmpty() ? 0.5 : TextMatch::similarity(f.artist.split(';').first(), credit));
                if (f.track == t.position && (f.disc == 0 || f.disc == m.position))
                    s += 0.20;
                else if (f.track == t.position)
                    s += 0.10;
                QString via = "tags";
                if (fingerprinted.contains(t.recordingId)) {
                    s = qMax(s, 0.90 + 0.08 * fingerprinted.value(t.recordingId));
                    via = "acoustid";
                }
                if (!f.mbRecordingId.isEmpty() && f.mbRecordingId == t.recordingId) {
                    s = qMax(s, 0.98);
                    via = "mbid";
                }
                pairs.append({fi, mi, ti, s, via});
            }
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.score > b.score; });
    QSet<int> filesUsed;
    QSet<QPair<int, int>> tracksUsed;
    QHash<int, Pair> chosen;
    for (const Pair &p : std::as_const(pairs)) {
        if (p.score < 0.40)
            break;
        if (filesUsed.contains(p.file) || tracksUsed.contains({p.medium, p.track}))
            continue;
        filesUsed.insert(p.file);
        tracksUsed.insert({p.medium, p.track});
        chosen.insert(p.file, p);
    }
    double trackTotal = 0;
    for (int fi = 0; fi < cluster.files.size(); ++fi) {
        if (!chosen.contains(fi)) {
            c.unmatched << cluster.files[fi].trackId;
            continue;
        }
        const Pair &p = chosen[fi];
        const MusicBrainz::Medium &m = release.media[p.medium];
        TrackMatch tm;
        tm.trackId = cluster.files[fi].trackId;
        tm.disc = m.position;
        tm.discTracks = m.trackCount ? m.trackCount : int(m.tracks.size());
        tm.track = m.tracks[p.track];
        tm.score = p.score;
        tm.via = p.via;
        c.mapping << tm;
        trackTotal += p.score;
    }
    const int files = int(cluster.files.size());
    const int tracks = release.trackCount();
    const double trackScore = files ? trackTotal / files : 0;
    double countScore = 0;
    if (files && tracks)
        countScore = files == tracks ? 1.0 : files < tracks ? double(files) / tracks : 0.5 * tracks / files;
    const double albumScore = cluster.album.isEmpty() ? 0.5 : TextMatch::similarity(cluster.album, release.title);
    const double artistScore =
        cluster.artist.isEmpty() ? 0.5 : TextMatch::similarity(cluster.artist, MusicBrainz::creditText(release.credits));
    double dateScore = 0.5;
    if (cluster.year && (release.year() || !release.firstReleaseDate.isEmpty()))
        dateScore = cluster.year == release.year() ? 1.0
                    : cluster.year == release.firstReleaseDate.left(4).toInt() ? 0.9
                                                                               : 0.0;
    // The user's preferences: the first country or format listed scores 1,
    // later ones less; Official releases over bootlegs and promos.
    const int countryRank = preferenceRank(options.countries, release.country);
    const double countryScore = options.countries.isEmpty() ? 0.5 : countryRank < 0 ? 0.0 : 1.0 - 0.1 * countryRank;
    int formatRank = -1;
    for (const MusicBrainz::Medium &m : release.media) {
        const int r = preferenceRank(options.formats, m.format);
        if (r >= 0 && (formatRank < 0 || r < formatRank))
            formatRank = r;
    }
    const double formatScore = options.formats.isEmpty() ? 0.5 : formatRank < 0 ? 0.0 : 1.0 - 0.1 * formatRank;
    const double statusScore = release.status.isEmpty() || release.status == "Official" ? 1.0 : 0.3;

    c.parts = {{"tracks", trackScore}, {"track_count", countScore}, {"album", albumScore}, {"artist", artistScore},
               {"date", dateScore},    {"country", countryScore},  {"format", formatScore}, {"status", statusScore}};
    c.score = 0.45 * trackScore + 0.12 * countScore + 0.17 * albumScore + 0.13 * artistScore + 0.05 * dateScore +
              0.03 * countryScore + 0.03 * formatScore + 0.02 * statusScore;
    c.score = qBound(0.0, c.score, 1.0);
    return c;
}

ClusterResult Identifier::identifyCluster(const Cluster &cl, const Options &options, Result *run) {
    ClusterResult result;
    result.cluster = cl;
    MusicBrainz::Client mb(m_http);
    QStringList releaseIds; // in the order they were found
    QHash<QString, QStringList> sourcesOf;
    auto addRelease = [&](const QString &id, const QString &source) {
        if (id.isEmpty())
            return;
        if (!releaseIds.contains(id))
            releaseIds << id;
        if (!sourcesOf[id].contains(source))
            sourcesOf[id] << source;
    };
    QHash<qint64, QMap<QString, double>> acoustid;
    bool offline = false;

    if (!options.forceRelease.isEmpty()) {
        addRelease(options.forceRelease, "chosen");
    } else {
        // (a) The tags' own release IDs.
        if (options.useMbids) {
            QStringList ids;
            for (const File &f : cl.files)
                ids << f.mbReleaseId;
            addRelease(consensus(ids), "mbid");
        }
        // (b) AcoustID, file by file.
        const QString key = AcoustId::applicationKey(&m_library);
        if (options.useAcoustId && !key.isEmpty()) {
            QHash<QString, double> votes;
            int done = 0;
            for (const File &f : cl.files) {
                if (done >= options.maxFingerprints || (m_job && m_job->isCancelled()))
                    break;
                if (!f.online || f.path.isEmpty())
                    continue;
                ++done;
                const AcoustId::Fingerprint fp = AcoustId::compute(f.path, options.fpcalc);
                if (!fp.ok) {
                    if (!result.notes.contains(fp.error))
                        result.notes << fp.error;
                    if (fp.error.contains("not installed"))
                        break;
                    continue;
                }
                const AcoustId::Result r = AcoustId::lookup(m_http, key, fp.fingerprint, fp.durationSeconds);
                if (!r.ok) {
                    offline = offline || r.offline;
                    if (!result.notes.contains(r.error))
                        result.notes << r.error;
                    if (r.missingKey || r.offline)
                        break;
                    continue;
                }
                if (run)
                    run->acoustIdUsed = true;
                QSet<QString> votedHere;
                for (const AcoustId::Match &m : r.matches) {
                    if (m.score < 0.5)
                        continue;
                    acoustid[f.trackId][m.recordingId] = qMax(acoustid[f.trackId].value(m.recordingId), m.score);
                    for (const QString &rel : m.releaseIds) {
                        if (votedHere.contains(rel))
                            continue;
                        votedHere.insert(rel);
                        votes[rel] += m.score;
                    }
                }
                if (m_job)
                    m_job->setProgress("Fingerprinting", done, qMin<qint64>(cl.files.size(), options.maxFingerprints));
            }
            QList<QPair<double, QString>> ranked;
            for (auto it = votes.constBegin(); it != votes.constEnd(); ++it)
                ranked.append({it.value(), it.key()});
            std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
                return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            for (int i = 0; i < ranked.size() && i < 3; ++i)
                addRelease(ranked[i].second, "acoustid");
        } else if (options.useAcoustId && run && !run->notes.contains(AcoustId::missingKeyMessage())) {
            run->notes << AcoustId::missingKeyMessage();
        }
        // (c) A MusicBrainz search by the tags.
        if (options.useSearch && !offline) {
            QString error;
            if (!cl.album.isEmpty()) {
                const QList<MusicBrainz::Release> found =
                    mb.searchReleases(cl.artist, cl.album, int(cl.files.size()), 10, &error);
                int taken = 0;
                for (const MusicBrainz::Release &r : found) {
                    if (r.searchScore < 50 || taken >= 3)
                        break;
                    addRelease(r.id, "search");
                    ++taken;
                }
                // Without the track count: a partial album, or extra files.
                if (taken == 0 && !mb.lastWasOffline()) {
                    const QList<MusicBrainz::Release> loose = mb.searchReleases(cl.artist, cl.album, 0, 10, &error);
                    for (const MusicBrainz::Release &r : loose) {
                        if (r.searchScore < 50 || taken >= 3)
                            break;
                        addRelease(r.id, "search");
                        ++taken;
                    }
                }
            } else if (!cl.files.isEmpty()) {
                const File &f = cl.files.first();
                const QList<MusicBrainz::Recording> found = mb.searchRecordings(f.artist, f.title, 5, &error);
                int taken = 0;
                for (const MusicBrainz::Recording &rec : found) {
                    if (rec.searchScore < 70)
                        break;
                    for (const MusicBrainz::ReleaseRef &ref : rec.releases) {
                        if (taken >= 3)
                            break;
                        addRelease(ref.id, "search");
                        ++taken;
                    }
                }
            }
            offline = offline || mb.lastWasOffline();
            if (!error.isEmpty() && releaseIds.isEmpty())
                result.notes << error;
        }
    }

    // Look the candidates up in full, one a second.
    int looked = 0;
    for (const QString &id : std::as_const(releaseIds)) {
        if (looked >= options.maxCandidates || (m_job && m_job->isCancelled()))
            break;
        QString error;
        const MusicBrainz::Release release = mb.release(id, &error);
        ++looked;
        if (release.id.isEmpty()) {
            offline = offline || mb.lastWasOffline();
            result.notes << error;
            if (offline)
                break;
            continue;
        }
        Candidate c = score(cl, release, options, acoustid);
        c.sources = sourcesOf.value(id);
        // An ID the tags already carry is strong evidence, as is agreement
        // between AcoustID and the search.
        if (c.sources.contains("mbid") || c.sources.size() > 1)
            c.score = qMin(1.0, c.score + 0.03);
        result.candidates << c;
    }
    std::stable_sort(result.candidates.begin(), result.candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    if (result.candidates.isEmpty())
        result.error = offline ? QString("Offline: MusicBrainz could not be reached")
                               : QString("No MusicBrainz release matches these tracks");
    if (run)
        run->offline = run->offline || offline;
    return result;
}

Result Identifier::identify(const QList<qint64> &trackIds, const Options &options) {
    Result run;
    const QList<Cluster> clusters = cluster(m_library, trackIds);
    if (m_job)
        m_job->start();
    for (int i = 0; i < clusters.size(); ++i) {
        if (m_job && m_job->isCancelled())
            break;
        if (m_job)
            m_job->setProgress(QString("Identifying %1").arg(clusters[i].album), i, clusters.size());
        run.clusters << identifyCluster(clusters[i], options, &run);
        if (run.offline)
            break; // the rest would fail the same way
    }
    if (run.offline)
        run.notes << "Offline: MusicBrainz could not be reached; what is shown came from the cache";
    if (m_job)
        m_job->finish();
    return run;
}

QList<TagEdit> editsFor(const Candidate &c, const ApplyOptions &options) {
    QList<TagEdit> edits;
    const MusicBrainz::Release &r = c.release;
    const QStringList albumArtists = MusicBrainz::creditNames(r.credits);
    const bool various = MusicBrainz::creditIds(r.credits).contains(variousArtistsId);
    QStringList genres;
    for (int i = 0; i < r.genres.size() && i < 3; ++i)
        genres << titleCase(r.genres[i]);
    for (const TrackMatch &m : c.mapping) {
        auto set = [&](const QString &field, const QString &value) {
            edits << TagEdit{m.trackId, field, value, {}, {}};
        };
        set("title", m.track.title);
        set("artist", MusicBrainz::creditNames(m.track.credits).join("; "));
        set("albumartist", albumArtists.join("; "));
        set("album", r.title);
        if (r.year())
            set("year", QString::number(r.year()));
        if (!r.firstReleaseDate.isEmpty())
            set("originalyear", r.firstReleaseDate.left(4));
        set("track", QString::number(m.track.position));
        set("tracktotal", QString::number(m.discTracks));
        set("disc", QString::number(m.disc));
        set("disctotal", QString::number(r.media.size()));
        if (various)
            set("compilation", "1");
        if (options.writeGenres && !genres.isEmpty())
            set("genre", genres.join("; "));
        if (!m.track.isrcs.isEmpty())
            set("isrc", m.track.isrcs.first());
        if (options.writeLabel && !r.labels.isEmpty()) {
            if (!r.labels.first().name.isEmpty())
                set("label", r.labels.first().name);
            if (!r.labels.first().catalogNumber.isEmpty())
                set("catalognumber", r.labels.first().catalogNumber);
        }
        if (!r.country.isEmpty())
            set("releasecountry", r.country);
        set("mbrecordingid", m.track.recordingId);
        set("mbreleasetrackid", m.track.id);
        set("mbreleaseid", r.id);
        set("mbartistid", MusicBrainz::creditIds(m.track.credits).join("; "));
        set("mbalbumartistid", MusicBrainz::creditIds(r.credits).join("; "));
        if (!r.releaseGroupId.isEmpty())
            set("mbreleasegroupid", r.releaseGroupId);
    }
    return edits;
}

QJsonObject ApplyResult::toJson() const {
    QJsonObject o = tags.toJson();
    if (!cover.isEmpty())
        o["cover"] = cover;
    if (!coverFile.isEmpty())
        o["cover_file"] = coverFile;
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

ApplyResult apply(Library &library, Net::HttpClient &http, const Candidate &candidate, const ApplyOptions &options,
                  Job *job) {
    ApplyResult result;
    QList<TagEdit> edits = editsFor(candidate, options);
    if (edits.isEmpty()) {
        result.error = "Nothing to write: no file matched a track of this release";
        return result;
    }

    CoverArt::Fetched cover;
    const bool wantCover = (options.coverEmbed || options.coverFile) && !options.dryRun;
    if (wantCover) {
        cover = CoverArt::front(http, candidate.release.id, options.coverSize, candidate.release.releaseGroupId);
        result.cover = cover.ok ? QString("fetched") : cover.offline ? QString("offline") : cover.notFound ? QString("not-found") : QString("failed");
        if (cover.ok && options.coverEmbed) {
            for (const TrackMatch &m : candidate.mapping)
                edits << TagEdit{m.trackId, "artwork", {}, cover.bytes, cover.mime};
        }
    } else if (options.coverEmbed || options.coverFile) {
        result.cover = "skipped";
    }

    TagWriter writer(library, job);
    writer.setSandbox(options.sandbox);
    TagWriteOptions o;
    o.dryRun = options.dryRun;
    o.source = "identify";
    result.tags = writer.apply(edits, o);
    if (options.dryRun || !cover.ok)
        return result;
    if (options.coverEmbed && result.tags.ok)
        result.cover = "embedded";

    // The picture for the album in the library, and cover.jpg when asked.
    const QString cacheDir = options.artCacheDir.isEmpty() ? ArtworkCache::defaultCacheDir() : options.artCacheDir;
    QDir().mkpath(cacheDir + "/fetched");
    const QString ext = cover.mime.contains("png") ? "png" : "jpg";
    const QString original = QString("%1/fetched/%2-%3.%4")
                                 .arg(cacheDir, cover.releaseId.isEmpty() ? "rg-" + cover.releaseGroupId : cover.releaseId)
                                 .arg(CoverArt::snapSize(options.coverSize))
                                 .arg(ext);
    QSaveFile save(original);
    if (save.open(QIODevice::WriteOnly) && save.write(cover.bytes) == cover.bytes.size() && save.commit()) {
        QSqlQuery q(library.db());
        q.prepare("SELECT album_id FROM tracks WHERE id = ?");
        q.addBindValue(candidate.mapping.first().trackId);
        if (q.exec() && q.next() && !q.value(0).isNull()) {
            const qint64 album = q.value(0).toLongLong();
            q.finish();
            ArtworkCache(library, cacheDir).setAlbumPicture(album, original);
        }
    }
    if (options.coverFile) {
        QString folder;
        for (const TrackMatch &m : candidate.mapping) {
            const QString f = QFileInfo(library.absolutePath(m.trackId)).absolutePath();
            if (folder.isEmpty())
                folder = f;
            else if (folder != f)
                folder = "\n"; // several folders: no single place for it
        }
        bool hasPicture = false;
        for (const QString &n : QDir(folder).entryList(QDir::Files)) {
            const QString base = QFileInfo(n).completeBaseName().toLower();
            hasPicture = hasPicture || QStringList{"cover", "folder", "front"}.contains(base);
        }
        const QString target = folder + "/cover." + ext;
        if (!folder.isEmpty() && folder != "\n" && !hasPicture && Journal::insideSandbox(options.sandbox, target)) {
            QSaveFile out(target);
            if (out.open(QIODevice::WriteOnly) && out.write(cover.bytes) == cover.bytes.size() && out.commit()) {
                result.coverFile = target;
                if (!options.coverEmbed)
                    result.cover = "saved";
            }
        }
    }
    return result;
}

}
