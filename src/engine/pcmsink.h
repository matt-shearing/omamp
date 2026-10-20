#pragma once

#include <QIODevice>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <memory>

class QAudioDevice;
class QAudioSink;
class OutputFollower;

// Where the native engine's audio goes. The engine hands the sink a reader;
// the sink pulls interleaved float stereo from it at its own pace.
class PcmReader {
public:
    virtual ~PcmReader() = default;
    // Fills all of out (frames × 2 floats), padding with silence, and
    // returns how many frames were real audio.
    virtual qint64 readPcm(float *out, qint64 frames) = 0;
    // Frames ready to read right now.
    virtual qint64 available() = 0;
};

class PcmSink {
public:
    virtual ~PcmSink() = default;
    // "device" for the sound card, "buffer" for memory (tests, benchmarks).
    virtual QString kind() const = 0;
    virtual int sampleRate() const = 0;
    // False if the output could not be opened.
    virtual bool start(PcmReader *reader) = 0;
    virtual bool started() const = 0;
    // Stops pulling while paused, so an idle player lets the device sleep.
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    // Frames already taken from the reader but not yet heard: the audio
    // waiting in the device's buffer. The engine takes them off its
    // position, so the clock and track changes follow the speakers.
    virtual qint64 queuedFrames() const { return 0; }
};

// The sound card through QAudioSink (PipeWire on Omarchy), in pull mode.
// Refuses to start when OMAMP_NO_AUDIO_DEVICE is set, so a test can never
// make a sound by mistake.
//
// It follows the default output: when the desktop's default device changes
// (headphones plugged in, a Bluetooth speaker connecting or leaving), the
// stream is opened again on the new default, at the same rate, and carries
// on from the same reader. The default comes from the sound server, not
// Qt's cache, through an OutputFollower (outputfollower.h).
class DeviceSink : public QObject, public PcmSink {
    Q_OBJECT

public:
    DeviceSink();
    ~DeviceSink() override;
    QString kind() const override { return QStringLiteral("device"); }
    int sampleRate() const override { return m_rate; }
    bool start(PcmReader *reader) override;
    bool started() const override { return m_sink != nullptr; }
    void suspend() override;
    void resume() override;
    void stop() override;
    qint64 queuedFrames() const override;

    // Opens the stream again on the current default output, if it differs
    // from the one in use. Returns false if the new device won't open.
    bool followDefaultOutput();
    // The device the stream is on (QAudioDevice::id()), empty when closed.
    QByteArray deviceId() const { return m_deviceId; }

    // The latency estimate on its own: frames pulled since the stream
    // started, less what the device says it has processed, kept between
    // zero and half a second. Public for the tests.
    static qint64 queuedEstimate(qint64 pulledFrames, qint64 processedUSecs, int rate);

private:
    class Pull;
    bool openOn(const QAudioDevice &device);
    bool openOnId(const QByteArray &id);
    void closeSink();

    int m_rate = 48000;
    PcmReader *m_reader = nullptr;
    std::unique_ptr<Pull> m_device;
    QAudioSink *m_sink = nullptr;
    QByteArray m_deviceId;
    OutputFollower *m_follower = nullptr;
    bool m_suspended = false;
    // Reopens after the stream failed on a device that is still listed; a
    // device that keeps failing is left alone until the next change.
    int m_errorReopens = 0;
};

// Wraps another sink and reports a fixed amount of audio as still in the
// device: the tests' stand-in for a sound card's buffer.
class LatencySink : public PcmSink {
public:
    LatencySink(PcmSink *inner, qint64 queued) : m_inner(inner), m_queued(queued) {}
    QString kind() const override { return m_inner->kind(); }
    int sampleRate() const override { return m_inner->sampleRate(); }
    bool start(PcmReader *reader) override { return m_inner->start(reader); }
    bool started() const override { return m_inner->started(); }
    void suspend() override { m_inner->suspend(); }
    void resume() override { m_inner->resume(); }
    void stop() override { m_inner->stop(); }
    qint64 queuedFrames() const override { return m_queued; }
    void setQueuedFrames(qint64 frames) { m_queued = frames; }
    PcmSink *inner() const { return m_inner.get(); }

private:
    std::unique_ptr<PcmSink> m_inner;
    qint64 m_queued;
};

// Audio pulled into memory as fast as asked, never to a device. Tests pull
// by hand (pull()) or let a timer pull faster than real time.
class BufferSink : public QObject, public PcmSink {
    Q_OBJECT

public:
    explicit BufferSink(int sampleRate = 48000, bool capture = true, QObject *parent = nullptr);
    QString kind() const override { return QStringLiteral("buffer"); }
    int sampleRate() const override { return m_rate; }
    bool start(PcmReader *reader) override {
        m_reader = reader;
        updateTimer();
        return true;
    }
    bool started() const override { return m_reader != nullptr; }
    void suspend() override { m_suspended = true; updateTimer(); }
    void resume() override { m_suspended = false; updateTimer(); }
    void stop() override { m_reader = nullptr; updateTimer(); }

    // Pulls up to frames frames and returns how many were real audio. Only
    // what is ready is pulled, like a device that never runs dry; a test
    // waits and pulls again. Works while suspended too.
    qint64 pull(qint64 frames);
    // Pulls on a timer at speed × real time (0 stops).
    void setAutoPull(double speed);

    bool suspended() const { return m_suspended; }
    const QVector<float> &data() const { return m_data; }
    qint64 frames() const { return m_data.size() / 2; }
    qint64 pulledFrames() const { return m_pulled; }
    void clear() { m_data.clear(); }

private:
    void updateTimer();

    int m_rate;
    bool m_capture, m_suspended = false;
    double m_speed = 0;
    PcmReader *m_reader = nullptr;
    QVector<float> m_data, m_scratch;
    qint64 m_pulled = 0;
    QTimer m_timer;
};
