#pragma once

#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QThread>

#include "spotify.h"

// Reading what Spotify keeps from new Web API apps, the way Spotify's own
// apps read it: the tracks of Spotify's own playlists ("This Is …", the
// Daily Mixes) and an artist's top tracks. omamp-spotify-session
// (sidecar/omamp-spotify-session, Rust and librespot) signs in with the
// librespot credential spotify_player already keeps, which it only reads,
// and asks Spotify's private spclient endpoints, which have no per-app
// quota. Spotify's developer terms don't cover those endpoints and Spotify
// can change them, so this is a switch in Settings → Spotify, on by default,
// and the Web API stays the fallback. docs/spotify.md.
namespace Spotify {

// The sidecar, kept running while Omamp runs: one JSON request a line on
// its input, one answer a line on its output. It lives on a thread of its
// own, so any worker can ask; one request runs at a time.
class SessionReader : public QObject {
    Q_OBJECT
public:
    // program: the sidecar; credentials: the librespot credential to read.
    SessionReader(const QString &program, const QString &credentials);
    ~SessionReader() override;

    // Sends one request and waits for its answer, at most timeoutMs. A
    // failure gives an answer with "ok": false and "error".
    QJsonObject ask(const QJsonObject &request, int timeoutMs = 30000);
    // How many sidecar processes were started (tests).
    int starts() const { return m_starts; }

    // The sidecar to run: OMAMP_SPOTIFY_SESSION when set ("off" for none),
    // else omamp-spotify-session beside Omamp's own program, else on PATH.
    // Empty when there is none.
    static QString findProgram();

private:
    QJsonObject askHere(const QJsonObject &request, int timeoutMs, const CancelFlag &cancel);
    bool ensureStarted(QString *error);
    // Reads one line of output, waiting at most until the deadline.
    bool readLine(QByteArray *line, qint64 deadline, const CancelFlag &cancel);

    QString m_program, m_credentials;
    QThread m_thread;
    // Lives on m_thread: work for the process is run through it there.
    QObject *m_context = nullptr;
    QMutex m_mutex;
    QProcess *m_process = nullptr;
    int m_next = 1;
    int m_starts = 0;
};

// The catalogue Omamp uses when the sidecar is there: every playlist's
// tracks and an artist's top tracks from the session, the artist's albums
// from Web API search, and the Web API whenever the session fails.
class SessionCatalogue : public Catalogue {
public:
    SessionCatalogue(Backend *backend, std::shared_ptr<SessionReader> reader);
    QString id() const override { return "session"; }
    bool listsQuickly(const Item &) const override { return true; }
    QList<Item> artistPage(const Item &artist, QStringList *sections, QString *error) override;
    QList<Item> playlistTracks(const Item &playlist, QString *error) override;

    SessionReader *reader() const { return m_reader.get(); }

private:
    WebApiCatalogue m_web;
    std::shared_ptr<SessionReader> m_reader;
};

} // namespace Spotify
