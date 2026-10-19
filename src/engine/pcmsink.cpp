#include "pcmsink.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

// QAudioSink reads from a QIODevice; this one asks the engine's reader and
// never runs dry (silence fills an underrun), so the sink never goes idle.
class DeviceSink::Pull : public QIODevice {
public:
    explicit Pull(PcmReader *reader) : m_reader(reader) { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

protected:
    qint64 readData(char *data, qint64 maxlen) override {
        const qint64 frames = maxlen / qint64(2 * sizeof(float));
        if (frames <= 0)
            return 0;
        m_reader->readPcm(reinterpret_cast<float *>(data), frames);
        return frames * qint64(2 * sizeof(float));
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    PcmReader *m_reader;
};

DeviceSink::DeviceSink() {
    // The rate the device runs at, so nothing resamples twice. Reading the
    // preferred format doesn't open the device.
    const int preferred = QMediaDevices::defaultAudioOutput().preferredFormat().sampleRate();
    if (preferred >= 8000 && preferred <= 384000)
        m_rate = preferred;
}

DeviceSink::~DeviceSink() { stop(); }

void DeviceSink::start(PcmReader *reader) {
    if (m_sink)
        return;
    if (qEnvironmentVariableIsSet("OMAMP_NO_AUDIO_DEVICE"))
        qFatal("DeviceSink: refusing to open the sound card under OMAMP_NO_AUDIO_DEVICE");
    QAudioFormat format;
    format.setSampleRate(m_rate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    m_device = std::make_unique<Pull>(reader);
    m_sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format);
    // About 100 ms in the device: enough to ride out a busy desktop, short
    // enough that pause and seek feel immediate.
    m_sink->setBufferSize(qsizetype(m_rate / 10) * 2 * qsizetype(sizeof(float)));
    m_sink->start(m_device.get());
    m_suspended = false;
}

void DeviceSink::suspend() {
    if (m_sink && !m_suspended) {
        m_sink->suspend();
        m_suspended = true;
    }
}

void DeviceSink::resume() {
    if (m_sink && m_suspended) {
        m_sink->resume();
        m_suspended = false;
    }
}

void DeviceSink::stop() {
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_device.reset();
}

BufferSink::BufferSink(int sampleRate, bool capture, QObject *parent)
    : QObject(parent), m_rate(sampleRate), m_capture(capture) {
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, [this] { pull(qint64(m_rate * 0.010 * m_speed)); });
}

qint64 BufferSink::pull(qint64 frames) {
    if (!m_reader)
        return 0;
    frames = qMin(frames, m_reader->available());
    if (frames <= 0)
        return 0;
    m_scratch.resize(frames * 2);
    const qint64 real = m_reader->readPcm(m_scratch.data(), frames);
    if (m_capture && real > 0)
        m_data.append(QVector<float>(m_scratch.constBegin(), m_scratch.constBegin() + real * 2));
    m_pulled += real;
    return real;
}

void BufferSink::setAutoPull(double speed) {
    m_speed = speed;
    updateTimer();
}

void BufferSink::updateTimer() {
    if (m_reader && m_speed > 0 && !m_suspended)
        m_timer.start();
    else
        m_timer.stop();
}
