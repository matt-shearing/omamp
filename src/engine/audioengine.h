#pragma once

#include <QAudioBuffer>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

// What the Player needs from whatever turns files into sound. Two engines
// implement it: QtEngine (QMediaPlayer, the default until the native engine
// is proven) and NativeEngine (FFmpeg decoding into one QAudioSink, with
// gapless playback, crossfade, ReplayGain and an equaliser).
//
// The Player owns the play order. It loads the current file and queues the
// one after it; the engine moves to the queued file on its own when the
// current one ends (gaplessly, if it can) and says so with advanced().
class AudioEngine : public QObject {
    Q_OBJECT

public:
    enum State { Stopped, Playing, Paused };
    Q_ENUM(State)

    enum class ReplayGainMode { Off, Track, Album };

    // Settings only the native engine acts on. The Qt engine keeps them so
    // the Player can show and save them either way.
    struct Settings {
        int crossfadeMs = 0;                // 0..12000; 0 means gapless
        ReplayGainMode replayGain = ReplayGainMode::Off;
        double replayGainPreampDb = 0;      // added to tagged files only
        bool clipPrevention = true;         // limit gain so the tagged peak stays below 1
        bool eqEnabled = false;
        double eqPreampDb = 0;
        QList<double> eqBandsDb = QList<double>(10, 0.0);
    };

    explicit AudioEngine(QObject *parent = nullptr) : QObject(parent) {}

    // "qt" or "native".
    virtual QString name() const = 0;
    // Whether consecutive tracks really join without a gap.
    virtual bool gapless() const = 0;
    // Whether the engine moves past a file it cannot play. The Qt engine
    // stops on it, as Thrum always has.
    virtual bool skipsUnplayable() const = 0;

    // Whether this engine can open path itself. A Spotify track
    // (spotify://…) only the native engine opens, when Thrum plays Spotify
    // itself (spotifyaudio.h); everything else is tried, and may fail.
    virtual bool canPlay(const QString &path) const { return !path.startsWith(QLatin1String("spotify:")); }

    // Replaces the current file and stops. An empty path unloads.
    //
    // path is the track's name in the play queue: a local file, or a
    // library reference such as subsonic://<source>/<song> that never holds
    // a secret. url, when valid, is where the audio really is (a signed
    // http(s) stream URL). The engine opens url but reports, logs and keeps
    // only path, so a token in the URL never reaches the window, a log or
    // the saved state.
    virtual void load(const QString &path, const QUrl &url = {}) = 0;
    // The file to play when the current one ends. An empty path clears it.
    virtual void queueNext(const QString &path, const QUrl &url = {}) = 0;
    // A jump the listener asked for (next, previous, a picked track) while
    // playing: the new track plays at once. The native engine crossfades
    // into it when crossfade is on; otherwise, and on the Qt engine, it is
    // load() and play().
    virtual void skipTo(const QString &path, const QUrl &url = {}) {
        load(path, url);
        play();
    }
    // path, never the URL.
    virtual QString source() const = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(qint64 ms) = 0;

    virtual qint64 position() const = 0;
    virtual qint64 duration() const = 0;
    virtual State state() const = 0;
    // True while playing but waiting for audio to arrive: a stream still
    // connecting, or a slow or stalled network.
    virtual bool buffering() const { return false; }

    // Linear gain, 0..1. The Player converts its slider to this.
    virtual void setVolume(qreal linear) = 0;
    virtual void setMuted(bool muted) = 0;

    // title, artist, album, albumArtist (strings) and bitrate (kbps, int).
    virtual QVariantMap metaData() const = 0;

    virtual void setSettings(const Settings &settings) { m_settings = settings; }
    Settings settings() const { return m_settings; }

    // Whether a location is a network stream rather than a local file.
    static bool isNetworkUrl(const QUrl &url) {
        const QString scheme = url.scheme().toLower();
        return scheme == QLatin1String("http") || scheme == QLatin1String("https");
    }
    // The text with every secret a URL in it could carry blanked out: the
    // Subsonic token, salt, password and API key, and user info. For error
    // messages and logs, which may quote what FFmpeg or Qt were asked to
    // open.
    static QString redact(const QString &text);

signals:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void stateChanged();
    void bufferingChanged();
    void metaDataChanged();
    // The decoded format of the current file: sample rate and channels.
    void formatChanged(int sampleRate, int channels);
    // Decoded audio for the analyser, roughly as it is heard.
    void pcmTapped(const QAudioBuffer &buffer);
    // The queued file has become the current one. Emitted before its
    // metadata, so the Player can clear the old track's tags first.
    void advanced();
    // The current file ended and nothing was queued.
    void endOfQueue();
    // A file could not be opened or decoded. detail may be empty.
    void trackFailed(const QString &path, const QString &detail);
    // Any other error, already worded for the user.
    void errorOccurred(const QString &message);

protected:
    Settings m_settings;
};
