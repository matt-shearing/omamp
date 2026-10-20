#include "spotifyrequests.h"

#include <QCoreApplication>
#include <QThread>
#include <algorithm>

namespace Spotify {

Requests::Requests(QObject *parent) : QObject(parent) {
    m_pool.setMaxThreadCount(2);
    m_wake.setSingleShot(true);
    connect(&m_wake, &QTimer::timeout, this, &Requests::pump);
    m_slowTimer.setSingleShot(true);
    connect(&m_slowTimer, &QTimer::timeout, this, [this] {
        if (m_running[Data])
            emit slow(m_running[Data]->key);
    });
}

Requests::~Requests() {
    for (const JobPtr &j : m_running)
        if (j)
            j->cancel->store(true);
    m_pool.waitForDone();
}

Requests::JobPtr Requests::find(const QString &key) const {
    if (key.isEmpty())
        return {};
    for (int lane = 0; lane < 2; ++lane) {
        if (m_running[lane] && m_running[lane]->key == key && !m_running[lane]->cancel->load())
            return m_running[lane];
        for (const JobPtr &j : m_waiting[lane])
            if (j->key == key)
                return j;
    }
    return {};
}

bool Requests::has(const QString &key) const { return bool(find(key)); }

bool Requests::idle() const {
    return !m_running[Control] && !m_running[Data] && m_waiting[Control].isEmpty() && m_waiting[Data].isEmpty();
}

void Requests::submit(Lane lane, const QString &key, Work work, QObject *context, Done done) {
    if (JobPtr same = find(key)) {
        same->callers << qMakePair(QPointer<QObject>(context), std::move(done));
        return;
    }
    auto job = std::make_shared<Job>();
    job->lane = lane;
    job->key = key;
    job->work = std::move(work);
    job->callers << qMakePair(QPointer<QObject>(context), std::move(done));
    m_waiting[lane] << job;
    emit activityChanged();
    pump();
}

void Requests::cancel(const QString &key) {
    if (key.isEmpty())
        return;
    for (int lane = 0; lane < 2; ++lane) {
        for (int i = m_waiting[lane].size() - 1; i >= 0; --i)
            if (m_waiting[lane].at(i)->key == key) {
                const JobPtr j = m_waiting[lane].takeAt(i);
                Result r;
                r.cancelled = true;
                r.error = tr("cancelled");
                for (const auto &c : j->callers)
                    if (c.first && c.second)
                        c.second(r);
            }
        if (m_running[lane] && m_running[lane]->key == key)
            m_running[lane]->cancel->store(true);
    }
    emit activityChanged();
}

void Requests::cancelAll() {
    for (int lane = 0; lane < 2; ++lane) {
        m_waiting[lane].clear();
        if (m_running[lane]) {
            m_running[lane]->cancel->store(true);
            m_running[lane]->callers.clear();
        }
    }
    emit activityChanged();
}

void Requests::holdData(int ms) {
    const qint64 left = heldForMs();
    if (ms <= left)
        return;
    m_holdMs = ms;
    m_holdClock.restart();
    emit activityChanged();
}

qint64 Requests::heldForMs() const {
    if (!m_holdClock.isValid())
        return 0;
    return qMax<qint64>(0, m_holdMs - m_holdClock.elapsed());
}

void Requests::pump() {
    if (!m_running[Control] && !m_waiting[Control].isEmpty())
        start(m_waiting[Control].takeFirst());
    if (!m_running[Data] && !m_waiting[Data].isEmpty()) {
        qint64 wait = heldForMs();
        if (m_lastDataStart.isValid())
            wait = qMax(wait, m_gapMs - m_lastDataStart.elapsed());
        if (wait > 0) {
            if (!m_wake.isActive() || m_wake.remainingTime() > wait)
                m_wake.start(int(wait));
        } else {
            m_lastDataStart.restart();
            start(m_waiting[Data].takeFirst());
        }
    }
}

void Requests::start(const JobPtr &job) {
    m_running[job->lane] = job;
    ++m_started;
    if (job->lane == Data && m_slowAfterMs > 0)
        m_slowTimer.start(m_slowAfterMs);
    auto backend = m_backend;
    QPointer<Requests> self(this);
    m_pool.start([self, job, backend] {
        Result r;
        QElapsedTimer t;
        t.start();
        {
            CancelScope scope(job->cancel);
            if (backend && !job->cancel->load())
                job->work(*backend, r);
        }
        r.elapsedMs = t.elapsed();
        if (job->cancel->load()) {
            r.cancelled = true;
            if (r.error.isEmpty())
                r.error = tr("cancelled");
        }
        QMetaObject::invokeMethod(
            self,
            [self, job, r] {
                if (self)
                    self->finish(job, r);
            },
            Qt::QueuedConnection);
    });
}

void Requests::finish(const JobPtr &job, Result result) {
    if (m_running[job->lane] == job)
        m_running[job->lane].reset();
    if (job->lane == Data)
        m_slowTimer.stop();
    for (const auto &c : job->callers)
        if (c.first && c.second)
            c.second(result);
    emit activityChanged();
    pump();
}

void Requests::waitForIdle(int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!idle() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    m_pool.waitForDone();
    QCoreApplication::processEvents();
}

} // namespace Spotify
