#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>
#include <QtNumeric>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <vector>

// Thrum's one Spotify session. thrum-spotify-session (sidecar/
// thrum-spotify-session, librespot) signs in with the credential
// spotify_player keeps and, over one session:
//
// - decodes Spotify's files to float stereo at 44.1 kHz: the native engine
//   opens a Spotify track (spotify://track/<id>) through a Decoder like any
//   file, so the equaliser, ReplayGain, the volume, the visualiser, the
//   output device and gapless joins all apply;
// - answers the catalogue (Spotify::SessionReader: playlists, artists'
//   top tracks, albums, Liked Songs);
// - with a Connect name set, is a Spotify Connect device whose sound plays
//   as spotify://connect, and whose news (what the phone plays, pauses,
//   volume) comes to the event handler.
//
// docs/spotify.md has the protocol.
//
// One sidecar serves it all, started when first needed. It is a child of a
// thread that lives exactly as long as it does, started with
// PR_SET_PDEATHSIG, and killed and waited for on shutdown(), so it never
// outlives Thrum.
//
// Thread-safe: the engine's worker and its background opens ask at once.
class SpotifyAudio {
public:
    // program: the sidecar; credentials: the librespot credential it reads.
    SpotifyAudio(const QString &program, const QString &credentials);
    ~SpotifyAudio();
    SpotifyAudio(const SpotifyAudio &) = delete;
    SpotifyAudio &operator=(const SpotifyAudio &) = delete;

    // The instance the engines use, or null when Thrum can't play Spotify
    // itself (no sidecar; the app hands Spotify to spotify_player then).
    static std::shared_ptr<SpotifyAudio> installed();
    static void install(std::shared_ptr<SpotifyAudio> audio);
    // Whether an engine can open path now: a Spotify track entry, with the
    // sidecar installed and Spotify switched on.
    static bool canPlay(const QString &path);
    // The track id of spotify://track/<id>, or empty.
    static QString trackId(const QString &path);
    // The Connect device's sound, as a queue entry.
    static QString connectPath() { return QStringLiteral("spotify://connect"); }
    static bool isConnect(const QString &path) { return path == connectPath(); }

    void setEnabled(bool on);
    bool enabled() const { return m_enabled; }

    struct Answer {
        QJsonObject header;
        QByteArray data;
        bool ok() const { return header.value(QStringLiteral("ok")).toBool(); }
        QString error() const { return header.value(QStringLiteral("error")).toString(); }
    };
    using Interrupt = std::function<bool()>;
    // Sends a request and waits for its answer, starting the sidecar if it
    // isn't running. A failure is an answer with "ok": false and "error".
    // interrupt, checked every 50 ms, abandons the wait.
    Answer ask(QJsonObject request, int timeoutMs, const Interrupt &interrupt = {});
    // Sends without waiting (closing a stream).
    void tell(QJsonObject request);
    // A new stream number, never used before.
    qint64 newStream();
    // Which sidecar the streams belong to: a stream of an earlier one is gone.
    quint64 generation() const;

    // Makes the session a Spotify Connect device called name, or none
    // (empty). A change restarts a running sidecar; with a name, it starts
    // at once (in the background), so the device appears.
    void setConnectName(const QString &name);
    QString connectName() const;
    // Starts the sidecar without waiting for it, when it isn't running.
    void warmUp();
    // Called on the sidecar's reader thread with each event frame
    // ({"event":"connect",…}); queue it to where it is wanted.
    using EventHandler = std::function<void(const QJsonObject &)>;
    void setEventHandler(EventHandler handler);
    // Whether the sidecar is running and signed in, and why it last failed.
    bool ready() const;
    QString lastFailure() const;
    // Why the Connect device couldn't start, or empty. The sidecar then
    // runs without it, so Spotify tracks still play.
    QString connectFailure() const;

    // Quits the sidecar and waits for it (killing it after a moment). No
    // other starts after this.
    void shutdown();

    // Tests: how many sidecars were started, and the running one's pid.
    int starts() const;
    pid_t pid() const;

private:
    bool ensureStarted(std::unique_lock<std::mutex> &lock, const Interrupt &interrupt, QString *error);
    // Starts a sidecar when none runs or starts; false while a failure is
    // remembered. Joins the last one's reader, unlocking meanwhile.
    bool launch(std::unique_lock<std::mutex> &lock);
    // Asks the running sidecar to quit (it is restarted when next needed).
    void quitRunning(std::unique_lock<std::mutex> &lock);
    void run(std::vector<std::string> argv);
    bool sendLine(const QByteArray &line);
    void stopLocked(std::unique_lock<std::mutex> &lock);

    QString m_program, m_credentials, m_connectName;
    bool m_enabled = true;
    EventHandler m_events;
    // The last start as a Connect device failed: starts go without it
    // until the name changes.
    bool m_connectBroken = false;
    QString m_connectFailure;
    std::thread m_warm;
    bool m_warming = false;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_reader;
    pid_t m_pid = 0;
    // The sidecar's input, guarded by m_writeMutex alone, so a send never
    // holds m_mutex (the reader needs that to hand over answers).
    std::mutex m_writeMutex;
    int m_toChild = -1;
    bool m_running = false, m_ready = false, m_starting = false, m_closed = false;
    QString m_failure;
    qint64 m_failedAt = 0;
    quint64 m_generation = 0;
    qint64 m_nextId = 1, m_nextStream = 1;
    int m_starts = 0;
    std::set<qint64> m_waiting;
    std::unordered_map<qint64, Answer> m_answers;
};

// One Spotify track being decoded, for a Decoder. Not thread-safe; one
// thread uses it at a time.
class SpotifyStream {
public:
    explicit SpotifyStream(std::shared_ptr<SpotifyAudio> audio);
    ~SpotifyStream();

    bool open(const QString &path, const SpotifyAudio::Interrupt &interrupt, QString *error);
    // Up to frames frames of interleaved stereo; 0 at the end (or when the
    // sidecar failed: readError() says so).
    int read(float *out, int frames);
    bool seek(qint64 ms);
    // Reads ahead, so a queued track starts at once.
    void prime();
    bool atEnd() const { return m_end && m_buffer.size() == m_head; }
    // The Connect device's sound: no length, no end.
    bool live() const { return m_live; }

    int sampleRate() const { return m_rate; }
    qint64 durationMs() const { return m_durationMs; }
    int bitrateKbps() const { return m_bitrate; }
    // title, artist, albumArtist, album, bitrate, as Decoder::metaData().
    QVariantMap metaData() const { return m_meta; }
    // Spotify's loudness figures for the track and its album, in dB and as
    // a linear peak; NaN when the file had none.
    double trackGain = qQNaN(), trackPeak = qQNaN(), albumGain = qQNaN(), albumPeak = qQNaN();
    QString readError() const { return m_readError; }

    static constexpr int chunkFrames = 4096;

private:
    bool fetch();

    std::shared_ptr<SpotifyAudio> m_audio;
    SpotifyAudio::Interrupt m_interrupt;
    qint64 m_stream = 0;
    quint64 m_generation = 0;
    bool m_open = false, m_end = false, m_live = false;
    int m_rate = 44100, m_bitrate = 0;
    qint64 m_durationMs = 0;
    QVariantMap m_meta;
    QString m_readError;
    std::vector<float> m_buffer;
    size_t m_head = 0;
};
