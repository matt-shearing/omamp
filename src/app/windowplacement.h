#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>

// Whether the compositor decides the window's size. On Hyprland a tiled (or
// fullscreen) window can't resize itself: when the window asks for the
// library's remembered size, Qt lays the content out at that size until the
// compositor answers with the tile's real size, so the library first shows
// small in the corner and then jumps to fill the tile. The window asks here
// before it changes its own size, and leaves the size alone when managed.
//
// The answer comes from `hyprctl -j clients` (the client with this process's
// pid), asked in the background when the window gains focus or changes size,
// so switching modes never waits for it. Without Hyprland nothing is managed.
class WindowPlacement : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool managed READ managed NOTIFY managedChanged)

public:
    // hyprctl: the program to ask; empty finds hyprctl when Hyprland runs.
    explicit WindowPlacement(QObject *parent = nullptr, const QString &hyprctl = {});
    ~WindowPlacement() override;

    bool managed() const { return m_managed; }
    // Asks again soon (debounced). Called on focus and size changes.
    Q_INVOKABLE void refresh();

    // One client from `hyprctl -j clients`, for the tests: true when the
    // client with this pid is tiled or fullscreen. found says whether it was there.
    static bool managedIn(const QByteArray &clientsJson, qint64 pid, bool *found = nullptr);

signals:
    void managedChanged();

private:
    void ask();

    QString m_hyprctl;
    QProcess m_process;
    QTimer m_debounce, m_timeout;
    bool m_managed = false;
};
