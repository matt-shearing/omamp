#pragma once

#include "http.h"
#include "musicbrainz.h"
#include "tagwriter.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class Job;
class Library;

// Identifying tracks against MusicBrainz, the way Picard does it: cluster,
// look up, review, save.
//
// 1. cluster() groups the chosen tracks into would-be albums by their album
//    and album artist tags and their folder.
// 2. Identifier::identify() finds release candidates for each cluster from
//    (a) MusicBrainz IDs already in the tags, (b) AcoustID fingerprints
//    (when there is an application key and fpcalc), and (c) a MusicBrainz
//    search by album, artist and track count. It looks each candidate up in
//    full and scores it: track count, track titles and lengths, album
//    title, artist, date, and the country and format the user prefers.
//    Each candidate carries a file-to-track mapping with a score per file.
// 3. editsFor() turns a candidate into TagEdits; apply() writes them through
//    the TagWriter (journalled and undoable, with a dry run first) and adds
//    the Cover Art Archive's front cover.
namespace Identify {

struct File {
    qint64 trackId = 0;
    QString path, title, artist, album, albumArtist;
    int track = 0, disc = 0, year = 0;
    qint64 durationMs = 0;
    QString mbRecordingId, mbReleaseId;
    qint64 folderId = 0;
    QString folder;
    bool online = false;
    QJsonObject toJson() const;
};

struct Cluster {
    QString key, album, albumArtist, artist, folder;
    int year = 0;
    QList<File> files;
    QJsonObject toJson() const;
};

// Groups tracks by album (ignoring case and accents), album artist (or the
// artist when there is none) and folder. Tracks with no album are grouped
// by folder and artist, as Picard's "unclustered files" would be.
QList<Cluster> cluster(Library &library, const QList<qint64> &trackIds);

struct TrackMatch {
    qint64 trackId = 0;
    int disc = 0;      // the medium's position
    int discTracks = 0; // tracks on that medium
    MusicBrainz::Track track;
    double score = 0;
    QString via; // "mbid", "acoustid" or "tags"
    QJsonObject toJson() const;
};

struct Candidate {
    MusicBrainz::Release release;
    double score = 0;          // 0 to 1
    QStringList sources;       // mbid, acoustid, search, chosen
    QList<TrackMatch> mapping; // in the cluster's file order
    QList<qint64> unmatched;   // files no release track fits
    QMap<QString, double> parts; // each part of the score, for the review dialog
    QString confidence() const; // high (0.85+), medium (0.65+) or low
    QJsonObject toJson(bool withTracks = true) const;
};

struct ClusterResult {
    Cluster cluster;
    QList<Candidate> candidates; // best first
    QStringList notes;
    QString error;
    const Candidate *best() const { return candidates.isEmpty() ? nullptr : &candidates.first(); }
    QJsonObject toJson() const;
};

struct Options {
    bool useMbids = true;
    bool useAcoustId = true;
    bool useSearch = true;
    int maxCandidates = 5; // full release look-ups per cluster, one a second
    int maxFingerprints = 40;
    QString fpcalc; // empty: on PATH
    // Preferences, best first: "GB", "XW"; "Digital Media", "CD".
    QStringList countries, formats;
    QString forceRelease; // look up only this release (the review's "use this one")
    // Reads identify.countries and identify.formats (comma separated).
    static Options load(const Library &library);
};

struct Result {
    QList<ClusterResult> clusters;
    QStringList notes; // once per run: no AcoustID key, no fpcalc, offline
    bool acoustIdUsed = false;
    bool offline = false;
    QJsonObject toJson() const;
};

class Identifier {
public:
    Identifier(Library &library, Net::HttpClient &http, Job *job = nullptr);

    Result identify(const QList<qint64> &trackIds, const Options &options);
    ClusterResult identifyCluster(const Cluster &cluster, const Options &options, Result *run = nullptr);

    // Scores one release against a cluster. acoustid maps a track id to the
    // recordings AcoustID matched for it, with their scores.
    static Candidate score(const Cluster &cluster, const MusicBrainz::Release &release, const Options &options,
                           const QHash<qint64, QMap<QString, double>> &acoustid = {});

private:
    Library &m_library;
    Net::HttpClient &m_http;
    Job *m_job;
};

struct ApplyOptions {
    bool dryRun = false;
    bool writeGenres = false; // MusicBrainz's genres, the three with most votes
    bool writeLabel = true;   // label and catalogue number
    bool coverEmbed = true;   // the Cover Art Archive's front cover, into the files
    bool coverFile = false;   // ... and as cover.jpg when the folder has no picture
    int coverSize = 500;      // 250, 500, 1200 or 0 for the original
    QString sandbox;          // tests: never write outside this folder
    QString artCacheDir;      // empty: ~/.cache/omamp/art
    // Reads identify.write_genres, identify.write_label, identify.cover_embed,
    // identify.cover_file and identify.cover_size.
    static ApplyOptions load(const Library &library);
};

// The tag edits a candidate makes (without the cover).
QList<TagEdit> editsFor(const Candidate &candidate, const ApplyOptions &options);

struct ApplyResult {
    TagBatchResult tags;
    QString cover;     // embedded, saved, not-found, skipped, offline, failed, or "" when not asked
    QString coverFile; // cover.jpg written
    QString error;
    QJsonObject toJson() const;
};

ApplyResult apply(Library &library, Net::HttpClient &http, const Candidate &candidate, const ApplyOptions &options,
                  Job *job = nullptr);

}
