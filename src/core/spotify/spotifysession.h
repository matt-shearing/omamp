#pragma once

#include <memory>

#include "spotify.h"
#include "spotifyaudio.h"

// Reading what Spotify keeps from new Web API apps, the way Spotify's own
// apps read it: the tracks of Spotify's own playlists ("This Is …", the
// Daily Mixes), an artist's top tracks, an album's tracks, and Liked Songs
// for any track (spotify_player's command line can like only the one it
// plays). thrum-spotify-session
// (sidecar/thrum-spotify-session, Rust and librespot) signs in with the
// librespot credential spotify_player already keeps, which it only reads,
// and asks Spotify's private spclient endpoints, which have no per-app
// quota. Spotify's developer terms don't cover those endpoints and Spotify
// can change them, so this is a switch in Settings → Spotify, on by default,
// and the Web API stays the fallback. docs/spotify.md.
namespace Spotify {

// The sidecar's catalogue: Thrum's one Spotify session (SpotifyAudio), the
// same process that decodes Spotify's tracks for Thrum, answers these too.
// Thread-safe; any worker can ask, and several asks run at once.
class SessionReader {
public:
    explicit SessionReader(std::shared_ptr<SpotifyAudio> audio);

    // Sends one request and waits for its answer, at most timeoutMs. A
    // failure gives an answer with "ok": false and "error". A CancelScope
    // on the calling thread abandons the wait.
    QJsonObject ask(const QJsonObject &request, int timeoutMs = 30000);
    // How many sidecar processes were started (tests).
    int starts() const { return m_audio->starts(); }
    SpotifyAudio *audio() const { return m_audio.get(); }

    // The sidecar to run: THRUM_SPOTIFY_SESSION when set ("off" for none),
    // else thrum-spotify-session beside Thrum's own program, else on PATH.
    // Empty when there is none.
    static QString findProgram();
    // The credential it reads: spotify_player's.
    static QString credentialsPath();

private:
    std::shared_ptr<SpotifyAudio> m_audio;
};

// The catalogue Thrum uses when the sidecar is there: every playlist's
// tracks and an artist's top tracks from the session, the artist's albums
// from Web API search, and the Web API whenever the session fails.
class SessionCatalogue : public Catalogue {
public:
    SessionCatalogue(Backend *backend, std::shared_ptr<SessionReader> reader);
    QString id() const override { return "session"; }
    bool listsQuickly(const Item &) const override { return true; }
    QList<Item> artistPage(const Item &artist, QStringList *sections, QString *error) override;
    QList<Item> playlistTracks(const Item &playlist, QString *error) override;
    QList<Item> albumTracks(const Item &album, QString *error) override;
    bool canLike() const override { return true; }
    bool like(const QStringList &trackIds, bool on, QString *error) override;
    QHash<QString, bool> liked(const QStringList &trackIds, QString *error) override;

    SessionReader *reader() const { return m_reader.get(); }

private:
    WebApiCatalogue m_web;
    std::shared_ptr<SessionReader> m_reader;
};

} // namespace Spotify
