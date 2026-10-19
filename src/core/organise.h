#pragma once

#include "mask.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

class Job;
class Library;

struct OrganiseOptions {
    // Where each file goes, without its extension, relative to the
    // destination: "<Album Artist>/<Album>/<Track#:2> <Title>".
    QString mask;
    // An absolute folder on a library volume. Empty means each track's own
    // library folder.
    QString destination;
    Mask::Sanitise sanitise;
    // Move cover pictures, cue sheets and logs with a folder whose every
    // track moves to one new folder; a track's own .lrc always follows it.
    bool moveCompanions = true;
    QStringList companionExtensions = {"jpg", "jpeg", "png", "gif", "webp", "bmp", "cue", "log",
                                       "nfo", "txt", "pdf", "m3u", "m3u8", "lrc"};
    // Delete folders the move leaves empty, up to (not including) the
    // library folder.
    bool removeEmptyFolders = true;
};

// One file to move, extra files that go with it, or a folder to delete.
struct CompanionMove {
    QString from, to;
    qint64 trackId = 0; // the track a .lrc belongs to; 0 for a folder's files
    bool skip = false;  // left behind: something already has its new name
    QString note;
    QJsonObject toJson() const;
};

struct OrganiseItem {
    qint64 trackId = 0;
    QString oldPath, newPath;
    // Planned: move, renamed (moves under a "(2)" name because the first
    // choice was taken), unchanged, offline, missing, error.
    // After apply: moved, unchanged, failed, skipped (not reached after a cancel).
    QString status;
    QString note;
    QList<CompanionMove> companions; // the track's .lrc
    bool moves() const { return status == "move" || status == "renamed"; }
    QJsonObject toJson() const;
};

struct OrganisePlan {
    bool ok = true;
    QString error;
    int errorPosition = -1; // in the mask
    QString mask, destination;
    QList<OrganiseItem> items;
    QList<CompanionMove> folderCompanions;
    int moves = 0, renamed = 0, unchanged = 0, problems = 0;
    QJsonObject toJson() const;
};

struct OrganiseResult {
    bool ok = true;
    QString error;
    QString batch; // the journal batch, for undo
    bool cancelled = false;
    int moved = 0, unchanged = 0, failed = 0, copied = 0; // copied: moved across filesystems
    int companionsMoved = 0, foldersRemoved = 0;
    QList<OrganiseItem> items;
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};

// Renames and moves files by a mask, MediaMonkey's Auto-Organize with a
// preview and undo.
//
// plan() works out every new path without touching anything: names that
// clash, with each other or with files already there, get " (2)", " (3)".
// The window shows the plan, the user may change any new path, and apply()
// carries it out as one job:
//
// - Within a filesystem a move is a rename that refuses to replace anything
//   (renameat2 RENAME_NOREPLACE, or link and unlink where that isn't
//   supported). Across filesystems the file is copied beside its new place,
//   checked byte for byte, given its new name the same way, and only then
//   is the original deleted.
// - An existing file is never overwritten.
// - Each move updates the track's row (its id stays, so ratings, history and
//   playlists follow) and journals the old and new place in the same
//   transaction. The journal is the rollback log; undo() moves everything
//   back, recreating folders that were removed.
class Organiser {
public:
    explicit Organiser(Library &library, Job *job = nullptr);

    OrganisePlan plan(const QList<qint64> &trackIds, const OrganiseOptions &options);
    OrganiseResult apply(const OrganisePlan &plan, const OrganiseOptions &options);
    OrganiseResult undo(const QString &batch);

    // Refuses to touch anything outside this folder. Tests set it.
    void setSandbox(const QString &folder) { m_sandbox = folder; }
    // Test seam: decides whether a move crosses filesystems, so the copy,
    // check and delete path can be tested on one disk.
    void setCrossDevice(std::function<bool(const QString &from, const QString &toFolder)> test) {
        m_crossDevice = std::move(test);
    }

private:
    bool moveFile(const QString &from, const QString &to, bool *copied, QString *error);
    bool removeEmptyFolders(const QStringList &folders, const QString &batch, const QString &source,
                            int *removed);
    bool updatePath(qint64 trackId, const QString &newPath, QString *oldKey, QString *newKey, QString *error);

    Library &m_library;
    Job *m_job;
    QString m_sandbox;
    std::function<bool(const QString &, const QString &)> m_crossDevice;
};

// Undoes any journal batch: an organise moves files back, a tag edit
// restores old tags. The result is the TagBatchResult or OrganiseResult as
// JSON, with "kind" set to "tag" or "organise".
QJsonObject undoBatch(Library &library, const QString &batch, Job *job = nullptr);
