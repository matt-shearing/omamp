#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>

class Library;

// Auto-DJ: keeps a number of upcoming tracks in the queue, drawn from a
// source, without repeating an artist too soon or a track heard lately.
namespace AutoDj {

struct Source {
    enum Kind { Library, Collection, Playlist, Query };
    Kind kind = Library;
    QString ref; // collection or playlist id or name, or the query text

    // "library", "collection:Music", "playlist:12", "playlist:Road trips/Summer",
    // "query:rating:4.. genre:jazz". A bare text that is none of these is a query.
    static Source parse(const QString &text);
    QString toString() const;
};

struct Settings {
    Source source;
    int upcoming = 10;           // keep this many tracks queued after the current one
    int artistSeparation = 5;    // no artist twice within this many tracks
    int skipPlayedWithinDays = 7; // leave out tracks played in the last X days (0: don't)
    bool weightByRating = true;  // five stars come up about three times as often as unrated
    double neverPlayedBoost = 2.0; // never-played tracks are this much more likely (1: no boost)
    bool onlineOnly = true;      // leave out offline and missing tracks
    // Content types Auto-DJ never picks, as MediaMonkey's per-type rule does.
    QStringList excludeTypes = {"audiobook", "podcast"};

    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &o);
    // Kept in the library's settings table under "autodj".
    static Settings load(::Library &library);
    void save(::Library &library) const;
};

struct Pick {
    qint64 trackId = 0;
    QString artist, title;
    // Which rules had to be relaxed to find it: "artist" (artist separation),
    // "recent" (played lately), "repeat" (already queued or just played).
    QStringList relaxed;
    QJsonObject toJson() const;
};

class Dj {
public:
    // seed makes the picks repeatable; 0 picks a random seed.
    Dj(::Library &library, const Settings &settings, quint32 seed = 0);

    quint32 seed() const { return m_seed; }
    // The candidate ids, loaded on first use; call reload() after the
    // library or the source changes.
    void reload();
    QString error() const { return m_error; }
    int candidates();
    // The clock the recently-played rule counts back from; tests pin it.
    void setNow(qint64 unixSeconds) { m_now = unixSeconds; }

    // Picks count tracks to follow `history` (what was played and is queued,
    // oldest first; only its tail matters). Rules relax one at a time when
    // nothing fits: first artist separation, then the recently played rule,
    // then repeats; a source of one track still plays.
    QList<Pick> pick(int count, const QList<qint64> &history = {});
    // What to append so that `upcoming` tracks follow the current one.
    // queued is what is already queued after the current track.
    QList<Pick> refill(const QList<qint64> &history, int queued);

private:
    struct Candidate {
        qint64 id;
        QStringList artists; // folded, for comparison
        QString artist, title;
        qint64 lastPlayed;
        double weight;
    };
    bool load();
    QStringList artistsOf(qint64 trackId);

    ::Library &m_library;
    Settings m_settings;
    quint32 m_seed;
    QRandomGenerator m_random;
    QList<Candidate> m_candidates;
    QHash<qint64, int> m_index;
    bool m_loaded = false;
    QString m_error;
    qint64 m_now = 0;
};

// The source's track ids, in its own order.
QList<qint64> sourceIds(::Library &library, const Source &source, QString *error = nullptr);

}

// Party mode: locks parts of the player so guests can't wreck the queue or the
// library. Backend only: the window asks allowed() before each action and
// unlock() with the PIN to leave party mode. The PIN is never stored, only a
// salted, iterated SHA-256 of it.
namespace PartyMode {

enum class Action {
    Play,          // play, pause, stop
    Skip,          // next, previous
    Seek,
    Volume,
    AddToQueue,
    RemoveFromQueue,
    Reorder,       // reorder or clear the queue
    EditTags,      // tag editor, rating, organise, delete
    EditPlaylists, // create, change or delete playlists
    Settings,      // options, Auto-DJ settings, theme
    Library,       // browse the library at all (off: the now-playing display only)
    Quit,          // quit or leave party mode without the PIN
};

QString actionName(Action a);
bool parseAction(const QString &name, Action *out);
QList<Action> allActions();

struct Settings {
    bool enabled = false;
    // jukebox: guests browse and add, nothing else. dj: guests also control
    // playback and the queue. display: now playing only, no input.
    QString preset = "jukebox";
    QList<Action> allowed; // what guests may do while enabled
    QString pinHash, pinSalt;
    int pinIterations = 0;

    bool hasPin() const { return !pinHash.isEmpty(); }
    QJsonObject toJson(bool withSecrets = false) const;
    static Settings fromJson(const QJsonObject &o);
    static Settings load(::Library &library);
    void save(::Library &library) const;
};

// The actions a preset allows: jukebox, dj or display.
QList<Action> presetActions(const QString &preset);
bool applyPreset(Settings &s, const QString &preset);
void setPin(Settings &s, const QString &pin); // an empty pin removes it
bool checkPin(const Settings &s, const QString &pin);
// True when not in party mode or when the action is allowed.
bool allowed(const Settings &s, Action a);
// Turns party mode on (with an optional new PIN) or off (needs the PIN when
// one is set). Saves the change. Returns false and leaves it on when the PIN
// is wrong.
bool enable(::Library &library, const QString &preset = {}, const QString &pin = {});
bool disable(::Library &library, const QString &pin = {});

}
