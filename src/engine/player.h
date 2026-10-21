#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <functional>

#include "audioengine.h"
#include "playlist.h"
#include "spectrum.h"

// The player's state and every action the window, the keyboard and MPRIS can
// take. QML only lays it out.
class Player : public QObject {
    Q_OBJECT
    Q_PROPERTY(Playlist *playlist READ playlist CONSTANT)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY trackChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY trackChanged)
    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY trackChanged)
    Q_PROPERTY(QString album READ album NOTIFY trackChanged)
    Q_PROPERTY(int bitrate READ bitrate NOTIFY trackChanged)
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY formatChanged)
    Q_PROPERTY(int channels READ channels NOTIFY formatChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY modesChanged)
    Q_PROPERTY(bool repeat READ repeat WRITE setRepeat NOTIFY modesChanged)
    Q_PROPERTY(QList<qreal> levels READ levels NOTIFY levelsChanged)
    Q_PROPERTY(QList<qreal> peaks READ peaks NOTIFY levelsChanged)
    // The sound itself, for the oscilloscope: 128 points from -1 to 1.
    Q_PROPERTY(QList<qreal> waveform READ waveform NOTIFY levelsChanged)
    // Left then right, Spectrum::wavePoints each, for the waveform ribbon.
    Q_PROPERTY(QList<qreal> stereoWaveform READ stereoWaveform NOTIFY levelsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // Playing, but waiting for a stream to arrive; statusText says so in
    // words ("Buffering…"), empty otherwise.
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY bufferingChanged)
    // The current entry is a live radio station (radio://…): no length and
    // no seeking; position is how long it has been listened to.
    Q_PROPERTY(bool live READ live NOTIFY trackChanged)
    // The current entry plays in another program (Spotify), so no sound
    // reaches the analyser: the visualisers show their idle animation.
    Q_PROPERTY(bool playsElsewhere READ playsElsewhere NOTIFY trackChanged)
    // Which engine plays ("qt" or "native") and whether it joins tracks
    // without a gap. Chosen at start: OMAMP_ENGINE, else playback/engine in
    // omamp.conf beside the state file, else "qt".
    Q_PROPERTY(QString engineName READ engineName CONSTANT)
    Q_PROPERTY(bool gapless READ gapless CONSTANT)
    // Playback settings. The native engine acts on them; the Qt engine
    // keeps them for when the native one is switched on.
    Q_PROPERTY(qreal crossfadeSeconds READ crossfadeSeconds WRITE setCrossfadeSeconds NOTIFY playbackSettingsChanged)
    Q_PROPERTY(QString replayGainMode READ replayGainMode WRITE setReplayGainMode NOTIFY playbackSettingsChanged)
    Q_PROPERTY(qreal replayGainPreamp READ replayGainPreamp WRITE setReplayGainPreamp NOTIFY playbackSettingsChanged)
    Q_PROPERTY(bool eqEnabled READ eqEnabled WRITE setEqEnabled NOTIFY playbackSettingsChanged)
    Q_PROPERTY(qreal eqPreamp READ eqPreamp WRITE setEqPreamp NOTIFY playbackSettingsChanged)
    Q_PROPERTY(QList<qreal> eqBands READ eqBands WRITE setEqBands NOTIFY playbackSettingsChanged)
    Q_PROPERTY(QList<qreal> eqFrequencies READ eqFrequencies CONSTANT)
    Q_PROPERTY(QStringList eqPresets READ eqPresets CONSTANT)
    // Limit ReplayGain so a file's tagged peak stays below full scale.
    Q_PROPERTY(bool clipPrevention READ clipPrevention WRITE setClipPrevention NOTIFY playbackSettingsChanged)
    // The engine chosen in the settings ("qt" or "native"), which takes
    // effect at the next start; engineName is the one playing now.
    Q_PROPERTY(QString preferredEngine READ preferredEngine WRITE setPreferredEngine NOTIFY playbackSettingsChanged)

public:
    // Turns a queue entry that isn't a local file (subsonic://<source>/<song>)
    // into the URL to play, or an invalid URL with *error set. The URL may
    // carry a token: the Player hands it to the engine and keeps it nowhere
    // else, never in the saved state, a playlist or a message.
    using Resolver = std::function<QUrl(const QString &path, qint64 trackId, QString *error)>;

    // An empty state path means nothing is remembered between runs (tests).
    explicit Player(const QString &statePath = {}, QObject *parent = nullptr);
    // With a resolver from the start, so the restored queue's streams
    // resolve when the track is first loaded.
    Player(const QString &statePath, Resolver resolver, QObject *parent = nullptr);
    // With a given engine, which the Player takes (tests).
    Player(AudioEngine *engine, const QString &statePath, QObject *parent = nullptr);
    Player(AudioEngine *engine, const QString &statePath, Resolver resolver, QObject *parent = nullptr);
    ~Player() override;

    // The engine the settings choose (OMAMP_ENGINE, else playback/engine in
    // omamp.conf beside the state file), for a caller that wraps it in a
    // HandoffEngine before passing it in.
    static AudioEngine *createEngine(const QString &statePath);

    Playlist *playlist() { return &m_playlist; }
    int currentIndex() const { return m_current; }
    QString state() const;
    qint64 position() const { return m_engine->position(); }
    qint64 duration() const;
    QString title() const;
    QString artist() const;
    QString album() const { return m_album; }
    int bitrate() const { return m_bitrate; }
    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }
    int volume() const { return m_volume; }
    bool shuffle() const { return m_shuffle; }
    bool repeat() const { return m_repeat; }
    QList<qreal> levels() const { return m_spectrum.levels(); }
    // How readily the analyser's bars fill: "low", "normal" or "high"
    // (Settings > Appearance > Visualiser; spectrum.h).
    void setVisualiserSensitivity(const QString &sensitivity) { m_spectrum.setSensitivity(sensitivity); }
    QString visualiserSensitivity() const { return m_spectrum.sensitivity(); }
    QList<qreal> peaks() const { return m_spectrum.peaks(); }
    QList<qreal> waveform() const { return m_spectrum.waveform(); }
    QList<qreal> stereoWaveform() const { return m_spectrum.stereoWaveform(); }
    QString error() const { return m_error; }
    bool buffering() const { return m_engine->buffering(); }
    bool live() const;
    bool playsElsewhere() const;
    // What a live station is playing now, from the radio's metadata (its
    // feed or the stream's ICY titles). Shown as the title, artist and
    // album wherever the playing track shows, and sent over MPRIS. Ignored
    // unless the current entry is live.
    void setLiveInfo(const QString &title, const QString &artist, const QString &album);
    QString statusText() const { return buffering() ? tr("Buffering…") : QString(); }

    // Without a resolver, http(s) entries play as they are and other
    // references fail with a message.
    void setResolver(Resolver resolver);

    void setVolume(int volume);
    void setShuffle(bool on);
    void setRepeat(bool on);
    void setMuted(bool muted) { m_engine->setMuted(muted); }

    AudioEngine *engine() const { return m_engine; }
    QString engineName() const { return m_engine->name(); }
    bool gapless() const { return m_engine->gapless(); }
    qreal crossfadeSeconds() const { return m_engine->settings().crossfadeMs / 1000.0; }
    QString replayGainMode() const;
    qreal replayGainPreamp() const { return m_engine->settings().replayGainPreampDb; }
    bool eqEnabled() const { return m_engine->settings().eqEnabled; }
    qreal eqPreamp() const { return m_engine->settings().eqPreampDb; }
    QList<qreal> eqBands() const { return m_engine->settings().eqBandsDb; }
    QList<qreal> eqFrequencies() const;
    QStringList eqPresets() const;
    bool clipPrevention() const { return m_engine->settings().clipPrevention; }
    void setClipPrevention(bool on);
    QString preferredEngine() const;
    void setPreferredEngine(const QString &name);
    void setCrossfadeSeconds(qreal seconds);
    void setReplayGainMode(const QString &mode);
    void setReplayGainPreamp(qreal db);
    void setEqEnabled(bool on);
    void setEqPreamp(qreal db);
    void setEqBands(const QList<qreal> &bands);
    Q_INVOKABLE void setEqBand(int band, qreal db);
    // Sets the bands to a named preset; false if there is no such preset.
    Q_INVOKABLE bool applyEqPreset(const QString &name);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void playIndex(int row);
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void seekBy(qint64 ms);
    Q_INVOKABLE int addPaths(const QStringList &paths);
    Q_INVOKABLE int addUrls(const QVariantList &urls);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void dismissError();
    // One row of Now Playing for QML: {path, title, artist, durationMs}.
    Q_INVOKABLE QVariantMap trackAt(int row) const;

    // From the library window: tracks whose tags are already known.
    // playTracks replaces the list and plays from start ("play now"),
    // queueNext puts them after the current track, appendTracks adds them
    // at the end. Each returns how many tracks it took.
    int playTracks(const QList<Playlist::Track> &tracks, int start);
    int queueNext(const QList<Playlist::Track> &tracks);
    int appendTracks(const QList<Playlist::Track> &tracks);

    // Adds folders the user picked, starting playback if the list was empty.
    void addPicked(const QStringList &paths);
    void reportError(const QString &message) { setError(message); }

    // Which row comes after the current one, or -1 at the end of the list.
    int nextIndex();

    // Ends the play being counted now (as on quitting), so playFinished()
    // reports it. Nothing happens when nothing is being counted.
    void finishPlay(bool userSkipped = false);
    // How much of the current play has been heard so far, in ms.
    qint64 heardMs() const { return m_heardMs; }

signals:
    void trackChanged();
    void stateChanged();
    void positionChanged();
    void formatChanged();
    void volumeChanged();
    void modesChanged();
    void levelsChanged();
    void errorChanged();
    void bufferingChanged();
    void playbackSettingsChanged();
    void seeked(qint64 position);
    // A live station's stream didn't open or broke off for good. The Player
    // doesn't skip past it, so the radio can try the station's next address.
    void liveStreamFailed(const QString &path, const QString &detail);
    // Play statistics. A play starts when a track begins playing and ends
    // when it finishes, is skipped, another track starts or playback stops.
    // heardMs is the time actually heard: position moves while playing,
    // never a seek. userSkipped is true for Next, Previous or picking
    // another track. playProgress comes every 15 seconds heard.
    void playStarted(const QString &path);
    void playProgress(const QString &path, qint64 heardMs);
    void playFinished(const QString &path, qint64 heardMs, qint64 durationMs, bool userSkipped);
    // The window asked for a folder. The app connects this to the desktop's
    // folder picker, so the engine needs no D-Bus or portal code.
    void folderRequested();

private:
    void connectEngine();
    void beginPlay();
    void countHeard(qint64 position);
    // skip: the listener jumped while playing, so the engine may crossfade.
    void load(int row, bool skip = false);
    void requeue();
    // Where row's audio is: an invalid URL for a local file; for a
    // reference, the resolver's URL (or invalid, with the reason noted).
    QUrl urlFor(int row);
    // A row's name for messages: "Artist - Title", or the file name.
    QString displayName(const QString &path) const;
    void readMetaData();
    void changeSettings(const std::function<void(AudioEngine::Settings &)> &change);
    void saveSettings();
    void restoreSettings();
    void setError(const QString &error);
    void saveSoon();
    void save();
    void restore();

    AudioEngine *m_engine;
    Resolver m_resolver;
    // The last URL handed over for the queued row, so requeueing the same
    // track doesn't ask the resolver again. Memory only.
    QString m_queuedPath;
    QUrl m_queuedUrl;
    // Why a reference couldn't be resolved, by path, for the message when
    // the engine then fails on it. Memory only, and free of secrets.
    QHash<QString, QString> m_resolveErrors;
    Playlist m_playlist;
    Spectrum m_spectrum;
    QTimer m_spectrumTimer, m_saveTimer;
    QString m_statePath, m_settingsPath;
    QList<int> m_history;
    int m_current = -1;
    // The row queued in the engine to follow the current one.
    int m_queued = -1;
    // Files that failed in a row, so a list of broken files can't loop.
    int m_badRun = 0;
    bool m_wantPlaying = false;
    int m_volume = 70;
    bool m_shuffle = false, m_repeat = false;
    bool m_stopped = true;
    QString m_title, m_artist, m_album, m_error;
    // The play being counted: its file, the time heard, the last position
    // seen while playing, and the next progress report.
    QString m_playPath;
    qint64 m_heardMs = 0, m_lastPos = 0, m_nextProgress = 15000;
    // Set while next(), previous() or playIndex() change the track.
    bool m_skipping = false;
    // The engine choice when there is no settings file (tests).
    QString m_preferredEngine;
    int m_bitrate = 0, m_sampleRate = 0, m_channels = 0;
};
