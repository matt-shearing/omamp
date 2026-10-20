#pragma once

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaPlayer>

#include "audioengine.h"

class OutputFollower;

// The engine Omamp has always used: QMediaPlayer with Qt's FFmpeg backend.
// No gapless playback, crossfade, ReplayGain or equaliser; it moves to the
// queued file when the current one ends, as the Player used to do itself.
class QtEngine : public AudioEngine {
    Q_OBJECT

public:
    // withDevice false leaves out the audio output, so nothing is ever heard
    // (tests). OMAMP_NO_AUDIO_DEVICE=1 forces that too.
    explicit QtEngine(bool withDevice = true, QObject *parent = nullptr);

    QString name() const override { return QStringLiteral("qt"); }
    bool gapless() const override { return false; }
    bool skipsUnplayable() const override { return false; }

    // A valid url (an http(s) stream) is played; path is what is reported.
    void load(const QString &path, const QUrl &url = {}) override;
    void queueNext(const QString &path, const QUrl &url = {}) override {
        m_queued = path;
        m_queuedUrl = url;
    }
    QString source() const override { return m_source; }
    bool buffering() const override { return m_buffering; }

    void play() override { m_player.play(); }
    void pause() override { m_player.pause(); }
    void stop() override { m_player.stop(); }
    void seek(qint64 ms) override { m_player.setPosition(ms); }

    qint64 position() const override { return m_player.position(); }
    qint64 duration() const override { return m_player.duration(); }
    State state() const override;

    void setVolume(qreal linear) override { m_output.setVolume(linear); }
    void setMuted(bool muted) override { m_output.setMuted(muted); }
    QVariantMap metaData() const override;

    bool hasDevice() const { return m_player.audioOutput() != nullptr; }
    // Keeps the audio output on the desktop's default device (null without
    // a device). Tests construct their own with fakes.
    OutputFollower *outputFollower() const { return m_follower; }
    void setOutputFollower(OutputFollower *follower);

private:
    QMediaPlayer m_player;
    QAudioOutput m_output;
    QAudioBufferOutput m_bufferOutput;
    OutputFollower *m_follower = nullptr;
    QString m_source, m_queued;
    QUrl m_queuedUrl;
    bool m_buffering = false;
    int m_sampleRate = 0, m_channels = 0;
};
