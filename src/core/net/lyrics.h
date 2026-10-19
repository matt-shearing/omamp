#pragma once

#include "http.h"
#include "tagwriter.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

class Job;
class Library;

// Lyrics: the LRC format, the lyrics already on disk (a .lrc beside the
// track, SYLT or USLT in ID3v2, LYRICS in Vorbis comments and MP4), LRCLIB
// look-ups, the lyrics cache table, and writing lyrics back to files.
namespace Lyrics {

struct Line {
    qint64 timeMs = -1; // -1 for an untimed line
    QString text;
};

// A parsed LRC text. For a scrolling lyrics view: lines in time order,
// lineAt(position) the one to highlight.
struct Parsed {
    QList<Line> lines;
    QMap<QString, QString> tags; // ar, ti, al, by, length, offset...
    qint64 offsetMs = 0;         // already applied to every line's time
    bool synced = false;         // at least one timed line

    // The line playing at a position: the last line starting at or before
    // it, or -1 before the first.
    int lineAt(qint64 positionMs) const;
    QString plainText() const; // the words alone, one line each
    bool isEmpty() const { return lines.isEmpty(); }
};

// Reads [mm:ss.xx], [mm:ss.xxx], [mm:ss:xx] and [mm:ss] stamps, several on
// one line, [offset:±ms], ID tags, and A2 "enhanced" word stamps
// (<mm:ss.xx>, which are dropped). Text with no stamps at all comes back as
// untimed lines with synced false.
Parsed parseLrc(const QString &text);
// "[01:02.34]text" lines, the ID tags first.
QString toLrc(const Parsed &parsed);
QString formatStamp(qint64 ms);

struct Entry {
    qint64 trackId = 0;
    QString plain, synced; // synced is LRC text
    // sidecar, embedded, lrclib; "none" when LRCLIB had nothing
    QString source;
    qint64 lrclibId = 0;
    bool instrumental = false;
    qint64 fetched = 0;
    QString error; // why there is nothing, when there isn't
    bool found() const { return !plain.isEmpty() || !synced.isEmpty() || instrumental; }
    Parsed parsed() const { return parseLrc(synced.isEmpty() ? plain : synced); }
    QJsonObject toJson() const;
};

// The .lrc file beside a track: same name, .lrc extension.
QString sidecarPath(const QString &audioPath);
// Lyrics already on disk: the sidecar first, then the tags (SYLT, then
// USLT or LYRICS). Empty source when there are none.
Entry local(const QString &audioPath);

// The lyrics cache table.
Entry cached(Library &library, qint64 trackId);
bool store(Library &library, const Entry &entry);

struct Query {
    QString artist, title, album;
    qint64 durationMs = 0;
};
Query queryFor(Library &library, qint64 trackId);

// LRCLIB: /api/get by artist, title, album and length, then /api/search
// when that finds nothing, keeping the closest match by length and words.
Entry fetchLrclib(Net::HttpClient &http, const Query &query);

struct Options {
    bool fetch = false;         // ask LRCLIB when nothing is on disk or cached
    bool refresh = false;       // ask again even if the cache has an answer
    int retryNotFoundDays = 30; // a "none" answer older than this is asked again
};
// What a lyrics view shows for a track: on disk, else the cache, else
// (with fetch) LRCLIB, whose answer is cached.
Entry forTrack(Library &library, Net::HttpClient *http, qint64 trackId, const Options &options = {});

// Writes lyrics into files through the TagWriter, so they are journalled
// and undoable: "lrc" writes the synced lyrics to the .lrc sidecar, "tag"
// writes the plain lyrics to USLT / LYRICS (or the LRC text with
// syncedInTag). Tracks without suitable lyrics are left out and listed in
// skipped.
struct WriteResult {
    TagBatchResult tags;
    QList<qint64> skipped;
    QString error;
};
WriteResult write(Library &library, const QList<Entry> &entries, const QString &mode, bool syncedInTag = false,
                  bool dryRun = false, Job *job = nullptr, const QString &sandbox = {});

}

