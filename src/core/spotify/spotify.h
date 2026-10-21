#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QHash>
#include <QVariantMap>
#include <atomic>
#include <memory>

// Spotify, through a program that already speaks to it. Thrum never signs in
// to Spotify, never sees a password and never keeps a token: the backend
// program does all of that with its own sign-in, and Thrum runs its command
// line. docs/spotify.md explains the choice.
//
// Two backends:
//
//   - spotify_player (https://github.com/aome510/spotify-player), run as a
//     daemon. Its CLI answers in JSON: the library, search, playback state,
//     and starting, pausing and seeking playback on its own Spotify Connect
//     device. This is the full integration.
//   - Spotifast, an app that is already open. Its CLI plays a Spotify link,
//     pauses, seeks and reports what is playing. It has no search or
//     library, so this is control only.
//
// Everything here is synchronous and blocking (it waits for a process), so
// the window calls it on a worker thread and the CLI calls it directly.
namespace Spotify {

// A track, album, artist or playlist as Thrum shows it.
struct Item {
    QString kind; // track, album, artist, playlist
    QString id;   // Spotify's base-62 id, without the spotify:<kind>: prefix
    QString name;
    QStringList artists;
    QStringList artistIds;
    QString album, albumId;
    qint64 durationMs = 0;
    QString year;      // an album's release year
    QString owner;     // a playlist owner's display name
    QString ownerId;   // and their Spotify user id ("spotify" for Spotify's own)
    QString imageUrl;  // only the playback state carries one
    bool explicitContent = false;

    bool isValid() const { return !id.isEmpty() && !kind.isEmpty(); }
    QString uri() const { return "spotify:" + kind + ':' + id; }
    QString webUrl() const { return "https://open.spotify.com/" + kind + '/' + id; }
    // The Now Playing entry for a track: spotify://track/<id>.
    QString queuePath() const { return queuePathFor(kind, id); }
    QString artistText() const { return artists.join(", "); }
    // A playlist Spotify itself made: "This Is …", the Daily Mixes, Discover
    // Weekly and the editorial lists. Spotify lets only its own apps read
    // their tracks; an app made after November 2024 is refused, so
    // spotify_player falls back to a shared client ID that is nearly always
    // rate-limited. Spotify-made playlists are owned by the user "spotify"
    // and their ids begin with 37i9dQZF1.
    bool spotifyMade() const;
    QVariantMap toMap() const;
    QJsonObject toJson() const;

    static QString queuePathFor(const QString &kind, const QString &id) { return "spotify://" + kind + '/' + id; }
};

// A Now Playing entry for a whole playlist, album or artist
// (spotify://playlist/<id>, with "?shuffle=1" to start it shuffled): Spotify
// plays it on its device, and Thrum follows whatever track it is on. Used
// where Thrum may not read the tracks (Spotify's own playlists) or need not.
QString contextQueuePath(const Item &context, bool shuffle);
// Whether a queue path asks for a shuffled start.
bool queuePathShuffles(const QString &path);

// What the backend is playing.
struct Playback {
    bool valid = false;   // false: nothing is playing anywhere
    bool playing = false;
    Item track;
    qint64 progressMs = 0;
    QString deviceId, deviceName, deviceType;
    int volume = -1;
    // What it plays from (spotify:playlist:<id>), when the backend says;
    // MPRIS doesn't.
    QString contextUri;
    QJsonObject toJson() const;
};

struct Device {
    QString id, name, type;
    bool active = false;
    int volume = -1;
    QJsonObject toJson() const;
};

struct SearchResults {
    QList<Item> tracks, albums, artists, playlists;
    bool isEmpty() const { return tracks.isEmpty() && albums.isEmpty() && artists.isEmpty() && playlists.isEmpty(); }
    QJsonObject toJson() const;
};

// Takes a spotify:<kind>:<id> URI, an open.spotify.com link, a
// spotify://<kind>/<id> queue path, or (with defaultKind) a bare id.
bool parseReference(const QString &text, QString *kind, QString *id, const QString &defaultKind = {});
bool isQueuePath(const QString &path);

// --- spotify_player's JSON ------------------------------------------------
//
// spotify_player prints what its client returns with serde_json, then
// replaces every two-character "\n" escape with a real newline. So a
// description with a line break arrives as invalid JSON; repair() puts the
// escapes back (serde_json writes no whitespace of its own).
QByteArray repair(const QByteArray &cliOutput);
// spotify_player's own Track, Album, Artist and Playlist (state/model.rs):
// ids are bare, a track's duration is {"secs", "nanos"}, a playlist's owner
// is ["display name", "user id"], and there are no pictures.
Item parseTrack(const QJsonObject &o);
Item parseAlbum(const QJsonObject &o);
Item parseArtist(const QJsonObject &o);
Item parsePlaylist(const QJsonObject &o);
QList<Item> parseItems(const QJsonArray &a, const QString &kind);
// rspotify's FullTrack, as in the playback state: duration_ms, and the
// album's pictures.
Item parseFullTrack(const QJsonObject &o);
// `get key playback`: rspotify's CurrentPlaybackContext, or null.
Playback parsePlayback(const QJsonValue &v);
QList<Device> parseDevices(const QJsonArray &a);
// `search`: {tracks, artists, albums, playlists, shows, episodes}.
SearchResults parseSearch(const QJsonObject &o);
// `get item … <kind>`: {playlist, tracks}, {album, tracks} or
// {artist, top_tracks, albums, related_artists}; the tracks.
QList<Item> parseContextTracks(const QJsonObject &o);

// --- Rate limits and slow answers ------------------------------------------------
//
// spotify_player talks to Spotify's Web API, which answers "429 Too Many
// Requests" when a client ID asks too often. spotify_player then waits and
// retries on its own, so a command that would take a second can take half a
// minute, and only its log says why. Thrum reads that log (never writes it)
// to tell "slow because Spotify is rate-limiting" from "broken".

// Whether a message from spotify_player is about a rate limit.
bool isRateLimitText(const QString &text);

// spotify_player 0.25 asks with your own client ID first and, when Spotify
// refuses that ID (a 4xx answer), asks again with ncspot's shared one. It
// logs each refusal: "Custom Spotify Web API request failed; retrying with
// ncspot client method="GET" url="artists/<id>/top-tracks" status=403
// Forbidden".
struct Refusal {
    QString method;
    QString url;    // as logged: relative to https://api.spotify.com/v1/
    int status = 0; // 403, 404, 400…
    QDateTime at;   // UTC
};

struct LogNews {
    int rateLimits = 0;     // rate-limit lines newer than the window
    QDateTime retryUntil;   // when Spotify said to try again, in UTC
    QDateTime lastAt;       // the newest rate-limit line's time, in UTC
    QList<Refusal> refusals; // your client ID refused, from today
};
// Reads what spotify_player's newest log (spotify-player-*.log in its cache
// folder) said about rate limits in the last windowSecs seconds. *file and
// *offset remember where the last read stopped, so a call reads only what
// is new; pass empty and 0 the first time.
LogNews readLogNews(const QString &cacheFolder, QString *file, qint64 *offset, int windowSecs = 120,
                    const QDateTime &now = QDateTime::currentDateTimeUtc());

// What a refused request was for, so the same kind of request isn't sent
// again: "artist-top-tracks", "artist-related-artists", "artist-albums",
// "playlist:<id>" (one playlist's details or tracks), "recommendations",
// or otherwise the path with its ids replaced by "*".
QString refusalKey(const QString &url);

// What Spotify refused to a client ID, learned from spotify_player's log and
// kept for the rest of the day (Spotify can change its mind, and a new day
// tries again). Kept in a small JSON file so Thrum remembers across runs.
class Refusals {
public:
    explicit Refusals(const QString &file = {});
    void setFile(const QString &file);
    // The client ID whose refusals count (another ID starts afresh).
    void setClientId(const QString &id);
    QString clientId() const { return m_clientId; }
    // Returns true when the key is new.
    bool learn(const QString &key, int status, const QDate &day = QDate::currentDate());
    bool refused(const QString &key, const QDate &day = QDate::currentDate()) const;
    int status(const QString &key) const { return m_status.value(key); }
    QStringList keys(const QDate &day = QDate::currentDate()) const;
    void clear();

private:
    void load();
    void save() const;

    QString m_file;
    QString m_clientId;
    QDate m_day;
    QHash<QString, int> m_status;
};

// A command can be cancelled from another thread: the worker running it
// holds a CancelScope, and runProgram stops the process when the flag is
// set.
using CancelFlag = std::shared_ptr<std::atomic<bool>>;
inline CancelFlag makeCancelFlag() { return std::make_shared<std::atomic<bool>>(false); }
class CancelScope {
public:
    explicit CancelScope(CancelFlag flag);
    ~CancelScope();
    static bool cancelled();
    // The flag of the scope on this thread, for work handed to another.
    static CancelFlag current();
private:
    CancelFlag m_previous;
};

// --- Settings ---------------------------------------------------------------
//
// ~/.config/thrum/spotify.conf. Switches only: no account, no token.
struct Settings {
    bool enabled = false;          // "Show Spotify in Thrum"
    QString backend = "spotify_player";
    bool blendSearch = true;       // Spotify results under the library search
    bool blendPalette = true;      // and in the command palette
    bool startDaemon = true;       // start spotify_player when Thrum needs it
    // Read Spotify's own playlists and artists' top tracks the way
    // Spotify's own app does (thrum-spotify-session), when it is installed.
    bool sessionReads = true;

    static QString defaultPath();
    static Settings load(const QString &path = {});
    bool save(const QString &path = {}) const;
};

// --- Backends ---------------------------------------------------------------

class Backend;

// Where artist pages and playlist tracks come from. The window and the
// command line ask only this, so a better source than the Web API (one
// that Spotify lets read its own playlists) can replace it without
// touching the views. Runs on a worker thread, like the backend.
class Catalogue {
public:
    virtual ~Catalogue() = default;
    // "web-api" for the Web API through the backend.
    virtual QString id() const = 0;
    // Whether this source lists a playlist's tracks quickly. When it can't,
    // the playlist plays as a whole and its tracks are listed on request.
    virtual bool listsQuickly(const Item &playlist) const = 0;
    // An artist's best-known tracks, then their albums, with a heading for
    // each item in *sections.
    virtual QList<Item> artistPage(const Item &artist, QStringList *sections, QString *error) = 0;
    virtual QList<Item> playlistTracks(const Item &playlist, QString *error) = 0;
};

// The Web API, through the backend's command line. Spotify refuses apps
// made since November 2024 an artist's top tracks, related artists and
// album list, and the tracks of the playlists Spotify makes, so artist pages
// come from search and Spotify's own playlists don't list quickly.
class WebApiCatalogue : public Catalogue {
public:
    explicit WebApiCatalogue(Backend *backend) : m_backend(backend) {}
    QString id() const override { return "web-api"; }
    bool listsQuickly(const Item &playlist) const override { return !playlist.spotifyMade(); }
    QList<Item> artistPage(const Item &artist, QStringList *sections, QString *error) override;
    QList<Item> playlistTracks(const Item &playlist, QString *error) override;

private:
    Backend *m_backend;
};

class Backend {
public:
    struct Caps {
        bool library = false;   // playlists, Liked Songs, saved albums, followed artists
        bool search = false;
        bool playlists = false; // adding to a playlist
    };

    virtual ~Backend() = default;
    virtual QString id() const = 0;   // spotify_player, spotifast
    virtual QString name() const = 0; // for people
    virtual Caps caps() const = 0;

    // The program, as found on PATH; empty when it isn't installed.
    virtual QString program() const = 0;
    bool installed() const { return !program().isEmpty(); }
    // Whether the backend has been signed in. spotify_player keeps its
    // sign-in in files whose names (never contents) say so; Spotifast
    // keeps it in the keyring, so it counts as signed in while it runs.
    virtual bool signedIn() const = 0;
    virtual bool running() const = 0;
    // Starts the backend in the background, when that is possible.
    virtual bool start(QString *error) = 0;
    // The command that signs in, run in a terminal for the person; empty
    // when sign-in happens in the backend's own window.
    virtual QStringList signInCommand() const = 0;
    // The name this computer has as a Spotify Connect device.
    virtual QString deviceName() const = 0;

    virtual Playback playback(QString *error) = 0;
    virtual bool playTrack(const QString &id, QString *error) = 0;
    // Plays a track, album, artist or playlist on the backend itself, with
    // Spotify choosing what follows (the command line's `play`).
    virtual bool playItem(const Item &item, QString *error) = 0;
    // Plays a playlist, album or artist as a whole on the backend, shuffled
    // or not. Only Spotify's player is asked: nothing reads the tracks.
    virtual bool playContext(const Item &context, bool shuffle, QString *error);
    virtual bool resume(QString *error) = 0;
    virtual bool pause(QString *error) = 0;
    // currentMs is the position Thrum believes, for backends that seek by
    // an offset.
    virtual bool seekTo(qint64 ms, qint64 currentMs, QString *error) = 0;
    // Saves (or removes) the playing track in Liked Songs.
    virtual bool like(bool on, QString *error) = 0;
    virtual QList<Device> devices(QString *error) = 0;
    virtual bool connectDevice(const QString &id, QString *error) = 0;

    // section: playlists, liked, albums, artists.
    virtual QList<Item> library(const QString &section, QString *error);
    // The tracks of a playlist or an album, or an artist's best-known tracks
    // (from search: Spotify refuses an artist's top tracks to new apps).
    virtual QList<Item> tracksOf(const Item &container, QString *error);
    // An artist's page from search alone: their best-known tracks, then
    // their albums. *sections gets a heading for each item. Search is
    // allowed for every app, where an artist's top tracks, related artists
    // and album list are refused to apps made since November 2024.
    virtual QList<Item> artistPage(const Item &artist, QStringList *sections, QString *error);
    virtual SearchResults search(const QString &text, QString *error);
    // Adds a track or a whole album to a playlist.
    virtual bool addToPlaylist(const QString &playlistId, const Item &item, QString *error);
    // Makes a new private playlist in your Spotify library.
    virtual bool createPlaylist(const QString &name, QString *error);
    // A track's name, artists and album, by id. Backends that can't look
    // one up return just the id.
    virtual Item track(const QString &id, QString *error);
    // The account's display name, when the backend can tell.
    virtual QString account(QString *error);

    // The backend's MPRIS name on the session bus, where it announces what
    // it plays without Thrum asking Spotify's Web API.
    virtual QString mprisName() const = 0;

    // Where artist pages and playlist tracks come from: the Web API through
    // this backend unless another source is set.
    Catalogue *catalogue();
    void setCatalogue(std::unique_ptr<Catalogue> catalogue) { m_catalogue = std::move(catalogue); }

    static std::unique_ptr<Backend> create(const QString &id);
    static QStringList ids() { return {"spotify_player", "spotifast"}; }
    // How long a quick command (play, pause, the playback state) may take
    // before it is stopped.
    static int timeoutMs();
    // How long a list may take: a playlist, an album, the library or a
    // search. Longer, because spotify_player waits out Spotify's rate
    // limits before it answers.
    static int longTimeoutMs();

protected:
    // Runs program with arguments, stdin closed (a prompt then reads end of
    // file instead of waiting for ever), and returns what it printed.
    // false, with *error, on a failure to start, a timeout or a non-zero
    // exit (the program's own message, first line).
    // A CancelScope on the calling thread stops it early ("cancelled").
    static bool runProgram(const QString &program, const QStringList &arguments, QByteArray *out,
                           QString *error, int timeoutMs = -1);
    static QString unsupported(const QString &what);

private:
    std::unique_ptr<Catalogue> m_catalogue;
};

class SpotifyPlayerBackend : public Backend {
public:
    // program: the spotify_player to run; empty finds it on PATH (or takes
    // THRUM_SPOTIFY_PLAYER).
    explicit SpotifyPlayerBackend(const QString &program = {});
    QString id() const override { return "spotify_player"; }
    QString name() const override { return "spotify_player"; }
    Caps caps() const override { return {true, true, true}; }
    QString program() const override { return m_program; }
    bool signedIn() const override;
    bool running() const override;
    bool start(QString *error) override;
    QStringList signInCommand() const override;
    QString deviceName() const override;
    QString mprisName() const override { return "org.mpris.MediaPlayer2.spotify_player"; }

    Playback playback(QString *error) override;
    bool playTrack(const QString &id, QString *error) override;
    bool playItem(const Item &item, QString *error) override;
    bool resume(QString *error) override;
    bool pause(QString *error) override;
    bool seekTo(qint64 ms, qint64 currentMs, QString *error) override;
    bool like(bool on, QString *error) override;
    QList<Device> devices(QString *error) override;
    bool connectDevice(const QString &id, QString *error) override;
    QList<Item> library(const QString &section, QString *error) override;
    QList<Item> tracksOf(const Item &container, QString *error) override;
    QList<Item> artistPage(const Item &artist, QStringList *sections, QString *error) override;
    bool playContext(const Item &context, bool shuffle, QString *error) override;
    SearchResults search(const QString &text, QString *error) override;
    bool addToPlaylist(const QString &playlistId, const Item &item, QString *error) override;
    bool createPlaylist(const QString &name, QString *error) override;
    Item track(const QString &id, QString *error) override;
    QString account(QString *error) override;

    // ~/.config/spotify-player and ~/.cache/spotify-player, as
    // spotify_player picks them.
    static QString configFolder();
    static QString cacheFolder();
    // Whether a process is spotify_player running as the app or a daemon
    // (not one of its CLI commands), by its executable and arguments.
    static bool isInstance(const QString &exe, const QStringList &arguments, const QString &program);
    // Stops every running instance of this spotify_player (SIGTERM, then
    // SIGKILL after five seconds). Only instances of m_program: a test's
    // fake never touches the real one.
    bool stopInstances(QString *error);

    // --- The Web API client ID ---------------------------------------------
    //
    // spotify_player asks Spotify's Web API with a client ID: the client_id
    // in app.toml, or ncspot's when there is none. Every person using the
    // same ID shares one rate limit, so a busy shared ID answers "429 Too
    // Many Requests". With your own ID (a free app on Spotify's developer
    // dashboard) the limit is yours; spotify_player 0.25 still falls back
    // to ncspot's ID when yours is refused, and always reads your playlist
    // list through it.
    static QString ncspotClientId() { return "d420a117a32841c2b3474932e49fb54b"; }
    // Spotify's own desktop app ID, which librespot uses to sign in. Set as
    // client_id it is shared by every librespot-based player.
    static QString spotifyDesktopClientId() { return "65b708073fc0480ea92a077233ca87bd"; }
    // The client_id in app.toml's top level, or empty.
    static QString configuredClientId(const QString &configFolder = {});
    // Whether app.toml sets client_id_command, which overrides client_id.
    static bool hasClientIdCommand(const QString &configFolder = {});
    // What spotify_player uses.
    static QString effectiveClientId(const QString &configFolder = {});
    // A Spotify client ID: 32 hexadecimal characters.
    static bool isValidClientId(const QString &id);
    // Sets client_id in app.toml and changes nothing else in it: the file
    // is copied to app.toml.thrum-backup-<time> first, then the client_id
    // line is replaced (or added above the first [table]), and the new file
    // replaces the old in one step. *backup gets the copy's path.
    static bool writeClientId(const QString &configFolder, const QString &id, QString *backup, QString *error);

private:
    // Runs a command that needs the running instance: refuses before
    // signing in and while nothing runs, because spotify_player would
    // otherwise start a client of its own that may ask to sign in.
    bool command(const QStringList &arguments, QByteArray *out, QString *error, int timeoutMs = -1) const;
    bool json(const QStringList &arguments, QJsonValue *value, QString *error, int timeoutMs = -1) const;

    QString m_program;
};

class SpotifastBackend : public Backend {
public:
    explicit SpotifastBackend(const QString &program = {});
    QString id() const override { return "spotifast"; }
    QString name() const override { return "Spotifast"; }
    Caps caps() const override { return {}; }
    QString program() const override { return m_program; }
    bool signedIn() const override { return running(); }
    bool running() const override;
    bool start(QString *error) override;
    QStringList signInCommand() const override { return {}; }
    QString deviceName() const override { return "Spotifast"; }
    QString mprisName() const override { return "org.mpris.MediaPlayer2.spotifast"; }

    Playback playback(QString *error) override;
    bool playTrack(const QString &id, QString *error) override;
    bool playItem(const Item &item, QString *error) override;
    bool resume(QString *error) override;
    bool pause(QString *error) override;
    bool seekTo(qint64 ms, qint64 currentMs, QString *error) override;
    bool like(bool on, QString *error) override;
    QList<Device> devices(QString *error) override;
    bool connectDevice(const QString &id, QString *error) override;

    // `spotifast now-playing --raw`: tab-separated state, title, artists,
    // album, position_ms, duration_ms, volume, shuffle, repeat, art_url,
    // saved, device. Later versions append fields.
    static Playback parseNowPlaying(const QByteArray &line, bool *saved = nullptr);

private:
    QString m_program;
};

} // namespace Spotify
