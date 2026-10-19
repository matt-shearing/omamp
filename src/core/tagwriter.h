#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>

class Job;
class Library;

// One field to set on one track. value is text: "A; B" for several
// artists or genres, a number for years and track numbers, 0 to 100 for a
// rating (20 per star), and empty to clear the field. For "artwork",
// picture holds the image bytes (empty removes every embedded picture).
struct TagEdit {
    qint64 trackId = 0;
    QString field;
    QString value;
    QByteArray picture;
    QString pictureMime; // guessed from the bytes when empty
};

struct TagFieldChange {
    QString field, oldValue, newValue;
};

struct TagFileResult {
    qint64 trackId = 0;
    QString path;
    bool ok = true;
    bool fileWritten = false; // false for a dry run, an unchanged track or database-only fields
    QString error;
    QList<TagFieldChange> changes; // only fields whose value really changes
    QJsonObject toJson() const;
};

struct TagBatchResult {
    QString batch; // the journal batch; empty for a dry run or when nothing changed
    bool ok = true; // false if any track failed
    bool dryRun = false;
    bool cancelled = false;
    QString error;  // a failure of the whole batch (bad field name, closed library)
    int tracks = 0, changed = 0, unchanged = 0, failed = 0;
    QList<TagFileResult> files;
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};

struct TagWriteOptions {
    bool dryRun = false;
    // The journal's source column: "edit", "tag-from-filename", "undo:<batch>".
    QString source = "edit";
    // -1 reads the library setting tags.write_ratings (default off): ratings
    // then live only in the database, so rating a song never rewrites a file
    // on the NAS.
    int writeRatingsToFiles = -1;
    // The POPM e-mail written with MP3 ratings. Empty reads the setting
    // tags.popm_email, default "no@email", MediaMonkey's.
    QString popmEmail;
};

// Writes tags with TagLib, safely:
//
// 1. Copy the file to a hidden temporary file beside it.
// 2. Write the tags into the copy, then read the copy back and check every
//    field and the audio's length.
// 3. Rename the copy over the original, which is atomic, so a crash or a
//    dropped NAS connection leaves either the old file or the new one,
//    never half of each. The original is left alone if anything changed it
//    meanwhile.
// 4. In one transaction, journal every change (tag_journal: batch, track,
//    field, old, new, time) and update the track's row with the file's new
//    tags, size and modification time, so a rescan sees nothing foreign.
//
// Each file stands alone: a failure is reported for that file and the rest
// of the batch carries on. undo() restores a batch's old values in the
// files and the database and is journalled itself, so it can be undone.
//
// Runs on the calling thread; the window runs it on a worker thread with its
// own Library. Progress and cancel go through the Job.
class TagWriter {
public:
    explicit TagWriter(Library &library, Job *job = nullptr);

    // The fields this writer knows, canonical names first.
    static const QStringList &fields();
    // "Album Artist", "album_artist", "tracknumber" and so on, to the canonical
    // name; empty if unknown.
    static QString canonicalField(const QString &name);
    // Fields kept only in the database (custom1..5, and rating unless file
    // writes are on).
    static bool isDatabaseOnly(const QString &field);

    TagBatchResult apply(const QList<TagEdit> &edits, const TagWriteOptions &options = {});
    TagBatchResult undo(const QString &batch);

    // What a file says now, for every field this writer knows. Multi-value
    // fields are joined with "; ". "artwork" is a JSON list of the pictures
    // (type, mime, description, base64 data), or empty.
    static QMap<QString, QString> readFile(const QString &path, QString *error = nullptr);
    // The values an editor shows: the file's (when online) plus the
    // database-only fields and the library's rating.
    QMap<QString, QString> currentValues(qint64 trackId, QString *error = nullptr);

    // Refuses to touch any file outside this folder. Tests set it; the
    // window and CLI don't need it.
    void setSandbox(const QString &folder) { m_sandbox = folder; }
    // Test seam: called after the copy is written and verified, just before
    // it replaces the original. Returning false fails that file there, as a
    // crash or a lost connection would.
    void setBeforeReplace(std::function<bool(const QString &temporary, const QString &target)> hook) {
        m_beforeReplace = std::move(hook);
    }

    // "tag-20261020-231500-a1b2": kind, UTC time and four random hex digits.
    static QString newBatchId(const QString &kind);

private:
    TagFileResult writeOne(qint64 trackId, const QList<TagEdit> &edits, const TagWriteOptions &options,
                           const QString &batch, bool writeRatings, const QString &popmEmail);

    Library &m_library;
    Job *m_job;
    QString m_sandbox;
    std::function<bool(const QString &, const QString &)> m_beforeReplace;
};

// The undo journal, shared by tag edits and organise.
namespace Journal {

struct Entry {
    qint64 id = 0;
    QString batch;
    qint64 trackId = 0; // 0 when the track was deleted, or for a companion file or folder
    QString field, oldValue, newValue, source;
    qint64 changedAt = 0;
    bool undone = false;
    QJsonObject toJson() const;
};

struct Batch {
    QString id, source;
    qint64 changedAt = 0;
    int entries = 0, tracks = 0, undoneEntries = 0;
    QString kind() const; // "tag", "organise" or "undo"
    bool undone() const { return entries > 0 && undoneEntries == entries; }
    QJsonObject toJson() const;
};

// Newest first.
QList<Batch> batches(Library &library, int limit = 50);
QList<Entry> entries(Library &library, const QString &batch);
// The newest batch that is not an undo and not undone yet, or empty.
QString lastUndoable(Library &library);
// True when a batch moved files (organise) rather than changing tags.
bool isOrganise(Library &library, const QString &batch);

// A row in the jobs table for a long piece of work, as scans have.
qint64 startJobRow(Library &library, const QString &kind, const QStringList &args);
void finishJobRow(Library &library, qint64 jobId, bool ok, bool cancelled, const QJsonObject &result,
                  const QString &error);

// True when path is inside folder (or folder is empty, meaning no limit).
bool insideSandbox(const QString &folder, const QString &path);

}
