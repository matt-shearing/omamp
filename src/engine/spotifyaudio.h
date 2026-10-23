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

// Spotify played by Thrum itself. thrum-spotify-session's audio mode
// (sidecar/thrum-spotify-session, librespot) signs in with the credential
// spotify_player keeps and decodes Spotify's files to float stereo at
// 44.1 kHz; the native engine opens a Spotify track (spotify://track/<id>)
// through a Decoder like any file, so the equaliser, ReplayGain, the volume,
// the visualiser, the output device and gapless joins all apply.
// docs/spotify.md has the protocol.
//
// One sidecar serves every track, started when the first one opens. It is
// a child of a thread that lives exactly as long as it does, started with
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

    // Quits the sidecar and waits for it (killing it after a moment). No
    // other starts after this.
    void shutdown();

    // Tests: how many sidecars were started, and the running one's pid.
    int starts() const;
    pid_t pid() const;

private:
    bool ensureStarted(std::unique_lock<std::mutex> &lock, const Interrupt &interrupt, QString *error);
    void run(std::vector<std::string> argv);
    bool sendLine(const QByteArray &line);
    void stopLocked(std::unique_lock<std::mutex> &lock);

    QString m_program, m_credentials;
    bool m_enabled = true;
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
    bool m_open = false, m_end = false;
    int m_rate = 44100, m_bitrate = 0;
    qint64 m_durationMs = 0;
    QVariantMap m_meta;
    QString m_readError;
    std::vector<float> m_buffer;
    size_t m_head = 0;
};
