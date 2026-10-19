#pragma once

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>
#include <QTimer>
#include <QVariantList>

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
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    // An empty state path means nothing is remembered between runs (tests).
    explicit Player(const QString &statePath = {}, QObject *parent = nullptr);
    ~Player() override;

    Playlist *playlist() { return &m_playlist; }
    int currentIndex() const { return m_current; }
    QString state() const;
    qint64 position() const { return m_player.position(); }
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
    QList<qreal> peaks() const { return m_spectrum.peaks(); }
    QString error() const { return m_error; }

    void setVolume(int volume);
    void setShuffle(bool on);
    void setRepeat(bool on);
    void setMuted(bool muted) { m_output.setMuted(muted); }

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

signals:
    void trackChanged();
    void stateChanged();
    void positionChanged();
    void formatChanged();
    void volumeChanged();
    void modesChanged();
    void levelsChanged();
    void errorChanged();
    void seeked(qint64 position);
    // The window asked for a folder. The app connects this to the desktop's
    // folder picker, so the engine needs no D-Bus or portal code.
    void folderRequested();

private:
    void load(int row);
    void readMetaData();
    void setError(const QString &error);
    void saveSoon();
    void save();
    void restore();

    QMediaPlayer m_player;
    QAudioOutput m_output;
    QAudioBufferOutput m_bufferOutput;
    Playlist m_playlist;
    Spectrum m_spectrum;
    QTimer m_spectrumTimer, m_saveTimer;
    QString m_statePath;
    QList<int> m_history;
    int m_current = -1;
    int m_volume = 70;
    bool m_shuffle = false, m_repeat = false;
    bool m_stopped = true;
    QString m_title, m_artist, m_album, m_error;
    int m_bitrate = 0, m_sampleRate = 0, m_channels = 0;
};
