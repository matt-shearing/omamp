#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

class Library;

// Play statistics: recording plays and skips, and asking what was played.
namespace Stats {

// When a play counts, the same rule scrobblers use: the listener heard at
// least half the track, or at least four minutes of it. A play that ends
// before a tenth of the track, or that the listener skipped, counts as a
// skip. Anything between (stopped at 30 % without skipping) changes nothing.
struct Rules {
    double countFraction = 0.5;
    qint64 countMs = 4 * 60 * 1000;
    double skipFraction = 0.1;
};

enum class Outcome { Counted, Skipped, Ignored };

struct PlayResult {
    Outcome outcome = Outcome::Ignored;
    qint64 historyId = 0; // the play_history row, when one was written
    QString error;
    bool ok() const { return error.isEmpty(); }
    QString outcomeName() const;
    QJsonObject toJson() const;
};

// Records the end of one playback of a track. playedMs is how much was
// heard (not the position: a seek forward does not count as listening).
// durationMs <= 0 reads the track's length from the library. userSkipped is
// true when the listener pressed Next or picked another track. at is the time
// it ended, in Unix seconds; 0 means now.
//
// Counted: play_count + 1, last_played = at, a play_history row.
// Skipped: skip_count + 1, a play_history row with skipped = 1.
PlayResult recordPlay(Library &library, qint64 trackId, qint64 playedMs, qint64 durationMs = 0,
                      bool userSkipped = false, qint64 at = 0, const Rules &rules = {});
// The rule alone, for the player to show or test.
Outcome classify(qint64 playedMs, qint64 durationMs, bool userSkipped, const Rules &rules = {});

enum class By { Tracks, Artists, Albums, Years, Genres };
bool parseBy(const QString &text, By *out);
QString byName(By by);

struct Row {
    qint64 id = 0;    // track, artist, album or genre id; the year for years
    QString name;     // title, artist, album or genre name; "1997" for years
    QString detail;   // a track's artist, an album's artist
    qint64 plays = 0;
    qint64 skips = 0;
    qint64 listenedMs = 0; // with since: what play_history says was heard
    qint64 tracks = 0;     // distinct tracks behind the row
    qint64 lastPlayed = 0;
    QJsonObject toJson() const;
};

struct TopOptions {
    By by = By::Tracks;
    // Unix seconds. 0 means all time, counted from the tracks' play counts
    // (which include counts imported from MediaMonkey or read from tags);
    // otherwise counted from play_history since then.
    qint64 since = 0;
    int limit = 25;
};

QList<Row> top(Library &library, const TopOptions &options, QString *error = nullptr);

struct Played {
    qint64 trackId = 0;
    QString title, artist, album;
    qint64 playedAt = 0; // Unix seconds
    qint64 playedMs = 0;
    bool skipped = false;
    QJsonObject toJson() const;
};

// The newest plays first (skips left out unless withSkips).
QList<Played> recent(Library &library, int limit = 25, bool withSkips = false);
// Tracks never played, newest additions first.
QList<qint64> neverPlayed(Library &library, int limit = 25);

struct Summary {
    qint64 plays = 0, skips = 0, listenedMs = 0, tracks = 0, neverPlayed = 0;
    QJsonObject toJson() const;
};
// Totals since a time (0: all of play_history).
Summary summary(Library &library, qint64 since = 0);

// "30d", "2w", "6m", "1y", "12h", or a date "2026-01-01", to Unix seconds
// before `now` (0 means the current time). Returns -1 when it can't parse.
qint64 parseSince(const QString &text, qint64 now = 0);

}
