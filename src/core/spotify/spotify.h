#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>

// Spotify, through a program that already speaks to it. Omamp never signs in
// to Spotify, never sees a password and never keeps a token: the backend
// program does all of that with its own sign-in, and Omamp runs its command
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

// A track, album, artist or playlist as Omamp shows it.
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
    QString imageUrl;  // only the playback state carries one
    bool explicitContent = false;

    bool isValid() const { return !id.isEmpty() && !kind.isEmpty(); }
    QString uri() const { return "spotify:" + kind + ':' + id; }
    QString webUrl() const { return "https://open.spotify.com/" + kind + '/' + id; }
    // The Now Playing entry for a track: spotify://track/<id>.
    QString queuePath() const { return queuePathFor(kind, id); }
    QString artistText() const { return artists.join(", "); }
    QVariantMap toMap() const;
    QJsonObject toJson() const;

    static QString queuePathFor(const QString &kind, const QString &id) { return "spotify://" + kind + '/' + id; }
};

// What the backend is playing.
struct Playback {
    bool valid = false;   // false: nothing is playing anywhere
    bool playing = false;
    Item track;
    qint64 progressMs = 0;
    QString deviceId, deviceName, deviceType;
    int volume = -1;
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

// --- Settings ---------------------------------------------------------------
//
// ~/.config/omamp/spotify.conf. Switches only: no account, no token.
struct Settings {
    bool enabled = false;          // "Show Spotify in Omamp"
    QString backend = "spotify_player";
    bool blendSearch = true;       // Spotify results under the library search
    bool blendPalette = true;      // and in the command palette
    bool startDaemon = true;       // start spotify_player when Omamp needs it

    static QString defaultPath();
    static Settings load(const QString &path = {});
    bool save(const QString &path = {}) const;
};

// --- Backends ---------------------------------------------------------------

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
    virtual bool resume(QString *error) = 0;
    virtual bool pause(QString *error) = 0;
    // currentMs is the position Omamp believes, for backends that seek by
    // an offset.
    virtual bool seekTo(qint64 ms, qint64 currentMs, QString *error) = 0;
    // Saves (or removes) the playing track in Liked Songs.
    virtual bool like(bool on, QString *error) = 0;
    virtual QList<Device> devices(QString *error) = 0;
    virtual bool connectDevice(const QString &id, QString *error) = 0;

    // section: playlists, liked, albums, artists.
    virtual QList<Item> library(const QString &section, QString *error);
    // The tracks of a playlist or an album, or an artist's top tracks.
    virtual QList<Item> tracksOf(const Item &container, QString *error);
    virtual SearchResults search(const QString &text, QString *error);
    // Adds a track or a whole album to a playlist.
    virtual bool addToPlaylist(const QString &playlistId, const Item &item, QString *error);
    // A track's name, artists and album, by id. Backends that can't look
    // one up return just the id.
    virtual Item track(const QString &id, QString *error);
    // The account's display name, when the backend can tell.
    virtual QString account(QString *error);

    static std::unique_ptr<Backend> create(const QString &id);
    static QStringList ids() { return {"spotify_player", "spotifast"}; }
    // How long a command may take before it is stopped.
    static int timeoutMs();

protected:
    // Runs program with arguments, stdin closed (a prompt then reads end of
    // file instead of waiting for ever), and returns what it printed.
    // false, with *error, on a failure to start, a timeout or a non-zero
    // exit (the program's own message, first line).
    static bool runProgram(const QString &program, const QStringList &arguments, QByteArray *out,
                           QString *error, int timeoutMs = -1);
    static QString unsupported(const QString &what);
};

class SpotifyPlayerBackend : public Backend {
public:
    // program: the spotify_player to run; empty finds it on PATH (or takes
    // OMAMP_SPOTIFY_PLAYER).
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
    SearchResults search(const QString &text, QString *error) override;
    bool addToPlaylist(const QString &playlistId, const Item &item, QString *error) override;
    Item track(const QString &id, QString *error) override;
    QString account(QString *error) override;

    // ~/.config/spotify-player and ~/.cache/spotify-player, as
    // spotify_player picks them.
    static QString configFolder();
    static QString cacheFolder();
    // Whether a process is spotify_player running as the app or a daemon
    // (not one of its CLI commands), by its executable and arguments.
    static bool isInstance(const QString &exe, const QStringList &arguments, const QString &program);

private:
    // Runs a command that needs the running instance: refuses before
    // signing in and while nothing runs, because spotify_player would
    // otherwise start a client of its own that may ask to sign in.
    bool command(const QStringList &arguments, QByteArray *out, QString *error) const;
    bool json(const QStringList &arguments, QJsonValue *value, QString *error) const;

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
