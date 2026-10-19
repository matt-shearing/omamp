#include "job.h"

void Job::start() {
    m_state = "running";
    emit stateChanged();
}

void Job::setProgress(const QString &phase, qint64 done, qint64 total) {
    m_phase = phase;
    m_done = done;
    m_total = total;
    emit progressChanged(phase, done, total);
}

void Job::finish() {
    m_state = m_cancelled ? "cancelled" : "done";
    emit stateChanged();
    emit finished();
}

void Job::fail(const QString &error) {
    m_state = "failed";
    m_error = error;
    emit stateChanged();
    emit finished();
}
