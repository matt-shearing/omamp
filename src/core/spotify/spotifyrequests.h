#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <memory>

#include "spotify.h"

// One queue for every command Omamp sends to the Spotify backend.
//
// Each spotify_player command is a Spotify Web API request, and the Web API
// answers a burst with "429 Too Many Requests". So commands never run in a
// burst: there are two lanes, each running one command at a time. The
// control lane carries play, pause, seek and the playback state, so a
// button press never waits behind a long list. The data lane carries lists
// and searches, spaced a little apart, and can be held back while Spotify
// asks Omamp to wait. A request with the same key as one already queued or
// running joins it instead of running again.
namespace Spotify {

// What a command produced. One shape for all of them keeps the queue simple.
struct Result {
    QString error;
    bool cancelled = false;
    qint64 elapsedMs = 0;
    QList<Item> items;
    QStringList sections;
    SearchResults search;
    Playback playback;
    QList<Device> devices;
    QVariantMap extra;
    bool ok() const { return error.isEmpty() && !cancelled; }
};

class Requests : public QObject {
    Q_OBJECT
public:
    enum Lane { Control, Data };
    using Work = std::function<void(Backend &, Result &)>;
    using Done = std::function<void(const Result &)>;

    explicit Requests(QObject *parent = nullptr);
    ~Requests() override;

    void setBackend(std::shared_ptr<Backend> backend) { m_backend = std::move(backend); }

    // Queues work. done runs on this object's thread when it finishes,
    // unless context has gone. key: requests with the same key while one
    // is waiting or running share its answer; empty never shares.
    void submit(Lane lane, const QString &key, Work work, QObject *context, Done done);
    // Stops a waiting or running request: its callers get a cancelled
    // Result.
    void cancel(const QString &key);
    // Drops every waiting request without answering it and stops the
    // running ones (a new backend, or shutting down).
    void cancelAll();
    // Whether a request with this key is waiting or running.
    bool has(const QString &key) const;
    // How long the data lane waits between two commands, and holding it
    // back (Spotify's rate limit, or spotify_player's own burst of
    // requests just after it starts).
    void setGapMs(int ms) { m_gapMs = ms; }
    int gapMs() const { return m_gapMs; }
    void holdData(int ms);
    qint64 heldForMs() const;
    // A data request running longer than this is slow: slow() is emitted.
    void setSlowAfterMs(int ms) { m_slowAfterMs = ms; }
    // How many commands ran (tests).
    int started() const { return m_started; }
    bool idle() const;
    // Runs the event loop until nothing waits or runs (tests, shutdown).
    void waitForIdle(int timeoutMs = 60000);

signals:
    void slow(const QString &key);
    void activityChanged();

private:
    struct Job {
        Lane lane = Data;
        QString key;
        Work work;
        QList<QPair<QPointer<QObject>, Done>> callers;
        CancelFlag cancel = makeCancelFlag();
    };
    using JobPtr = std::shared_ptr<Job>;

    void pump();
    void start(const JobPtr &job);
    void finish(const JobPtr &job, Result result);
    JobPtr find(const QString &key) const;

    std::shared_ptr<Backend> m_backend;
    QThreadPool m_pool;
    QList<JobPtr> m_waiting[2];
    JobPtr m_running[2];
    int m_gapMs = 300;
    int m_slowAfterMs = 4000;
    int m_started = 0;
    QElapsedTimer m_lastDataStart;
    QElapsedTimer m_holdClock;
    qint64 m_holdMs = 0;
    QTimer m_wake, m_slowTimer;
};

} // namespace Spotify
