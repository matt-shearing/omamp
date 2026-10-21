#pragma once

#include <functional>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

class QLocalServer;
class QLocalSocket;

// Shares the analyser with the Omarchy bar plugin (omarchy-plugin/), so the
// bar can draw a small copy of the visualiser behind the track's name. The
// bar has only MPRIS, which carries no sound, so Thrum sends the levels.
//
// A Unix socket at $XDG_RUNTIME_DIR/thrum-levels.sock. The plugin connects
// with Quickshell's Socket and reads one JSON line per frame:
//
//   {"style":"synthwave","reduceMotion":false,"active":true,"levels":[0..100 x16]}
//
// Nothing is sent and no timer runs while nobody is connected or nothing
// plays through Thrum (paused, stopped, Spotify). A client gets one frame on
// connecting, one when playing stops ("active":false, no levels), one when
// the style or Reduce motion changes, and up to 20 a second while music
// plays (10 with Reduce motion); a frame identical to the last is skipped.
// Writes never wait: a client that stops reading is dropped.
class LevelsFeed : public QObject {
    Q_OBJECT
    // Settings > Appearance > Share the visualiser with the Omarchy bar.
    // Kept in window.conf under [appearance] as shareWithBar.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int clients READ clients NOTIFY clientsChanged)

public:
    // settingsPath: window.conf; empty keeps the setting in memory (tests).
    // socketPath: empty uses defaultSocketPath().
    explicit LevelsFeed(const QString &settingsPath = {}, const QString &socketPath = {}, QObject *parent = nullptr);
    ~LevelsFeed() override;

    static constexpr int bands = 16;
    static constexpr int intervalMs = 50;        // 20 frames a second
    static constexpr int calmIntervalMs = 100;   // 10 with Reduce motion
    static constexpr qint64 maxBacklog = 64 * 1024;

    static QString defaultSocketPath();
    // The analyser's bands (24) folded to `count`, each 0..100.
    static QList<int> shrink(const QList<qreal> &levels, int count = bands);
    // One line of the feed, newline included.
    static QByteArray frame(const QString &style, bool reduceMotion, bool active, const QList<int> &levels);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    int clients() const { return m_clients.size(); }
    QString socketPath() const { return m_socketPath; }
    bool listening() const;
    // Whether the frame clock is running (tests).
    bool sending() const { return m_timer.isActive(); }
    int interval() const { return m_timer.interval(); }
    // Frames written, counted once per frame however many clients (tests).
    int framesSent() const { return m_framesSent; }

    // Where the levels come from; read on each tick.
    void setLevelsSource(std::function<QList<qreal>()> source) { m_source = std::move(source); }
    // Music is playing through Thrum, so the levels mean something.
    void setActive(bool on);
    void setStyle(const QString &style);
    void setReduceMotion(bool on);

    // Opens the socket when the setting is on. Call once the app is up.
    void start();

signals:
    void enabledChanged();
    void clientsChanged();

private:
    void listen();
    void close();
    void onNewConnection();
    void tick();
    void updateClock();
    void broadcast(const QByteArray &line);
    void sendState();
    QByteArray currentFrame();

    QString m_settingsPath, m_socketPath;
    bool m_enabled = true;
    bool m_started = false;
    bool m_active = false;
    bool m_reduceMotion = false;
    QString m_style = "peaks";
    QLocalServer *m_server = nullptr;
    QList<QLocalSocket *> m_clients;
    QTimer m_timer;
    std::function<QList<qreal>()> m_source;
    QByteArray m_last;
    int m_framesSent = 0;
};
