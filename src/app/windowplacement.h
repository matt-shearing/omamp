#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <functional>

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
//
// The mini and micro players: a window rule only applies when a window
// opens, so switching to a compact player asks Hyprland directly to float,
// pin, size and place this window (enterCompact), and switching back puts
// it as it was (leaveCompact). Hyprland 0.56 reads dispatches as Lua:
//
//   hyprctl --batch "dispatch hl.dsp.window.float({ window = "address:0x…", action = "on" }); …"
//
// Each step runs in the background; compactPlaced / compactLeft say whether
// it worked, so the window can lay out the strip on its own when not.
//
// Resizing a floating window: Hyprland doesn't follow a floating window that
// changes its own size (measured on 0.56.2: Qt went to the library's
// 1360 x 820 while Hyprland kept the window at 1100 x 760, so the bottom of
// the content was cut off; going back to the deck's 500 x 680 left a gap).
// So the window asks Hyprland for the size too (resizeFloating), and takes
// whatever size Hyprland then reports (floatingResized).
class WindowPlacement : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool managed READ managed NOTIFY managedChanged)
    // hyprctl is there to ask: the compact players are placed through it.
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    // hyprctl: the program to ask; empty finds hyprctl when Hyprland runs.
    explicit WindowPlacement(QObject *parent = nullptr, const QString &hyprctl = {});
    ~WindowPlacement() override;

    bool managed() const { return m_managed; }
    bool available() const { return m_available; }
    // Asks again soon (debounced). Called on focus and size changes.
    Q_INVOKABLE void refresh();

    // Floats, pins, sizes and places the window top right under the bar,
    // as the window rule in docs/now-playing.md does. The state before the
    // first compact player is kept for leaveCompact. fresh: the window has
    // just opened as a compact player (`omamp mini`), so going back tiles it.
    Q_INVOKABLE void enterCompact(const QString &title, int width, int height, bool fresh = false);
    // Unpins and puts back the tiled or floating state, size and place.
    Q_INVOKABLE void leaveCompact();
    // Asks Hyprland to size a floating window, kept within its monitor and
    // moved back on to it when it would hang off. growOnly: never smaller
    // than it is now in either direction. Answers with floatingResized and
    // the number returned here; a request replaced by a newer one before it
    // ran never answers.
    Q_INVOKABLE int resizeFloating(int width, int height, bool growOnly = false);

    // One client from `hyprctl -j clients`, for the tests: true when the
    // client with this pid is tiled or fullscreen. found says whether it was there.
    static bool managedIn(const QByteArray &clientsJson, qint64 pid, bool *found = nullptr);

    // What the window was before the compact player.
    struct Saved {
        bool valid = false;
        bool floating = false;
        bool pinned = false;
        int x = 0, y = 0, width = 0, height = 0;
    };
    // The client with this pid; one titled `title` first. Empty when none.
    static QJsonObject clientIn(const QByteArray &clientsJson, qint64 pid, const QString &title = {});
    // The Lua dispatches that float, size, place and pin `client` on its
    // monitor: (monitor_w - w - 24, monitor_h * 0.05), as the window rule.
    // Empty when the client's monitor isn't listed.
    static QStringList enterCommands(const QJsonObject &client, const QJsonArray &monitors, int width, int height);
    // The Lua dispatches that put the client at `address` back as `saved`.
    static QStringList leaveCommands(const QString &address, const Saved &saved);
    // The Lua dispatches that size the floating `client` to width x height
    // (or at least its present size, growOnly), no larger than its monitor's
    // free area, and centre it there when it would hang off. *size gets the
    // size asked for. Empty when nothing needs to change or the monitor
    // isn't listed.
    static QStringList resizeCommands(const QJsonObject &client, const QJsonArray &monitors, int width, int height,
                                      bool growOnly = false, QSize *size = nullptr);
    // `hyprctl --batch` argument for these dispatches.
    static QString batch(const QStringList &luas);
    // hyprctl's reply to a batch: every line "ok" (Lua errors say "error: …").
    static bool batchOk(const QByteArray &reply);

signals:
    void managedChanged();
    // enterCompact finished: ok when Hyprland floated and sized the window.
    void compactPlaced(bool ok);
    // leaveCompact finished: ok when the window was put back.
    void compactLeft(bool ok);
    // resizeFloating finished: request is the number it returned. floating: the window floats (false when
    // Hyprland tiles it or it is fullscreen); width x height: its size as
    // Hyprland now reports it, 0 x 0 when unknown.
    void floatingResized(int request, bool floating, int width, int height);

private:
    void ask();
    void setManaged(bool managed);
    // Runs hyprctl with args in the background; done gets ok and stdout.
    void run(const QStringList &args, std::function<void(bool, const QByteArray &)> done);
    // One request at a time, in order.
    enum class Kind { Enter, Leave, Resize };
    struct Request {
        Kind kind = Kind::Enter;
        QString title;
        int width = 0, height = 0;
        bool fresh = false;   // Enter
        bool growOnly = false; // Resize
        int id = 0;            // Resize
    };
    void submit(const Request &r);
    void startNext();
    void finish(Kind kind, bool ok);
    void doResize(const Request &r);
    void resizeDone(int id, const QString &address);
    void enterFind(const Request &r, int tries);
    void enterPlace(const Request &r, const QJsonObject &client);
    void enterCheck(const Request &r, const QString &address, int tries);
    void doLeave();

    QString m_hyprctl;
    bool m_available = false;
    QProcess m_process;
    QTimer m_debounce, m_timeout;
    bool m_managed = false;

    bool m_busy = false;
    // In order; a resize waiting is replaced by a newer one.
    QList<Request> m_queue;
    int m_lastResize = 0;
    Saved m_saved;
    QString m_address;
};
