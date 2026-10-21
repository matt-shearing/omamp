#include "outputfollower.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtGlobal>

namespace {
QProcessEnvironment pactlEnvironment() {
    // pactl's messages are translated; the follower reads only names and
    // event lines, but keep them in one language anyway.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    return env;
}
} // namespace

PactlDefaultSource::PactlDefaultSource(QObject *parent) : DefaultOutputSource(parent) {}

PactlDefaultSource::~PactlDefaultSource() { stopWatching(); }

QString PactlDefaultSource::defaultSinkName() {
    // A few milliseconds normally. The timeout keeps a wedged sound server
    // from holding the window for long; the follower then falls back to Qt.
    QProcess pactl;
    pactl.setProcessEnvironment(pactlEnvironment());
    pactl.start(QStringLiteral("pactl"), {QStringLiteral("get-default-sink")}, QIODevice::ReadOnly);
    if (!pactl.waitForStarted(300))
        return {};
    if (!pactl.waitForFinished(500)) {
        pactl.kill();
        pactl.waitForFinished(100);
        return {};
    }
    if (pactl.exitStatus() != QProcess::NormalExit || pactl.exitCode() != 0)
        return {};
    return QString::fromUtf8(pactl.readAllStandardOutput()).trimmed();
}

PactlDefaultSource::Event PactlDefaultSource::classify(const QByteArray &line) {
    // "Event 'change' on server #-1" when the default changes;
    // "Event 'new' on sink #183" and "Event 'remove' on sink #183" as
    // outputs come and go. Sink volume changes ("'change' on sink") and
    // every other facility (clients, sink-inputs) are ignored.
    if (!line.startsWith("Event '"))
        return Event::None;
    if (line.contains(" on server"))
        return Event::Server;
    if (line.contains(" on sink #") && (line.startsWith("Event 'new'") || line.startsWith("Event 'remove'")))
        return Event::Sinks;
    return Event::None;
}

void PactlDefaultSource::startWatching() {
    if (m_watch)
        return;
    m_partial.clear();
    auto *watch = new QProcess(this);
    m_watch = watch;
    watch->setProcessEnvironment(pactlEnvironment());
    connect(watch, &QProcess::readyReadStandardOutput, this, [this, watch] {
        if (watch != m_watch)
            return;
        m_partial += watch->readAllStandardOutput();
        bool relevant = false;
        qsizetype end;
        while ((end = m_partial.indexOf('\n')) >= 0) {
            if (classify(m_partial.left(end)) != Event::None)
                relevant = true;
            m_partial.remove(0, end + 1);
        }
        // One signal per batch: a device arriving is a burst of lines.
        if (relevant)
            emit changed();
    });
    auto died = [this, watch] {
        if (watch != m_watch)
            return;
        m_watch = nullptr;
        watch->deleteLater();
        emit watchStopped();
    };
    connect(watch, &QProcess::finished, this, died);
    connect(watch, &QProcess::errorOccurred, this, [died](QProcess::ProcessError error) {
        // A crash or a kill also ends in finished(); only a failed start
        // ends here alone.
        if (error == QProcess::FailedToStart)
            died();
    });
    watch->start(QStringLiteral("pactl"), {QStringLiteral("subscribe")}, QIODevice::ReadOnly);
}

void PactlDefaultSource::stopWatching() {
    if (!m_watch)
        return;
    QProcess *watch = m_watch;
    m_watch = nullptr;
    watch->disconnect(this);
    watch->kill();
    watch->waitForFinished(200);
    delete watch;
}

bool PactlDefaultSource::watching() const { return m_watch != nullptr; }

QtOutputList::QtOutputList(QObject *parent) : OutputList(parent) { refresh(); }

QList<QByteArray> QtOutputList::outputIds() {
    QList<QByteArray> ids;
    for (const QAudioDevice &device : QMediaDevices::audioOutputs())
        ids.append(device.id());
    return ids;
}

QByteArray QtOutputList::qtDefaultId() { return QMediaDevices::defaultAudioOutput().id(); }

void QtOutputList::refresh() {
    delete m_devices;
    m_devices = new QMediaDevices(this);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this, &OutputList::changed);
}

OutputFollower::OutputFollower(OutputList *outputs, DefaultOutputSource *source, QObject *parent)
    : QObject(parent), m_outputs(outputs), m_source(source) {
    m_outputs->setParent(this);
    m_source->setParent(this);
    for (QTimer *timer : {&m_debounce, &m_second, &m_retry, &m_restart})
        timer->setSingleShot(true);
    setTiming(Timing{});
    connect(m_outputs, &OutputList::changed, this, &OutputFollower::checkSoon);
    connect(m_source, &DefaultOutputSource::changed, this, [this] {
        // The watch is alive and talking, so its next failure starts the
        // back-off from the beginning.
        m_restartDelay = 0;
        checkSoon();
    });
    connect(m_source, &DefaultOutputSource::watchStopped, this, &OutputFollower::watchStopped);
    connect(&m_debounce, &QTimer::timeout, this, &OutputFollower::checkNow);
    connect(&m_second, &QTimer::timeout, this, &OutputFollower::checkNow);
    connect(&m_retry, &QTimer::timeout, this, &OutputFollower::checkNow);
    connect(&m_poll, &QTimer::timeout, this, &OutputFollower::checkNow);
    connect(&m_restart, &QTimer::timeout, this, &OutputFollower::startWatch);
}

OutputFollower::~OutputFollower() { setActive(false); }

OutputFollower *OutputFollower::createForDesktop(QObject *parent) {
    return new OutputFollower(new QtOutputList, new PactlDefaultSource, parent);
}

void OutputFollower::setTiming(const Timing &timing) {
    m_timing = timing;
    m_debounce.setInterval(timing.debounceMs);
    m_second.setInterval(timing.secondMs);
    m_poll.setInterval(timing.pollMs);
}

void OutputFollower::setActive(bool active) {
    if (active == m_active)
        return;
    m_active = active;
    if (!active) {
        for (QTimer *timer : {&m_debounce, &m_second, &m_poll, &m_retry, &m_restart})
            timer->stop();
        m_source->stopWatching();
        m_retries = 0;
        m_restartDelay = 0;
        return;
    }
    startWatch();
    checkNow();
}

void OutputFollower::startWatch() {
    if (!m_active)
        return;
    m_source->startWatching();
    updatePoll();
}

void OutputFollower::watchStopped() {
    if (!m_active)
        return;
    // pactl exits when pipewire-pulse restarts; start it again, backing
    // off if it keeps dying, and poll in the meantime.
    m_restartDelay = m_restartDelay <= 0 ? m_timing.restartMinMs
                                         : qMin(m_restartDelay * 2, m_timing.restartMaxMs);
    m_restart.start(m_restartDelay);
    updatePoll();
    checkSoon();
}

void OutputFollower::updatePoll() {
    if (m_active && !m_source->watching()) {
        if (!m_poll.isActive())
            m_poll.start();
    } else {
        m_poll.stop();
    }
}

void OutputFollower::checkSoon() {
    if (!m_active)
        return;
    // A fresh change gets a fresh series of retries.
    m_retries = 0;
    m_retry.stop();
    m_debounce.start();
    m_second.start();
}

void OutputFollower::checkNow() {
    if (!m_active)
        return;
    const QByteArray target = resolve();
    if (!target.isEmpty() && target != m_current)
        emit switchTo(target);
}

void OutputFollower::scheduleRetry() {
    if (m_retry.isActive() || !m_active)
        return;
    if (m_retries < m_timing.retryMs.size()) {
        m_retry.start(m_timing.retryMs.at(m_retries++));
    } else if (!m_warnedStale) {
        m_warnedStale = true;
        qWarning("Thrum: the sound server's default output isn't among Qt's outputs; staying on the current one");
    }
}

QByteArray OutputFollower::resolve() {
    QList<QByteArray> ids = m_outputs->outputIds();
    const QByteArray name = m_source->defaultSinkName().toUtf8();
    if (!name.isEmpty()) {
        if (!ids.contains(name)) {
            // Qt hasn't caught up with the server: ask it again now, and
            // later if that doesn't help.
            m_outputs->refresh();
            ids = m_outputs->outputIds();
        }
        if (ids.contains(name)) {
            m_retry.stop();
            m_retries = 0;
            m_warnedStale = false;
            return name;
        }
        scheduleRetry();
        // The default will appear; until then, stay where the audio is
        // heard if that device is still there.
        if (!m_current.isEmpty() && ids.contains(m_current))
            return m_current;
    }
    const QByteArray qtDefault = m_outputs->qtDefaultId();
    if (!qtDefault.isEmpty() && ids.contains(qtDefault))
        return qtDefault;
    if (!m_current.isEmpty() && ids.contains(m_current))
        return m_current;
    return ids.isEmpty() ? QByteArray() : ids.first();
}
