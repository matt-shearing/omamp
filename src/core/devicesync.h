#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class Job;
class Library;

// Device sync: copies a playlist, auto-playlist, collection or query to a
// folder, a USB drive or a Syncthing folder, converting the formats a
// profile says to convert with the `ffmpeg` command.
//
// A profile names where the music goes, what goes there, how files are named
// (a mask, docs/masks.md), the format rules, and whether files Thrum didn't
// put there are deleted. Profiles live in the library's settings table
// (`sync.profiles`), so `--db` picks them as it picks everything else.
//
// A sync is two steps. plan() reads the library and the device and says what
// would happen, file by file: copy, convert, keep (already there and
// unchanged), or delete. That is the dry run, and it writes nothing. apply()
// does what a plan says.
//
// The device keeps a manifest, `.thrum-sync.json`, at the top of the target:
// each file Thrum wrote, the source it came from (path, size, modification
// time) and the rule it was made with. A file whose source and rule are
// unchanged is kept, so a second sync of the same list copies nothing. A
// file Thrum wrote earlier that is no longer in the list is deleted. Other
// files on the device are left alone unless the profile says "delete other
// files". Hidden files and folders (.stfolder, .stignore, .Trashes…) are
// never touched.
namespace DeviceSync {

struct Source {
    QString kind; // playlist (static or auto), collection, query, library
    QString ref;  // a playlist or collection id, name or path; query text
    QString toString() const; // "playlist:Road trip", "query:rating:5"
    static Source fromString(const QString &text, QString *error = nullptr);
};

// Files whose format (or extension) is in `from` are copied, converted to
// `to` at `bitrate` kbit/s, or left off the device. The first rule that
// matches wins; a file no rule matches is copied.
struct FormatRule {
    QStringList from; // flac, wav, mp3, m4a, ogg, opus, aiff, wma, alac, ape, … or "lossless" or "*"
    QString action = "convert"; // copy, convert, skip
    QString to;       // opus, mp3, aac, vorbis, flac
    int bitrate = 0;  // kbit/s; 0 picks the codec's default
    QString toString() const; // "flac,wav->opus:128", "wma->skip", "*->copy"
    static FormatRule fromString(const QString &text, QString *error = nullptr);
    QString signature() const; // what the manifest remembers: "copy" or "opus:128"
};

struct Profile {
    qint64 id = 0;
    QString name;
    QString kind = "folder"; // folder, usb, syncthing
    QString target;          // absolute folder
    QList<Source> sources;
    QString mask;            // empty: the default mask
    QList<FormatRule> rules;
    bool deleteOthers = false;
    bool writePlaylists = true; // an .m3u8 per playlist source, beside the music
    qint64 lastSync = 0;        // Unix seconds
    QJsonObject lastResult;

    QJsonObject toJson() const;
    static Profile fromJson(const QJsonObject &o);
};

extern const char *const defaultMask;
extern const char *const manifestName;

// Every profile, in the order they were made.
QList<Profile> list(Library &library);
// By id or name (ignoring case). Sets error when nothing fits.
Profile find(Library &library, const QString &idOrName, QString *error = nullptr);
// Creates (id 0) or replaces a profile after checking it; returns its id or
// 0 with error set.
qint64 save(Library &library, Profile profile, QString *error = nullptr);
bool remove(Library &library, qint64 id, QString *error = nullptr);
// Records a run's result and time on the profile.
void recordRun(Library &library, qint64 id, const QJsonObject &result);

// Whether the target can be written now. state is one of: ready, missing
// (a folder that will be made), not-connected (a USB drive that isn't
// plugged in), not-syncthing (no .stfolder), inside-library, read-only.
struct TargetState {
    bool ready = false;
    QString state, message;
    qint64 freeBytes = -1, totalBytes = -1;
    QString mountPoint;
    QJsonObject toJson() const;
};
TargetState checkTarget(Library &library, const Profile &profile);

// USB drives that are plugged in and Syncthing folders that are set up, so
// the window and `thrum sync devices` can offer them as targets.
struct Device {
    QString kind; // usb, syncthing
    QString path, label;
    qint64 freeBytes = -1, totalBytes = -1;
    QJsonObject toJson() const;
};
QList<Device> detectDevices();

struct Action {
    QString op;      // copy, convert, keep, delete, skip
    qint64 trackId = 0;
    QString source;  // absolute path in the library (empty for delete)
    QString target;  // path relative to the profile's target
    QString rule;    // the rule signature: copy, opus:128
    qint64 bytes = 0; // copy and convert: what will be written (an estimate for convert); delete: freed
    QString reason;  // keep: "unchanged"; copy: "new", "changed", "renamed"; delete: "no longer in the list",
                     // "not from this library"; skip: "offline", "missing", "rule"
    QJsonObject toJson() const;
};

struct Plan {
    Profile profile;
    TargetState target;
    QList<Action> actions;
    QStringList playlists; // .m3u8 files that will be written, relative to the target
    QHash<QString, QStringList> playlistEntries; // each playlist file's entries, in order
    int copies = 0, conversions = 0, kept = 0, deletions = 0, skipped = 0;
    qint64 bytesToWrite = 0, bytesToFree = 0;
    bool fits = true; // what will be written fits in the free space
    QString error;
    QStringList warnings;
    bool ok() const { return error.isEmpty(); }
    // limit: at most this many actions in "actions" (0: all); keeps are left
    // out unless withKept.
    QJsonObject toJson(int limit = 0, bool withKept = false) const;
};

// The tracks a profile syncs, in order, each once (before format rules and
// offline tracks are taken out). For the Devices node in the tree.
QList<qint64> trackIds(Library &library, const Profile &profile, QString *error = nullptr);

// The dry run: what a sync would do. Reads the library, the target folder
// and the manifest; writes nothing.
Plan plan(Library &library, const Profile &profile, Job *job = nullptr);

struct Result {
    bool ok = false, cancelled = false;
    QString error;
    int copied = 0, converted = 0, kept = 0, deleted = 0, skipped = 0, failed = 0;
    QStringList failures; // "relative/path: why"
    QStringList playlists;
    qint64 bytesWritten = 0;
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};

// Does what the plan says. Conversions run `workers` at a time (0: up to
// four). Refuses a plan that doesn't fit or whose target isn't ready.
Result apply(Library &library, const Plan &plan, Job *job = nullptr, int workers = 0);

// The ffmpeg command (default "ffmpeg" on PATH); tests point it elsewhere.
void setFfmpegProgram(const QString &program);
QString ffmpegProgram();
// Whether a path is on a removable or mounted drive rather than the system
// disk; tests replace it.
void setMountCheck(std::function<bool(const QString &path, QString *mountPoint)> check);
// Where detectDevices() looks; tests replace them.
void setDeviceRoots(const QStringList &mediaRoots, const QStringList &syncthingConfigs);

// The codec's file extension: opus -> opus, mp3 -> mp3, aac -> m4a, vorbis -> ogg, flac -> flac.
QString extensionFor(const QString &codec);

}
