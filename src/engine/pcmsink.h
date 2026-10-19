#pragma once

#include <QIODevice>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <memory>

class QAudioSink;

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
    virtual void start(PcmReader *reader) = 0;
    virtual bool started() const = 0;
    // Stops pulling while paused, so an idle player lets the device sleep.
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
};

// The sound card through QAudioSink (PipeWire on Omarchy), in pull mode.
// Refuses to start when OMAMP_NO_AUDIO_DEVICE is set, so a test can never
// make a sound by mistake.
class DeviceSink : public PcmSink {
public:
    DeviceSink();
    ~DeviceSink() override;
    QString kind() const override { return QStringLiteral("device"); }
    int sampleRate() const override { return m_rate; }
    void start(PcmReader *reader) override;
    bool started() const override { return m_sink != nullptr; }
    void suspend() override;
    void resume() override;
    void stop() override;

private:
    class Pull;
    int m_rate = 48000;
    std::unique_ptr<Pull> m_device;
    QAudioSink *m_sink = nullptr;
    bool m_suspended = false;
};

// Audio pulled into memory as fast as asked, never to a device. Tests pull
// by hand (pull()) or let a timer pull faster than real time.
class BufferSink : public QObject, public PcmSink {
    Q_OBJECT

public:
    explicit BufferSink(int sampleRate = 48000, bool capture = true, QObject *parent = nullptr);
    QString kind() const override { return QStringLiteral("buffer"); }
    int sampleRate() const override { return m_rate; }
    void start(PcmReader *reader) override { m_reader = reader; updateTimer(); }
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
