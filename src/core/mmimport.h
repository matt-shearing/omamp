#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class Job;
class Library;

// Importing a MediaMonkey library: ratings, play counts, last played, date
// added, skip counts, custom fields, play history, playlists and
// AutoPlaylists, from MediaMonkey 4's MM.DB or MediaMonkey 5's MM5.DB.
//
// The database is never opened in place. It is copied to a temporary folder
// and the copy is read with the SQLite C library, which can register the
// IUNICODE and NUMERICSTRING collations MediaMonkey declares on its columns
// (any other unknown collation gets a case-insensitive stand-in). Only the
// columns that exist are read, so MM4 and MM5 both work, and a column this
// importer expects but doesn't find is reported rather than fatal.
//
// MediaMonkey runs on Windows, so its paths are mapped to this machine's:
// each --map 'D:\Music=/mnt/nas/music' rule swaps a drive or UNC prefix
// (case and slash direction ignored) for a Linux folder. A track that maps to
// no library path is matched by its last folders and file name, then by
// artist, title and length.
//
// run() with apply = false is a dry run: it reports what would change and
// writes nothing. With apply = true every change is made in one transaction
// and journalled in import_journal under one batch id, so undo(batch) puts
// everything back. Files are never touched.
namespace MmImport {

struct PathMap {
    QString from; // a Windows prefix: "D:\Music", "\\nas\music", "D:"
    QString to;   // a Linux folder: "/mnt/nas/music"
    static bool parse(const QString &text, PathMap *out); // "from=to"
};

struct Options {
    QList<PathMap> maps;
    bool apply = false;
    // Overwrite Omamp's values. Off: a rating or custom field is filled only
    // where Omamp has none; counts and dates always merge (the larger count,
    // the later last play, the earlier date added), so importing twice
    // changes nothing the second time.
    bool overwrite = false;
    bool ratings = true, playCounts = true, skips = true, lastPlayed = true, dateAdded = true, custom = true;
    bool history = true;
    bool playlists = true;
    bool matchBySuffix = true; // last folders + file name when no map fits
    bool matchByTags = true;   // artist + title + length as a last resort
    qint64 durationToleranceMs = 3000;
    // Where imported playlists go: a folder playlist with this name.
    QString playlistFolder = "MediaMonkey";
    int sampleLimit = 25; // unmatched paths and changes listed in the report
};

struct FieldChange {
    QString field, oldValue, newValue;
};

struct TrackChange {
    qint64 mmId = 0, trackId = 0;
    QString mmPath, how; // how: path, suffix, tags
    QList<FieldChange> changes;
    QJsonObject toJson() const;
};

struct PlaylistReport {
    qint64 mmId = 0;
    QString name, parent;
    // static, auto (translated to a query), snapshot (an AutoPlaylist that
    // could not be translated, imported as the tracks MediaMonkey listed for
    // it, possibly none), folder (no tracks, only children)
    QString kind;
    QString query;    // the translation, for auto
    QString criteria; // MediaMonkey's own criteria, for auto and snapshot
    QString reason;   // why a snapshot could not be translated
    int entries = 0, matched = 0;
    qint64 playlistId = 0; // after apply
    QJsonObject toJson() const;
};

struct Report {
    bool ok = true;
    QString error;
    bool applied = false;
    QString batch;
    QString database, schema; // schema: MM4, MM5 or unknown
    QStringList missingColumns; // expected but absent, e.g. "Songs.SkipCount"
    QStringList notes;
    int mmTracks = 0;
    int matchedByPath = 0, matchedBySuffix = 0, matchedByTags = 0, unmatched = 0;
    int tracksChanged = 0, fieldChanges = 0;
    QHash<QString, int> changesByField;
    int historyRows = 0, historyDuplicates = 0;
    QList<PlaylistReport> playlists;
    QStringList unmatchedSample;
    QList<TrackChange> changeSample;
    // Suggested --map rules, guessed from tracks matched by their file names.
    QStringList suggestedMaps;
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};

Report run(Library &library, const QString &mmDatabase, const Options &options, Job *job = nullptr);

struct UndoResult {
    bool ok = true;
    QString error;
    QString batch;
    int tracks = 0, fields = 0, playlists = 0, historyRows = 0;
    QJsonObject toJson() const;
};
UndoResult undo(Library &library, const QString &batch);
// The newest import batch not yet undone, or empty.
QString lastBatch(Library &library);
bool isImportBatch(const QString &batch);

// Exposed for tests.
QString mapPath(const QString &windowsPath, const QList<PathMap> &maps, bool *mapped = nullptr);
// An OLE Automation date (days since 30 December 1899) to Unix seconds,
// reading it as UTC; 0 for empty or impossible values.
qint64 oleToUnix(double ole);
// Translates MediaMonkey AutoPlaylist criteria to the query language, or
// returns empty and says why.
QString translateCriteria(const QString &criteria, QString *why);

}
