#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <memory>

#include "audioengine.h"
#include "pcmsink.h"

namespace native {
class Ring;
class Reader;
class Worker;
} // namespace native

// Omamp's own engine. A worker thread decodes with FFmpeg, joins tracks
// sample for sample (or crossfades them), applies ReplayGain and the
// equaliser, and writes float stereo at the sink's rate into a ring buffer;
// the sink pulls from it, with volume and pause ramps applied on the way.
//
// Track changes, failures and the end of the queue travel through the ring
// as markers, so the window hears about a track when its first sample is
// played, not when it was decoded.
class NativeEngine : public AudioEngine {
    Q_OBJECT

public:
    // Takes the sink. Without one: the sound card, or a silent BufferSink
    // when OMAMP_NO_AUDIO_DEVICE is set.
    explicit NativeEngine(PcmSink *sink = nullptr, QObject *parent = nullptr);
    ~NativeEngine() override;

    QString name() const override { return QStringLiteral("native"); }
    bool gapless() const override { return true; }
    bool skipsUnplayable() const override { return true; }

    void load(const QString &path, const QUrl &url = {}) override;
    void queueNext(const QString &path, const QUrl &url = {}) override;
    void skipTo(const QString &path, const QUrl &url = {}) override;
    QString source() const override { return m_source; }
    bool buffering() const override { return m_buffering; }

    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 ms) override;

    qint64 position() const override;
    qint64 duration() const override { return m_durationMs; }
    State state() const override { return m_state; }

    void setVolume(qreal linear) override;
    void setMuted(bool muted) override;
    QVariantMap metaData() const override { return m_meta; }
    void setSettings(const Settings &settings) override;

    PcmSink *sink() const { return m_sink.get(); }
    int sampleRate() const { return m_rate; }
    // Delivers markers, position and the analyser tap. A timer calls it;
    // tests that pull a BufferSink by hand call it themselves.
    void poll();
    // Frames the sink has taken from the ring so far.
    qint64 consumedFrames() const;
    // True while the ramp-down after a pause is still sounding.
    bool ramping() const;

    // Short fades, in frames at the output rate: a pause or resume ramps
    // over 10 ms, a seek or skip fades the old audio out over ~5 ms and the
    // new audio in over the same.
    int pauseRampFrames() const { return m_rate / 100; }
    static constexpr int kJumpFadeFrames = 256;
    // Playing with no audio arriving for this long reads as buffering.
    static constexpr int kBufferingAfterMs = 250;

private:
    void setState(State state);
    void setBuffering(bool on);
    void flush(bool keepReached);
    void post(int type, const QString &path = {}, qint64 ms = 0, const QUrl &url = {},
              const QString &fromPath = {});
    void updateTimer();
    // Frames the listener has heard: taken by the sink, less what is still
    // in the device's buffer.
    qint64 heardFrames() const;

    std::unique_ptr<PcmSink> m_sink;
    int m_rate;
    std::unique_ptr<native::Ring> m_ring;
    std::unique_ptr<native::Reader> m_reader;
    std::unique_ptr<native::Worker> m_worker;
    QTimer m_timer;
    State m_state = Stopped;
    QString m_source;
    QVariantMap m_meta;
    qint64 m_durationMs = 0;
    qint64 m_posBaseMs = 0, m_baseFrame = 0, m_lastEmitted = -1;
    quint64 m_gen = 1;
    qint64 m_tapSeen = 0;
    // For buffering: when the sink last took audio.
    qint64 m_lastConsumed = 0;
    QElapsedTimer m_stall;
    bool m_buffering = false;
};
