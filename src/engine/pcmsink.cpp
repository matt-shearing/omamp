#include "pcmsink.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include "outputfollower.h"

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
        pulled += frames;
        return frames * qint64(2 * sizeof(float));
    }
    qint64 writeData(const char *, qint64) override { return -1; }

public:
    // Frames handed to the current QAudioSink since it started. QAudioSink
    // may pull on a thread of its own.
    std::atomic<qint64> pulled{0};

private:
    PcmReader *m_reader;
};

DeviceSink::DeviceSink() {
    // The rate the device runs at, so nothing resamples twice. Reading the
    // preferred format doesn't open the device.
    const int preferred = QMediaDevices::defaultAudioOutput().preferredFormat().sampleRate();
    if (preferred >= 8000 && preferred <= 384000)
        m_rate = preferred;
    // A new default output (headphones, a Bluetooth speaker, a dongle that
    // comes back) takes over the stream while it is open.
    m_follower = OutputFollower::createForDesktop(this);
    connect(m_follower, &OutputFollower::switchTo, this, [this](const QByteArray &id) {
        if (m_sink)
            openOnId(id);
    });
}

DeviceSink::~DeviceSink() { stop(); }

bool DeviceSink::start(PcmReader *reader) {
    if (m_sink)
        return true;
    if (qEnvironmentVariableIsSet("THRUM_NO_AUDIO_DEVICE"))
        qFatal("DeviceSink: refusing to open the sound card under THRUM_NO_AUDIO_DEVICE");
    m_reader = reader;
    m_suspended = false;
    m_errorReopens = 0;
    m_follower->setCurrent({});
    const QByteArray target = m_follower->resolve();
    const bool opened = target.isEmpty() ? openOn(QMediaDevices::defaultAudioOutput()) : openOnId(target);
    if (opened)
        m_follower->setActive(true);
    return opened;
}

bool DeviceSink::openOnId(const QByteArray &id) {
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice &device : outputs)
        if (device.id() == id)
            return openOn(device);
    // Gone between the check and now; the next change tries again.
    return m_sink != nullptr;
}

bool DeviceSink::openOn(const QAudioDevice &device) {
    if (qEnvironmentVariableIsSet("THRUM_NO_AUDIO_DEVICE"))
        qFatal("DeviceSink: refusing to open the sound card under THRUM_NO_AUDIO_DEVICE");
    closeSink();
    if (device.isNull())
        return false;
    QAudioFormat format;
    // The engine's rate stays as it was; a device that prefers another one
    // is resampled by PipeWire.
    format.setSampleRate(m_rate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    if (!m_device)
        m_device = std::make_unique<Pull>(m_reader);
    m_device->pulled = 0;
    m_sink = new QAudioSink(device, format);
    m_deviceId = device.id();
    m_follower->setCurrent(m_deviceId);
    // Qt pins the stream to this device and tells PipeWire not to move it,
    // so when the device goes away the stream just fails. Mark it as on no
    // device and let the follower pick the new default.
    QAudioSink *sink = m_sink;
    connect(sink, &QAudioSink::stateChanged, this, [this, sink](QtAudio::State state) {
        if (sink != m_sink)
            return;
        if (state == QtAudio::ActiveState) {
            m_errorReopens = 0;
            return;
        }
        const QtAudio::Error error = sink->error();
        if (state != QtAudio::StoppedState || (error != QtAudio::IOError && error != QtAudio::FatalError))
            return;
        if (m_errorReopens++ < 3)
            m_follower->setCurrent({});
        m_follower->checkSoon();
    });
    // About 100 ms in the device: enough to ride out a busy desktop, short
    // enough that pause and seek feel immediate.
    m_sink->setBufferSize(qsizetype(m_rate / 10) * 2 * qsizetype(sizeof(float)));
    m_sink->start(m_device.get());
    if (m_sink->error() == QtAudio::OpenError) {
        stop();
        return false;
    }
    if (m_suspended)
        m_sink->suspend();
    return true;
}

bool DeviceSink::followDefaultOutput() {
    if (!m_sink)
        return true;
    const QByteArray target = m_follower->resolve();
    if (target.isEmpty() || target == m_deviceId)
        return true;
    // The audio still in the old device's buffer (at most 100 ms) is lost;
    // the reader carries on where it was.
    return openOnId(target);
}

void DeviceSink::closeSink() {
    if (m_sink) {
        QAudioSink *sink = m_sink;
        m_sink = nullptr;
        sink->disconnect(this);
        sink->stop();
        delete sink;
    }
    m_deviceId.clear();
}

qint64 DeviceSink::queuedEstimate(qint64 pulledFrames, qint64 processedUSecs, int rate) {
    const qint64 heard = processedUSecs * rate / 1000000;
    return qBound<qint64>(qint64(0), pulledFrames - heard, qint64(rate / 2));
}

qint64 DeviceSink::queuedFrames() const {
    if (!m_sink || !m_device)
        return 0;
    return queuedEstimate(m_device->pulled.load(), m_sink->processedUSecs(), m_rate);
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
    if (m_follower) {
        m_follower->setActive(false);
        m_follower->setCurrent({});
    }
    closeSink();
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
