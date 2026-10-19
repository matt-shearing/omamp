#pragma once

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaPlayer>

#include "audioengine.h"

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

    void load(const QString &path) override;
    void queueNext(const QString &path) override { m_queued = path; }
    QString source() const override { return m_player.source().toLocalFile(); }

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

private:
    QMediaPlayer m_player;
    QAudioOutput m_output;
    QAudioBufferOutput m_bufferOutput;
    QString m_queued;
    int m_sampleRate = 0, m_channels = 0;
};
