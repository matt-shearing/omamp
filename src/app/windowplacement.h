#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
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

private:
    void ask();
    void setManaged(bool managed);
    // Runs hyprctl with args in the background; done gets ok and stdout.
    void run(const QStringList &args, std::function<void(bool, const QByteArray &)> done);
    // One compact request at a time; a newer one waiting replaces an older.
    struct Request {
        bool enter = false;
        QString title;
        int width = 0, height = 0;
        bool fresh = false;
    };
    void submit(const Request &r);
    void startNext();
    void finish(bool enter, bool ok);
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
    bool m_hasPending = false;
    Request m_pending;
    Saved m_saved;
    QString m_address;
};
