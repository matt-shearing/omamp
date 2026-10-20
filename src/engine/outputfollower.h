#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

class QMediaDevices;
class QProcess;

// Keeping Omamp's audio on the desktop's default output.
//
// Qt pins every stream to one device: its PipeWire backend connects with a
// target node and PW_STREAM_FLAG_DONT_RECONNECT, so PipeWire never moves the
// stream for us, and Qt's own idea of the default can go stale after a
// device leaves and comes back. So the default is read from the sound
// server itself (DefaultOutputSource), matched by name against the outputs
// Qt can open (OutputList), and the follower says which device the stream
// should be on. The engine does the reopening. "Following the default
// output" in docs/plan/progress.md has the reasoning.

// The sound server's view of the default output.
class DefaultOutputSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    // The default sink's name (the same string Qt uses as QAudioDevice::id()),
    // or empty when the server can't be asked.
    virtual QString defaultSinkName() = 0;
    // A long-running watch for changes to the default and to the set of
    // outputs. changed() fires for each; watchStopped() when it dies.
    virtual void startWatching() = 0;
    virtual void stopWatching() = 0;
    virtual bool watching() const = 0;

signals:
    void changed();
    void watchStopped();
};

// `pactl`, which talks to PipeWire through pipewire-pulse (and to a real
// PulseAudio): `pactl get-default-sink` for the name, and `pactl subscribe`
// for server changes (a new default) and sinks coming and going.
class PactlDefaultSource : public DefaultOutputSource {
    Q_OBJECT

public:
    explicit PactlDefaultSource(QObject *parent = nullptr);
    ~PactlDefaultSource() override;
    QString defaultSinkName() override;
    void startWatching() override;
    void stopWatching() override;
    bool watching() const override;

    // What one line of `pactl subscribe` means to the follower: a server
    // change (the default moved), a sink added or removed, or nothing.
    enum class Event { None, Server, Sinks };
    static Event classify(const QByteArray &line);

private:
    QProcess *m_watch = nullptr;
    QByteArray m_partial;
};

// The outputs Qt can open, by QAudioDevice::id().
class OutputList : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    virtual QList<QByteArray> outputIds() = 0;
    // QMediaDevices::defaultAudioOutput(): the fallback when the server
    // can't be asked.
    virtual QByteArray qtDefaultId() = 0;
    // Asks Qt again, in case its list has gone stale.
    virtual void refresh() = 0;

signals:
    void changed();
};

// QMediaDevices. refresh() recreates the QMediaDevices object; Qt keeps one
// device monitor per process, so that only helps when the stale part is the
// object's own state, and the follower's retries do the rest.
class QtOutputList : public OutputList {
    Q_OBJECT

public:
    explicit QtOutputList(QObject *parent = nullptr);
    QList<QByteArray> outputIds() override;
    QByteArray qtDefaultId() override;
    void refresh() override;

private:
    QMediaDevices *m_devices = nullptr;
};

class OutputFollower : public QObject {
    Q_OBJECT

public:
    // Takes ownership of both.
    OutputFollower(OutputList *outputs, DefaultOutputSource *source, QObject *parent = nullptr);
    ~OutputFollower() override;

    // A follower for the real desktop: QMediaDevices and pactl.
    static OutputFollower *createForDesktop(QObject *parent = nullptr);

    // Watching only while a stream is open, so an idle player runs no
    // pactl. Turning it on checks at once.
    void setActive(bool active);
    bool active() const { return m_active; }

    // The device the stream is on now; empty when it is on none (it failed
    // or was lost), which makes the next check reopen it.
    void setCurrent(const QByteArray &id) { m_current = id; }
    QByteArray current() const { return m_current; }

    // The device the stream should be on: the server's default when Qt
    // lists it; otherwise the current device while Qt still lists it (the
    // default will show up, and a retry is scheduled); otherwise Qt's
    // default, or any output. Empty when Qt lists nothing.
    QByteArray resolve();

    // Asks for a check shortly, and again a little later: devices often
    // appear in two steps (the node, then its formats and the default).
    void checkSoon();
    // Resolves now and emits switchTo() if the stream should move.
    void checkNow();

    struct Timing {
        int debounceMs = 150;   // after any change
        int secondMs = 2000;    // the second look after a change
        int pollMs = 3000;      // while the server can't be watched
        QList<int> retryMs = {500, 1000, 2000, 4000, 8000, 15000}; // default not yet listed by Qt
        int restartMinMs = 1000; // the watch's restart back-off, doubling
        int restartMaxMs = 30000;
    };
    void setTiming(const Timing &timing);

    OutputList *outputs() const { return m_outputs; }
    DefaultOutputSource *source() const { return m_source; }

signals:
    // Open the stream on this device (an id from OutputList::outputIds()).
    void switchTo(const QByteArray &id);

private:
    void startWatch();
    void watchStopped();
    void scheduleRetry();
    void updatePoll();

    OutputList *m_outputs;
    DefaultOutputSource *m_source;
    Timing m_timing;
    bool m_active = false;
    QByteArray m_current;
    QTimer m_debounce, m_second, m_poll, m_retry, m_restart;
    int m_retries = 0;
    int m_restartDelay = 0;
    bool m_warnedStale = false;
};
