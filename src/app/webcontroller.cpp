#include "webcontroller.h"

#include "acoustid.h"
#include "artwork.h"
#include "coverart.h"
#include "http.h"
#include "identify.h"
#include "job.h"
#include "library.h"
#include "librarycontroller.h"
#include "lyrics.h"
#include "musicbrainz.h"
#include "tagwriter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

namespace {

// Field names as the review shows them.
QString fieldLabel(const QString &field) {
    static const QHash<QString, const char *> words = {
        {"title", QT_TRANSLATE_NOOP("WebController", "Title")},
        {"artist", QT_TRANSLATE_NOOP("WebController", "Artist")},
        {"album", QT_TRANSLATE_NOOP("WebController", "Album")},
        {"albumartist", QT_TRANSLATE_NOOP("WebController", "Album artist")},
        {"year", QT_TRANSLATE_NOOP("WebController", "Year")},
        {"originalyear", QT_TRANSLATE_NOOP("WebController", "Original year")},
        {"track", QT_TRANSLATE_NOOP("WebController", "Track")},
        {"tracktotal", QT_TRANSLATE_NOOP("WebController", "Tracks")},
        {"disc", QT_TRANSLATE_NOOP("WebController", "Disc")},
        {"disctotal", QT_TRANSLATE_NOOP("WebController", "Discs")},
        {"genre", QT_TRANSLATE_NOOP("WebController", "Genre")},
        {"label", QT_TRANSLATE_NOOP("WebController", "Label")},
        {"catalognumber", QT_TRANSLATE_NOOP("WebController", "Catalogue number")},
        {"releasecountry", QT_TRANSLATE_NOOP("WebController", "Country")},
        {"isrc", QT_TRANSLATE_NOOP("WebController", "ISRC")},
        {"artwork", QT_TRANSLATE_NOOP("WebController", "Cover")},
        {"mbrecordingid", QT_TRANSLATE_NOOP("WebController", "MusicBrainz recording")},
        {"mbreleasetrackid", QT_TRANSLATE_NOOP("WebController", "MusicBrainz track")},
        {"mbreleaseid", QT_TRANSLATE_NOOP("WebController", "MusicBrainz release")},
        {"mbartistid", QT_TRANSLATE_NOOP("WebController", "MusicBrainz artist")},
        {"mbalbumartistid", QT_TRANSLATE_NOOP("WebController", "MusicBrainz album artist")},
        {"mbreleasegroupid", QT_TRANSLATE_NOOP("WebController", "MusicBrainz release group")}};
    const char *w = words.value(field);
    return w ? WebController::tr(w) : field;
}

QString clockText(qint64 ms) {
    if (ms <= 0)
        return {};
    const qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// Why a candidate scored as it did, in a sentence.
QString whyText(const Identify::Candidate &c, int files) {
    QStringList parts;
    const double tracks = c.parts.value("tracks");
    parts << WebController::tr("tracks match %1%").arg(qRound(tracks * 100));
    const int count = c.release.trackCount();
    if (count == files)
        parts << WebController::tr("same number of tracks");
    else
        parts << WebController::tr("%1 files for %2 tracks").arg(files).arg(count);
    const double album = c.parts.value("album"), artist = c.parts.value("artist");
    parts << (album >= 0.9 ? WebController::tr("album title matches")
              : album >= 0.6 ? WebController::tr("album title is close") : WebController::tr("album title differs"));
    parts << (artist >= 0.9 ? WebController::tr("artist matches")
              : artist >= 0.6 ? WebController::tr("artist is close") : WebController::tr("artist differs"));
    QStringList from;
    for (const QString &s : c.sources) {
        if (s == "mbid")
            from << WebController::tr("the MusicBrainz IDs in the tags");
        else if (s == "acoustid")
            from << WebController::tr("AcoustID fingerprints");
        else if (s == "search")
            from << WebController::tr("a search by the tags");
        else if (s == "chosen")
            from << WebController::tr("your choice");
    }
    QString text = parts.join(", ");
    if (!text.isEmpty())
        text[0] = text[0].toUpper();
    if (!from.isEmpty())
        text += ". " + WebController::tr("Found by %1.").arg(from.join(WebController::tr(" and ")));
    return text;
}

// The fields a person reads first come first; the IDs last.
int fieldRank(const QString &field) {
    static const QStringList order = {"title",  "artist",     "album",        "albumartist", "track",
                                      "tracktotal", "disc",   "disctotal",    "year",        "originalyear",
                                      "genre",  "label",      "catalognumber", "releasecountry", "isrc", "artwork"};
    const int i = order.indexOf(field);
    return i < 0 ? int(order.size()) : i;
}

QJsonArray changesJson(const TagBatchResult &r) {
    QJsonArray out;
    for (const TagFileResult &f : r.files) {
        QList<TagFieldChange> changes = f.changes;
        std::stable_sort(changes.begin(), changes.end(), [](const TagFieldChange &a, const TagFieldChange &b) {
            return fieldRank(a.field) < fieldRank(b.field);
        });
        for (const TagFieldChange &c : changes)
            out.append(QJsonObject{{"trackId", f.trackId},
                                   {"file", QFileInfo(f.path).fileName()},
                                   {"field", c.field},
                                   {"label", fieldLabel(c.field)},
                                   {"oldValue", c.field == "artwork" ? (c.oldValue.isEmpty() ? QString() : WebController::tr("a picture")) : c.oldValue},
                                   {"newValue", c.field == "artwork" ? WebController::tr("the release's front cover") : c.newValue}});
        if (!f.ok)
            out.append(QJsonObject{{"trackId", f.trackId},
                                   {"file", QFileInfo(f.path).fileName()},
                                   {"field", "error"},
                                   {"label", WebController::tr("Error")},
                                   {"oldValue", QString()},
                                   {"newValue", f.error}});
    }
    return out;
}

// The apply options from the sheet's switches over the settings.
Identify::ApplyOptions applyOptionsFrom(const Library &library, const QVariantMap &m) {
    Identify::ApplyOptions o = Identify::ApplyOptions::load(library);
    if (m.contains("coverEmbed"))
        o.coverEmbed = m.value("coverEmbed").toBool();
    if (m.contains("coverFile"))
        o.coverFile = m.value("coverFile").toBool();
    if (m.contains("writeGenres"))
        o.writeGenres = m.value("writeGenres").toBool();
    if (m.contains("writeLabel"))
        o.writeLabel = m.value("writeLabel").toBool();
    return o;
}

QString extensionFor(const QString &mime) { return mime.contains("png") ? "png" : "jpg"; }

} // namespace

WebController::WebController(LibraryController *library) : QObject(library), m_controller(library) {}

WebController::~WebController() {
    if (m_lyricsThread) {
        m_lyricsThread->wait();
        delete m_lyricsThread;
    }
}

// ---- Identify ---------------------------------------------------------------

bool WebController::acoustIdKeySet() const {
    if (!qEnvironmentVariable("OMAMP_ACOUSTID_KEY").trimmed().isEmpty())
        return true;
    Library *lib = m_controller->library();
    return lib && !lib->setting("acoustid.key").toString().trimmed().isEmpty();
}

bool WebController::identifyOffline() const { return m_result && m_result->offline; }

int WebController::identifyFiles() const {
    int n = 0;
    if (m_result)
        for (const Identify::ClusterResult &c : m_result->clusters)
            n += int(c.cluster.files.size());
    return n;
}

QVariantList WebController::clusters() const {
    QVariantList out;
    if (!m_result)
        return out;
    const QLocale locale;
    for (int i = 0; i < m_result->clusters.size(); ++i) {
        const Identify::ClusterResult &cr = m_result->clusters[i];
        const Identify::Cluster &cl = cr.cluster;
        QVariantMap m;
        m["index"] = i;
        m["album"] = cl.album.isEmpty() ? tr("(no album)") : cl.album;
        m["artist"] = cl.albumArtist.isEmpty() ? cl.artist : cl.albumArtist;
        m["folder"] = cl.folder;
        m["fileCount"] = int(cl.files.size());
        m["error"] = cr.error;
        m["notes"] = cr.notes;
        m["included"] = m_included.value(i, false);
        const int chosen = m_chosen.value(i, -1);
        m["chosen"] = chosen;
        QVariantList candidates;
        for (int k = 0; k < cr.candidates.size(); ++k) {
            const Identify::Candidate &c = cr.candidates[k];
            QStringList labels;
            for (const MusicBrainz::Label &l : c.release.labels)
                if (!l.name.isEmpty())
                    labels << (l.catalogNumber.isEmpty() ? l.name : l.name + " " + l.catalogNumber);
            candidates << QVariantMap{{"index", k},
                                      {"mbid", c.release.id},
                                      {"title", c.release.title},
                                      {"disambiguation", c.release.disambiguation},
                                      {"artist", MusicBrainz::creditText(c.release.credits)},
                                      {"date", c.release.date},
                                      {"country", c.release.country},
                                      {"format", c.release.formats()},
                                      {"tracks", c.release.trackCount()},
                                      {"labels", labels.join(", ")},
                                      {"score", qRound(c.score * 100)},
                                      {"confidence", c.confidence()},
                                      {"why", whyText(c, int(cl.files.size()))},
                                      {"url", "https://musicbrainz.org/release/" + c.release.id}};
        }
        m["candidates"] = candidates;
        // The chosen release's pairing of files and tracks, in file order.
        QVariantList mapping;
        if (chosen >= 0 && chosen < cr.candidates.size()) {
            const Identify::Candidate &c = cr.candidates[chosen];
            QHash<qint64, const Identify::TrackMatch *> byTrack;
            for (const Identify::TrackMatch &t : c.mapping)
                byTrack.insert(t.trackId, &t);
            for (const Identify::File &f : cl.files) {
                QVariantMap row{{"trackId", f.trackId},
                                {"file", QFileInfo(f.path).fileName()},
                                {"from", f.title},
                                {"fromLength", clockText(f.durationMs)}};
                if (const Identify::TrackMatch *t = byTrack.value(f.trackId)) {
                    row["matched"] = true;
                    row["position"] = t->discTracks > 0 && c.release.media.size() > 1
                                          ? QString("%1-%2").arg(t->disc).arg(t->track.position)
                                          : QString::number(t->track.position);
                    row["title"] = t->track.title;
                    row["length"] = clockText(t->track.lengthMs);
                    row["score"] = qRound(t->score * 100);
                    row["via"] = t->via;
                } else {
                    row["matched"] = false;
                    row["score"] = 0;
                }
                mapping << row;
            }
        }
        m["mapping"] = mapping;
        m["changes"] = m_changes.value(i);
        m["previewError"] = m_previewErrors.value(i);
        int changed = 0;
        QSet<qint64> files;
        for (const QVariant &v : m_changes.value(i)) {
            const QVariantMap ch = v.toMap();
            files.insert(ch.value("trackId").toLongLong());
            ++changed;
        }
        m["changedFiles"] = int(files.size());
        m["changeCount"] = changed;
        out << m;
    }
    return out;
}

QVariantMap WebController::identifyOptions() const {
    Library *lib = m_controller->library();
    if (!lib)
        return {};
    const Identify::ApplyOptions o = Identify::ApplyOptions::load(*lib);
    return {{"coverEmbed", o.coverEmbed}, {"coverFile", o.coverFile}, {"writeGenres", o.writeGenres},
            {"writeLabel", o.writeLabel}};
}

bool WebController::identify(const QVariantList &trackIds) {
    QList<qint64> ids;
    for (const QVariant &v : trackIds)
        if (v.toLongLong() > 0)
            ids << v.toLongLong();
    if (ids.isEmpty() || !m_controller->activate())
        return false;
    m_result.reset();
    m_chosen.clear();
    m_included.clear();
    m_changes.clear();
    m_previewErrors.clear();
    m_notes.clear();
    m_identifyState = "working";
    emit identifyChanged();
    auto out = std::make_shared<Identify::Result>();
    const QString sandbox = m_controller->writeSandbox();
    const bool started = m_controller->runJob(
        "identify", tr("Looking up %n files on MusicBrainz", "", int(ids.size())),
        [ids, out, sandbox](Library &library, Job *job) {
            Net::HttpClient http(&library);
            http.setJob(job);
            Identify::Identifier identifier(library, http, job);
            *out = identifier.identify(ids, Identify::Options::load(library));
            // What each cluster's best release would change: a dry run, which
            // reads the files and writes nothing.
            QJsonArray previews;
            Identify::ApplyOptions a = Identify::ApplyOptions::load(library);
            a.dryRun = true;
            a.sandbox = sandbox;
            for (const Identify::ClusterResult &c : out->clusters) {
                if (job->isCancelled() || !c.best()) {
                    previews.append(QJsonObject{});
                    continue;
                }
                const Identify::ApplyResult r = Identify::apply(library, http, *c.best(), a, nullptr);
                previews.append(QJsonObject{{"changes", changesJson(r.tags)}, {"error", r.error.isEmpty() ? r.tags.error : r.error}});
            }
            return QJsonObject{{"ok", true}, {"previews", previews}, {"cancelled", job->isCancelled()}};
        },
        [this, out](const QJsonObject &r) {
            m_result = out;
            const QJsonArray previews = r.value("previews").toArray();
            for (int i = 0; i < m_result->clusters.size(); ++i) {
                const Identify::ClusterResult &c = m_result->clusters[i];
                m_chosen << (c.candidates.isEmpty() ? -1 : 0);
                // Low-confidence matches are left out until the user picks them.
                m_included << (c.best() && c.best()->confidence() != QLatin1String("low"));
                const QJsonObject p = previews.at(i).toObject();
                m_changes << p.value("changes").toArray().toVariantList();
                m_previewErrors << p.value("error").toString();
            }
            // The core's notes are for the command line; the window says the
            // ones it knows in its own words and passes on the rest.
            for (const QString &n : std::as_const(m_result->notes)) {
                if (n.startsWith("AcoustID was skipped") || n.startsWith("Offline:"))
                    continue;
                m_notes << n;
            }
            m_identifyState = r.value("cancelled").toBool() && m_result->clusters.isEmpty() ? "idle" : "ready";
            emit identifyChanged();
        });
    if (!started) {
        m_identifyState = "idle";
        emit identifyChanged();
    }
    return started;
}

void WebController::preview(const QList<int> &which) {
    if (!m_result)
        return;
    QList<QPair<int, Identify::Candidate>> todo;
    for (int i : which) {
        const int k = m_chosen.value(i, -1);
        if (i >= 0 && i < m_result->clusters.size() && k >= 0 && k < m_result->clusters[i].candidates.size())
            todo.append({i, m_result->clusters[i].candidates[k]});
    }
    if (todo.isEmpty()) {
        emit identifyChanged();
        return;
    }
    m_identifyState = "working";
    emit identifyChanged();
    const QString sandbox = m_controller->writeSandbox();
    auto result = m_result;
    m_controller->runJob(
        "identify", tr("Working out the changes"),
        [todo, sandbox](Library &library, Job *) {
            Net::HttpClient http(&library);
            Identify::ApplyOptions a = Identify::ApplyOptions::load(library);
            a.dryRun = true;
            a.sandbox = sandbox;
            QJsonArray previews;
            for (const auto &[i, candidate] : todo) {
                const Identify::ApplyResult r = Identify::apply(library, http, candidate, a, nullptr);
                previews.append(QJsonObject{{"cluster", i},
                                            {"changes", changesJson(r.tags)},
                                            {"error", r.error.isEmpty() ? r.tags.error : r.error}});
            }
            return QJsonObject{{"ok", true}, {"previews", previews}};
        },
        [this, result](const QJsonObject &r) {
            // A new identify replaced the result meanwhile: this preview is stale.
            if (result != m_result)
                return;
            for (const QJsonValue &v : r.value("previews").toArray()) {
                const QJsonObject p = v.toObject();
                const int i = p.value("cluster").toInt();
                if (i >= 0 && i < m_changes.size()) {
                    m_changes[i] = p.value("changes").toArray().toVariantList();
                    m_previewErrors[i] = p.value("error").toString();
                }
            }
            m_identifyState = "ready";
            emit identifyChanged();
        });
}

void WebController::chooseCandidate(int cluster, int candidate) {
    if (!m_result || cluster < 0 || cluster >= m_result->clusters.size())
        return;
    if (candidate < 0 || candidate >= m_result->clusters[cluster].candidates.size() ||
        m_chosen.value(cluster) == candidate)
        return;
    m_chosen[cluster] = candidate;
    m_included[cluster] = true;
    m_changes[cluster].clear();
    preview({cluster});
}

void WebController::setIncluded(int cluster, bool on) {
    if (cluster < 0 || cluster >= m_included.size() || m_included[cluster] == on)
        return;
    m_included[cluster] = on && m_chosen.value(cluster, -1) >= 0;
    emit identifyChanged();
}

QString WebController::releaseIdFrom(const QString &text) {
    static const QRegularExpression uuid("([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");
    const QString t = text.trimmed();
    // A link to something other than a release (an artist, a recording) is not one.
    if (t.contains("musicbrainz.org/") && !t.contains("/release/"))
        return {};
    const QRegularExpressionMatch m = uuid.match(t);
    return m.hasMatch() ? m.captured(1).toLower() : QString();
}

bool WebController::useRelease(int cluster, const QString &text) {
    if (!m_result || cluster < 0 || cluster >= m_result->clusters.size())
        return false;
    const QString mbid = releaseIdFrom(text);
    if (mbid.isEmpty()) {
        emit message(tr("That isn't a MusicBrainz release ID or a link to a release"));
        return false;
    }
    const Identify::Cluster cl = m_result->clusters[cluster].cluster;
    auto out = std::make_shared<Identify::ClusterResult>();
    auto result = m_result;
    m_identifyState = "working";
    emit identifyChanged();
    return m_controller->runJob(
        "identify", tr("Looking up release %1").arg(mbid.left(8)),
        [cl, mbid, out](Library &library, Job *job) {
            Net::HttpClient http(&library);
            http.setJob(job);
            Identify::Identifier identifier(library, http, job);
            Identify::Options o = Identify::Options::load(library);
            o.forceRelease = mbid;
            *out = identifier.identifyCluster(cl, o);
            return QJsonObject{{"ok", true}};
        },
        [this, cluster, out, result, mbid](const QJsonObject &) {
            if (result != m_result)
                return;
            if (out->candidates.isEmpty()) {
                m_identifyState = "ready";
                emit identifyChanged();
                emit message(out->error.isEmpty() ? tr("MusicBrainz has no release %1").arg(mbid)
                                                  : tr("Couldn't use that release: %1").arg(out->error));
                return;
            }
            QList<Identify::Candidate> &list = m_result->clusters[cluster].candidates;
            list.removeIf([&](const Identify::Candidate &c) { return c.release.id == mbid; });
            list.prepend(out->candidates.first());
            m_chosen[cluster] = 0;
            m_included[cluster] = true;
            m_changes[cluster].clear();
            m_result->clusters[cluster].error.clear();
            preview({cluster});
        });
}

bool WebController::applyIdentify(const QVariantMap &options) {
    if (!m_result || m_identifyState != QLatin1String("ready"))
        return false;
    QList<Identify::Candidate> todo;
    for (int i = 0; i < m_result->clusters.size(); ++i) {
        const int k = m_chosen.value(i, -1);
        if (m_included.value(i) && k >= 0 && k < m_result->clusters[i].candidates.size())
            todo << m_result->clusters[i].candidates[k];
    }
    if (todo.isEmpty()) {
        emit message(tr("Nothing to save: tick an album first"));
        return false;
    }
    // The sheet's switches become the settings, so they are remembered.
    if (Library *lib = m_controller->library()) {
        for (const auto &[key, setting] : {std::pair{"coverEmbed", "identify.cover_embed"},
                                           std::pair{"coverFile", "identify.cover_file"},
                                           std::pair{"writeGenres", "identify.write_genres"},
                                           std::pair{"writeLabel", "identify.write_label"}})
            if (options.contains(key))
                lib->setSetting(setting, options.value(key).toBool() ? "1" : "0");
    }
    const QString sandbox = m_controller->writeSandbox();
    const QString artDir = m_controller->artCacheDir();
    m_identifyState = "applying";
    emit identifyChanged();
    return m_controller->runJob(
        "identify", tr("Saving tags from MusicBrainz"),
        [todo, options, sandbox, artDir](Library &library, Job *job) {
            Net::HttpClient http(&library);
            http.setJob(job);
            Identify::ApplyOptions a = applyOptionsFrom(library, options);
            a.sandbox = sandbox;
            a.artCacheDir = artDir;
            int changed = 0, failed = 0, covers = 0;
            QStringList errors;
            for (const Identify::Candidate &c : todo) {
                if (job->isCancelled())
                    break;
                const Identify::ApplyResult r = Identify::apply(library, http, c, a, job);
                changed += r.tags.changed;
                failed += r.tags.failed;
                if (r.cover == "embedded" || r.cover == "saved")
                    ++covers;
                if (!r.error.isEmpty())
                    errors << r.error;
                else if (!r.tags.error.isEmpty())
                    errors << r.tags.error;
            }
            return QJsonObject{{"ok", failed == 0 && errors.isEmpty()}, {"changed", changed}, {"failed", failed},
                               {"covers", covers}, {"albums", int(todo.size())}, {"errors", QJsonArray::fromStringList(errors)}};
        },
        [this](const QJsonObject &r) {
            const int changed = r.value("changed").toInt(), failed = r.value("failed").toInt();
            m_identifyState = "ready";
            m_controller->refreshAll();
            if (failed > 0 || !r.value("errors").toArray().isEmpty()) {
                const QString first = r.value("errors").toArray().first().toString();
                emit message(failed > 0 ? tr("Saved %n files; %1 could not be written", "", changed).arg(failed)
                                        : tr("Couldn't save: %1").arg(first));
            } else if (changed == 0) {
                emit message(tr("Nothing changed: the files already match MusicBrainz"));
            } else {
                emit message(tr("Saved MusicBrainz tags to %n files. Ctrl+Z undoes it.", "", changed));
            }
            emit identifyChanged();
            emit identifyApplied(changed, failed);
        });
}

void WebController::clearIdentify() {
    m_result.reset();
    m_chosen.clear();
    m_included.clear();
    m_changes.clear();
    m_previewErrors.clear();
    m_notes.clear();
    m_identifyState = "idle";
    emit identifyChanged();
}

// ---- Covers -----------------------------------------------------------------

QString WebController::previewDir() const {
    if (!m_previewDir.isEmpty())
        return m_previewDir;
    const QString art = m_controller->artCacheDir().isEmpty() ? ArtworkCache::defaultCacheDir() : m_controller->artCacheDir();
    return art + "/choices";
}

bool WebController::findMissingCovers() {
    if (!m_controller->activate())
        return false;
    Library *lib = m_controller->library();
    CoverArt::AlbumOptions o;
    o.size = lib->setting("coverart.size", 500).toInt();
    o.embed = lib->setting("identify.cover_embed", "1").toString() != "0";
    o.saveFile = lib->setting("identify.cover_file", "0").toString() == "1";
    o.artCacheDir = m_controller->artCacheDir();
    o.sandbox = m_controller->writeSandbox();
    m_coverState = "working";
    emit coverChanged();
    return m_controller->runJob(
        "covers", tr("Finding missing covers"),
        [o](Library &library, Job *job) {
            Net::HttpClient http(&library);
            http.setJob(job);
            const QList<qint64> albums = CoverArt::albumsMissingArt(library);
            int fetched = 0, notFound = 0, failed = 0;
            bool offline = false;
            for (int i = 0; i < albums.size(); ++i) {
                if (job->isCancelled())
                    break;
                job->setProgress(QObject::tr("Finding missing covers"), i, albums.size());
                const CoverArt::AlbumResult r = CoverArt::fetchForAlbum(library, http, albums[i], o, job);
                if (r.status == "fetched" || r.status == "embedded")
                    ++fetched;
                else if (r.status == "offline") {
                    offline = true;
                    break;
                } else if (r.status == "failed")
                    ++failed;
                else
                    ++notFound;
            }
            return QJsonObject{{"ok", !offline}, {"albums", int(albums.size())}, {"fetched", fetched},
                               {"notFound", notFound}, {"failed", failed}, {"offline", offline}};
        },
        [this](const QJsonObject &r) {
            m_coverState = "idle";
            emit coverChanged();
            m_controller->refreshAll();
            const int fetched = r.value("fetched").toInt(), none = r.value("notFound").toInt();
            if (r.value("offline").toBool())
                emit message(tr("Offline: the Cover Art Archive couldn't be reached. Found %n covers before that.", "", fetched));
            else if (r.value("albums").toInt() == 0)
                emit message(tr("Every album already has a cover"));
            else if (none > 0)
                emit message(tr("Found covers for %n albums; %1 had none on the Cover Art Archive", "", fetched).arg(none));
            else
                emit message(tr("Found covers for %n albums", "", fetched));
            emit coversFetched(fetched, none);
        });
}

bool WebController::findCover(qint64 albumId) {
    if (albumId <= 0 || !m_controller->activate())
        return false;
    m_coverAlbum = albumId;
    m_coverCandidates.clear();
    m_coverState = "working";
    {
        QSqlQuery q(m_controller->library()->db());
        q.prepare("SELECT album, coalesce(nullif(album_artist, ''), artist) FROM tracks WHERE album_id = ? LIMIT 1");
        q.addBindValue(albumId);
        m_coverAlbumName = q.exec() && q.next() ? q.value(0).toString() : QString();
    }
    emit coverChanged();
    const QString dir = previewDir();
    return m_controller->runJob(
        "covers", tr("Looking for covers of %1").arg(m_coverAlbumName),
        [albumId, dir](Library &library, Job *job) {
            Net::HttpClient http(&library);
            http.setJob(job);
            QString album, artist;
            int tracks = 0;
            QHash<QString, int> releaseVotes;
            QSqlQuery q(library.db());
            q.prepare("SELECT album, coalesce(nullif(album_artist, ''), artist), coalesce(mb_release_id, '') "
                      "FROM tracks WHERE album_id = ?");
            q.addBindValue(albumId);
            if (q.exec())
                while (q.next()) {
                    album = q.value(0).toString();
                    artist = q.value(1).toString();
                    ++tracks;
                    if (!q.value(2).toString().isEmpty())
                        releaseVotes[q.value(2).toString()]++;
                }
            q.finish();
            struct Found {
                QString release, group, title, artist, date, country, format, how;
            };
            QList<Found> found;
            // The release the tags name, when most tracks agree.
            for (auto it = releaseVotes.cbegin(); it != releaseVotes.cend(); ++it)
                if (it.value() * 2 >= tracks)
                    found.append({it.key(), {}, album, artist, {}, {}, {}, "tags"});
            bool offline = false;
            QString error;
            if (!album.isEmpty()) {
                MusicBrainz::Client mb(http);
                const QList<MusicBrainz::Release> list = mb.searchReleases(artist, album, 0, 10, &error);
                offline = list.isEmpty() && mb.lastWasOffline();
                QSet<QString> groups;
                for (const MusicBrainz::Release &r : list) {
                    if (r.searchScore < 60 || found.size() >= 6)
                        break;
                    // One per release group: editions of an album usually share a cover.
                    if (!r.releaseGroupId.isEmpty() && groups.contains(r.releaseGroupId))
                        continue;
                    groups.insert(r.releaseGroupId);
                    bool dup = false;
                    for (Found &f : found)
                        if (f.release == r.id) {
                            f.group = r.releaseGroupId, f.date = r.date, f.country = r.country, f.format = r.formats();
                            dup = true;
                        }
                    if (!dup)
                        found.append({r.id, r.releaseGroupId, r.title, MusicBrainz::creditText(r.credits), r.date,
                                      r.country, r.formats(), "search"});
                }
            }
            // A small preview of each, saved where the sheet can show it.
            QDir().mkpath(dir);
            QJsonArray candidates;
            for (int i = 0; i < found.size(); ++i) {
                if (job->isCancelled())
                    break;
                job->setProgress(QObject::tr("Fetching previews"), i, found.size());
                const Found &f = found[i];
                const CoverArt::Fetched c = CoverArt::front(http, f.release, 250, f.group);
                if (c.offline) {
                    offline = true;
                    break;
                }
                if (!c.ok)
                    continue;
                const QString file = QString("%1/%2-%3.%4").arg(dir).arg(albumId).arg(f.release, extensionFor(c.mime));
                QSaveFile out(file);
                if (!out.open(QIODevice::WriteOnly) || out.write(c.bytes) != c.bytes.size() || !out.commit())
                    continue;
                candidates.append(QJsonObject{{"release", f.release},   {"group", f.group},
                                              {"title", f.title},       {"artist", f.artist},
                                              {"date", f.date},         {"country", f.country},
                                              {"format", f.format},     {"how", f.how},
                                              {"preview", QUrl::fromLocalFile(file).toString()}});
            }
            return QJsonObject{{"ok", !offline}, {"offline", offline}, {"error", error}, {"candidates", candidates}};
        },
        [this, albumId](const QJsonObject &r) {
            if (albumId != m_coverAlbum)
                return;
            m_coverCandidates = r.value("candidates").toArray().toVariantList();
            if (r.value("offline").toBool() && m_coverCandidates.isEmpty()) {
                m_coverState = "idle";
                emit coverChanged();
                emit message(tr("Offline: MusicBrainz and the Cover Art Archive couldn't be reached"));
                return;
            }
            if (m_coverCandidates.isEmpty()) {
                m_coverState = "idle";
                emit coverChanged();
                emit message(tr("No cover found for %1").arg(m_coverAlbumName));
                return;
            }
            if (m_coverCandidates.size() == 1) {
                chooseCover(0);
                return;
            }
            m_coverState = "choose";
            emit coverChanged();
        });
}

bool WebController::chooseCover(int index) {
    if (index < 0 || index >= m_coverCandidates.size() || m_coverAlbum <= 0)
        return false;
    const QVariantMap c = m_coverCandidates[index].toMap();
    Library *lib = m_controller->library();
    const int size = lib->setting("coverart.size", 500).toInt();
    const bool embed = lib->setting("identify.cover_embed", "1").toString() != "0";
    const bool saveFile = lib->setting("identify.cover_file", "0").toString() == "1";
    const QString artDir = m_controller->artCacheDir().isEmpty() ? ArtworkCache::defaultCacheDir() : m_controller->artCacheDir();
    const QString sandbox = m_controller->writeSandbox();
    const qint64 albumId = m_coverAlbum;
    const QString release = c.value("release").toString(), group = c.value("group").toString();
    m_coverState = "working";
    emit coverChanged();
    return m_controller->runJob(
        "covers", tr("Saving the cover of %1").arg(m_coverAlbumName),
        [=](Library &library, Job *job) {
            Net::HttpClient http(&library);
            http.setJob(job);
            const CoverArt::Fetched f = CoverArt::front(http, release, size, group);
            if (!f.ok)
                return QJsonObject{{"ok", false}, {"offline", f.offline}, {"error", f.error}};
            QDir().mkpath(artDir + "/fetched");
            const QString picture = QString("%1/fetched/%2-%3.%4")
                                        .arg(artDir, f.releaseId.isEmpty() ? "rg-" + f.releaseGroupId : f.releaseId)
                                        .arg(CoverArt::snapSize(size))
                                        .arg(extensionFor(f.mime));
            QSaveFile out(picture);
            if (!out.open(QIODevice::WriteOnly) || out.write(f.bytes) != f.bytes.size() || !out.commit())
                return QJsonObject{{"ok", false}, {"error", QString("Can't save %1").arg(picture)}};
            ArtworkCache cache(library, artDir);
            if (!cache.setAlbumPicture(albumId, picture))
                return QJsonObject{{"ok", false}, {"error", cache.lastError()}};
            QList<qint64> tracks;
            QStringList folders;
            QSqlQuery q(library.db());
            q.prepare("SELECT id FROM tracks WHERE album_id = ? AND availability = 'online'");
            q.addBindValue(albumId);
            if (q.exec())
                while (q.next())
                    tracks << q.value(0).toLongLong();
            q.finish();
            for (qint64 t : std::as_const(tracks)) {
                const QString folder = QFileInfo(library.absolutePath(t)).absolutePath();
                if (!folders.contains(folder))
                    folders << folder;
            }
            QString coverFile;
            if (saveFile && folders.size() == 1) {
                bool has = false;
                for (const QString &n : QDir(folders.first()).entryList(QDir::Files))
                    has = has || QStringList{"cover", "folder", "front"}.contains(QFileInfo(n).completeBaseName().toLower());
                const QString target = folders.first() + "/cover." + extensionFor(f.mime);
                if (!has && Journal::insideSandbox(sandbox, target)) {
                    QSaveFile file(target);
                    if (file.open(QIODevice::WriteOnly) && file.write(f.bytes) == f.bytes.size() && file.commit())
                        coverFile = target;
                }
            }
            QJsonObject result{{"ok", true}, {"picture", picture}, {"coverFile", coverFile}};
            if (embed && !tracks.isEmpty()) {
                QList<TagEdit> edits;
                for (qint64 t : std::as_const(tracks))
                    edits << TagEdit{t, "artwork", {}, f.bytes, f.mime};
                TagWriter writer(library, job);
                writer.setSandbox(sandbox);
                TagWriteOptions o;
                o.source = "fetch-art";
                const TagBatchResult r = writer.apply(edits, o);
                result["embedded"] = r.changed;
                result["failed"] = r.failed;
                if (!r.ok)
                    result["error"] = r.error.isEmpty() ? QString("%1 files could not be written").arg(r.failed) : r.error;
            }
            return result;
        },
        [this](const QJsonObject &r) {
            m_coverState = "idle";
            m_coverCandidates.clear();
            emit coverChanged();
            m_controller->refreshAll();
            if (!r.value("ok").toBool())
                emit message(r.value("offline").toBool() ? tr("Offline: the Cover Art Archive couldn't be reached")
                                                         : tr("Couldn't save the cover: %1").arg(r.value("error").toString()));
            else if (r.value("embedded").toInt() > 0)
                emit message(tr("Cover saved into %n files. Ctrl+Z undoes it.", "", r.value("embedded").toInt()));
            else if (!r.value("error").toString().isEmpty())
                emit message(tr("Cover saved for the album, but %1").arg(r.value("error").toString()));
            else
                emit message(tr("Cover saved for %1").arg(m_coverAlbumName));
            emit coversFetched(1, 0);
        });
}

void WebController::clearCovers() {
    m_coverCandidates.clear();
    if (m_coverState == QLatin1String("choose"))
        m_coverState = "idle";
    emit coverChanged();
}

// ---- Lyrics -----------------------------------------------------------------

void WebController::loadLyrics(qint64 trackId, bool fetch) {
    if (trackId <= 0) {
        m_lyricsWanted = 0;
        if (m_lyricsTrack != 0 || m_lyricsState != QLatin1String("idle")) {
            m_lyricsTrack = 0;
            m_lyricsState = "idle";
            m_lyricsLines.clear();
            m_parsed.reset();
            emit lyricsChanged();
        }
        return;
    }
    if (!m_controller->activate())
        return;
    if (m_lyricsThread) {
        // One look-up at a time; the newest request runs when this one ends.
        m_lyricsWanted = trackId;
        m_lyricsFetchWanted = fetch;
        return;
    }
    if (trackId != m_lyricsTrack) {
        m_lyricsLines.clear();
        m_parsed.reset();
        m_lyricsSource.clear();
        m_lyricsSynced = false;
        m_lyricsInstrumental = false;
    }
    // Reading the same track again keeps its lines on screen meanwhile.
    if (trackId != m_lyricsTrack || m_lyricsState != QLatin1String("found") || fetch)
        m_lyricsState = "loading";
    m_lyricsTrack = trackId;
    m_lyricsWanted = 0;
    m_lyricsError.clear();
    emit lyricsChanged();
    const QString path = m_controller->databasePath();
    m_lyricsThread = QThread::create([this, path, trackId, fetch] {
        QVariantMap entry;
        Library library(path);
        QString why;
        if (!library.open(&why)) {
            entry["error"] = why;
        } else {
            Net::HttpClient http(&library);
            Lyrics::Options o;
            o.fetch = fetch;
            o.refresh = fetch;
            const Lyrics::Entry e = Lyrics::forTrack(library, &http, trackId, o);
            entry = e.toJson().toVariantMap();
            entry["plain"] = e.plain;
            entry["synced"] = e.synced;
            entry["source"] = e.source;
            entry["instrumental"] = e.instrumental;
            entry["error"] = e.error;
            entry["found"] = e.found();
        }
        entry["asked"] = fetch;
        QMetaObject::invokeMethod(this, [this, trackId, entry] { finishLyrics(trackId, entry); }, Qt::QueuedConnection);
    });
    m_lyricsThread->setObjectName("omamp-lyrics");
    m_lyricsThread->start(QThread::LowPriority);
}

void WebController::finishLyrics(qint64 trackId, const QVariantMap &entry) {
    if (m_lyricsThread) {
        m_lyricsThread->wait();
        delete m_lyricsThread;
        m_lyricsThread = nullptr;
    }
    if (trackId == m_lyricsTrack) {
        m_lyricsPlain = entry.value("plain").toString();
        m_lyricsSyncedText = entry.value("synced").toString();
        m_lyricsSource = entry.value("source").toString();
        m_lyricsInstrumental = entry.value("instrumental").toBool();
        const QString error = entry.value("error").toString();
        m_parsed = std::make_unique<Lyrics::Parsed>(
            Lyrics::parseLrc(m_lyricsSyncedText.isEmpty() ? m_lyricsPlain : m_lyricsSyncedText));
        m_lyricsSynced = m_parsed->synced;
        m_lyricsLines.clear();
        for (const Lyrics::Line &l : std::as_const(m_parsed->lines))
            m_lyricsLines << QVariantMap{{"time", l.timeMs}, {"text", l.text}};
        // Only a look-up that was asked for and failed (offline, a server
        // error) is an error; finding nothing is just "none".
        if (entry.value("found").toBool())
            m_lyricsState = "found";
        else if (entry.value("asked").toBool() && m_lyricsSource != QLatin1String("none") && !error.isEmpty())
            m_lyricsState = "error";
        else
            m_lyricsState = "none";
        m_lyricsError = m_lyricsState == QLatin1String("error") ? error : QString();
        emit lyricsChanged();
    }
    if (m_lyricsWanted > 0) {
        const qint64 next = m_lyricsWanted;
        m_lyricsWanted = 0;
        loadLyrics(next, m_lyricsFetchWanted);
    }
}

int WebController::lyricsLineAt(qint64 positionMs) const {
    if (!m_parsed || !m_parsed->synced)
        return -1;
    return m_parsed->lineAt(positionMs);
}

bool WebController::saveLyrics(const QString &mode) {
    if (m_lyricsTrack <= 0 || m_lyricsState != QLatin1String("found"))
        return false;
    if (mode == QLatin1String("lrc") && m_lyricsSyncedText.isEmpty()) {
        emit message(tr("These lyrics have no times, so there is nothing for a .lrc file"));
        return false;
    }
    Lyrics::Entry e;
    e.trackId = m_lyricsTrack;
    e.plain = m_lyricsPlain;
    e.synced = m_lyricsSyncedText;
    // Writing reads the source to skip what is already there; "lrclib"
    // means none of it is on disk yet.
    e.source = "lrclib";
    e.instrumental = m_lyricsInstrumental;
    const bool syncedInTag = mode == QLatin1String("tag") && e.plain.isEmpty();
    const QString sandbox = m_controller->writeSandbox();
    const qint64 track = m_lyricsTrack;
    return m_controller->runJob(
        "lyrics", tr("Saving lyrics"),
        [e, mode, syncedInTag, sandbox](Library &library, Job *job) {
            const Lyrics::WriteResult r = Lyrics::write(library, {e}, mode, syncedInTag, false, job, sandbox);
            QJsonObject o = r.tags.toJson();
            o["skipped"] = int(r.skipped.size());
            if (!r.error.isEmpty())
                o["error"] = r.error;
            return o;
        },
        [this, mode, track](const QJsonObject &r) {
            m_controller->refreshAll();
            if (!r.value("error").toString().isEmpty() || r.value("failed").toInt() > 0)
                emit message(tr("Couldn't save the lyrics: %1").arg(r.value("error").toString()));
            else if (r.value("changed").toInt() == 0)
                emit message(tr("The lyrics were already saved"));
            else
                emit message(mode == QLatin1String("lrc") ? tr("Lyrics saved as a .lrc file. Ctrl+Z undoes it.")
                                                          : tr("Lyrics saved into the file. Ctrl+Z undoes it."));
            if (track == m_lyricsTrack)
                loadLyrics(track, false);
        });
}
