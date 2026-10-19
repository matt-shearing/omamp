#pragma once

#include <QObject>
#include <QString>
#include <atomic>

// A long piece of background work (scan, lookup, organise, sync) with
// progress and cancel. The window lists these in its jobs panel; the CLI
// prints their progress to stderr.
//
// Progress may be reported from a worker thread: the signals then arrive
// queued in the receiver's thread. cancel() is safe from any thread.
class Job : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString kind READ kind CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY progressChanged)
    Q_PROPERTY(qint64 done READ done NOTIFY progressChanged)
    Q_PROPERTY(qint64 total READ total NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit Job(const QString &kind, QObject *parent = nullptr) : QObject(parent), m_kind(kind) {}

    QString kind() const { return m_kind; }
    // queued, running, done, failed or cancelled, as in the jobs table.
    QString state() const { return m_state; }
    QString phase() const { return m_phase; }
    qint64 done() const { return m_done; }
    // 0 while the amount of work is still unknown.
    qint64 total() const { return m_total; }
    QString error() const { return m_error; }

    Q_INVOKABLE void cancel() { m_cancelled = true; }
    bool isCancelled() const { return m_cancelled; }

    void start();
    void setProgress(const QString &phase, qint64 done, qint64 total);
    void finish();
    void fail(const QString &error);

signals:
    void stateChanged();
    void progressChanged(const QString &phase, qint64 done, qint64 total);
    void finished();

private:
    QString m_kind, m_state = "queued", m_phase, m_error;
    qint64 m_done = 0, m_total = 0;
    std::atomic_bool m_cancelled = false;
};
