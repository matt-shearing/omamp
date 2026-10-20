// The menu bar and the visible ways between the views: every menu entry
// runs its action (or is greyed when it can't) on the deck and in the
// library; View's Player / Library / Now Playing; the bar hidden in the
// mini and micro players and the big view; party mode's locks; the Spotify
// menu switched off and on; the one-time library hint; the keys from any
// focus; a tiled window never resizing itself; the compact players on
// Hyprland through a fake hyprctl; and how long Tab takes.
//
// Offscreen in software, never on an audio device, settings in temporary
// folders. Entries that would reach the internet, open a file picker or a
// browser, or quit are only checked for being listed and enabled.

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "artprovider.h"
#include "fixture_library.h"
#include "library.h"
#include "librarycontroller.h"
#include "nativeengine.h"
#include "nowplaying_context.h"
#include "pcmsink.h"
#include "player.h"
#include "playrecorder.h"
#include "djcontroller.h"
#include "spotifycontroller.h"
#include "theme.h"
#include "translator.h"
#include "windowplacement.h"

namespace {

QStringList *qmlWarnings = nullptr;
QtMessageHandler previousHandler = nullptr;
void collect(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    // The made-up tracks have no pictures; the grid says so for each.
    if (qmlWarnings && type == QtWarningMsg && message.contains("qrc:/") && !message.contains("No picture"))
        qmlWarnings->append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

QObject *findItem(QQuickItem *item, const QString &name) {
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QObject *o = findItem(child, name))
            return o;
    return nullptr;
}

// Never run from a test: they reach the internet, open a picker or a
// browser on the desktop, or quit.
const QStringList unsafe = {"quit", "open-website", "open-mediamonkey-guide", "library-add-folder", "open-folder",
                            "import-playlists", "export-playlist", "radio-import", "radio-export", "export-csv",
                            "export-html", "find-missing-covers", "find-cover", "identify", "radio", "radio-find",
                            "radio-next", "radio-previous", "sync-servers", "rescan"};

struct Entry {
    QObject *item;
    QString command, text;
};

// The menu's entries, its submenus' too.
void entriesOf(QObject *menu, QList<Entry> *out) {
    const int count = menu->property("count").toInt();
    for (int i = 0; i < count; ++i) {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, i));
        if (!item)
            continue;
        if (QObject *sub = item->property("subMenu").value<QObject *>()) {
            entriesOf(sub, out);
            continue;
        }
        const QVariant command = item->property("command");
        if (command.isValid())
            out->append({item, command.toString(), item->property("text").toString()});
    }
}

// The window with its context, as main() builds it, in a temporary folder.
struct Rig {
    QTemporaryDir &dir;
    Translator translator;
    Theme theme;
    std::unique_ptr<Player> player;
    std::unique_ptr<LibraryController> library;
    SpotifyController *spotify = nullptr;
    WindowPlacement *placement = nullptr;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    explicit Rig(QTemporaryDir &d, const QString &hyprctl = "/nonexistent/hyprctl")
        : dir(d), translator(d.filePath("language")), theme(d.filePath("theme"), nullptr, d.filePath("no-hyprctl")) {
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        player = std::make_unique<Player>(new NativeEngine(sink), dir.filePath("state.json"));
        library = std::make_unique<LibraryController>(dir.filePath("library.db"));
        library->setPlayer(player.get());
        library->setWatching(false);
        library->setArtCacheDir(dir.filePath("art"));
        library->recorder()->setFlushing(false);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", player.get());
        engine.rootContext()->setContextProperty("library", library.get());
        engine.rootContext()->setContextProperty("commandPalette", library->palette());
        engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(dir.filePath("window.conf")));
        installNowPlaying(engine, &theme, player.get(), dir.filePath("library.db"), dir.filePath("art"),
                          dir.filePath("window.conf"));
        spotify = engine.rootContext()->contextProperty("spotify").value<SpotifyController *>();
        // The tree's Spotify node follows the switch, as main() wires it.
        QObject::connect(spotify, &SpotifyController::settingsChanged, library.get(), [this] {
            library->tree()->setExtraNode("spotify", "Spotify", spotify->enabled());
        });
        placement = new WindowPlacement(&engine, hyprctl);
        engine.rootContext()->setContextProperty("windowPlacement", placement);
        engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider(dir.filePath("library.db"), dir.filePath("art")));
    }
    bool load() {
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().size() != 1)
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        return window && QTest::qWaitForWindowExposed(window);
    }
    QObject *find(const QString &name) const {
        if (QObject *o = window->findChild<QObject *>(name))
            return o;
        QQuickItem *top = window->contentItem()->parentItem() ? window->contentItem()->parentItem() : window->contentItem();
        return findItem(top, name);
    }
    QObject *bar() const { return window->property("menuBar").value<QObject *>(); }
    QVariant call(const char *method, const QVariant &arg = {}) {
        QVariant result;
        if (arg.isValid())
            QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, arg));
        else
            QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result));
        return result;
    }
    void run(const QString &id) { call("runAction", id); }
    bool advanced() const { return window->property("advanced").toBool(); }
    QObject *modes() const { return bar()->property("modes").value<QObject *>(); }
    QList<Entry> entries() {
        QList<Entry> out;
        for (const char *name : {"fileMenu", "editMenu", "viewMenu", "playMenu", "libraryMenu", "radioMenu",
                                 "spotifyMenu", "helpMenu"})
            if (QObject *menu = find(name))
                entriesOf(menu, &out);
        return out;
    }
    Entry entry(const QString &objectName) {
        QObject *o = find(objectName);
        return {o, o ? o->property("command").toString() : QString(), o ? o->property("text").toString() : QString()};
    }
    // Every sheet, dialog, menu and the palette closed again.
    void closePopups() {
        for (QObject *o : window->findChildren<QObject *>())
            if (o->metaObject()->inherits(&QQuickItem::staticMetaObject) == false && o->property("opened").toBool()
                && o->metaObject()->indexOfMethod("close()") >= 0)
                QMetaObject::invokeMethod(o, "close");
        window->setProperty("helpVisible", false);
    }
    // Runs an entry as a click would, and waits for the action.
    bool trigger(QObject *item) {
        window->setProperty("lastAction", QString());
        QMetaObject::invokeMethod(item, "triggered");
        const QString want = item->property("command").toString();
        QElapsedTimer t;
        t.start();
        while (window->property("lastAction").toString() != want && t.elapsed() < 2000)
            QTest::qWait(10);
        return window->property("lastAction").toString() == want;
    }
};

// Fills the library with the made-up fixture tracks.
bool fillLibrary(const QString &path) {
    Library library(path);
    if (!library.open() || !Fixture::fill(library))
        return false;
    QSqlQuery q(library.db());
    q.exec("UPDATE volumes SET online = 1");
    q.exec("UPDATE tracks SET availability = 'online'");
    return true;
}

void writeConf(const QString &path, bool advanced, bool hintDone = true) {
    QSettings conf(path, QSettings::IniFormat);
    conf.setValue("window/advanced", advanced);
    conf.setValue("window/libraryHintDone", hintDone);
}

// A hyprctl that keeps one client (this process, address 0x5afe, on
// monitor 1) in files beside it, applies the Lua dispatches it is sent to
// that state, logs every batch to `log`, and fails each batch while a file
// called `fail` is there. floating: how the window starts.
QString fakeHyprctl(const QString &dir, bool floating) {
    auto put = [&](const QString &name, const QByteArray &text) {
        QFile f(dir + "/" + name);
        f.open(QIODevice::WriteOnly);
        f.write(text);
    };
    put("floating", floating ? "true\n" : "false\n");
    put("pinned", "false\n");
    put("at", floating ? "100,200\n" : "12,42\n");
    put("size", floating ? "1100,760\n" : "1700,1000\n");
    // Monitor 1 sits right of monitor 0 and scales by 1.5: 2560 x 1440.
    put("monitors.json", R"([{"id":0,"name":"DP-1","x":0,"y":0,"width":6144,"height":2560,"scale":1,"transform":0},
                            {"id":1,"name":"HDMI-A-1","x":6144,"y":0,"width":3840,"height":2160,"scale":1.5,"transform":0}])");
    const QString script = dir + "/hyprctl";
    put("hyprctl", QString(R"SH(#!/bin/sh
d=$(dirname "$0")
st() { tr -d '\n' < "$d/$1"; }
xy() { echo "$1" | sed 's/.*x = \([0-9-]*\), y = \([0-9-]*\).*/\1,\2/'; }
case "$1" in
-j)
  if [ "$2" = monitors ]; then cat "$d/monitors.json"; exit 0; fi
  printf '[{"address":"0x5afe","pid":%1,"title":"Omamp","floating":%s,"pinned":%s,"fullscreen":0,"monitor":1,"at":[%s],"size":[%s]}]\n' \
    "$(st floating)" "$(st pinned)" "$(st at)" "$(st size)" ;;
--batch)
  printf '%s\n' "$2" >> "$d/log"
  if [ -e "$d/fail" ]; then echo ok; echo 'error: [string "x"]:1: attempt to call a nil value'; exit 0; fi
  echo "$2" | tr ';' '\n' | while IFS= read -r c; do
    case "$c" in
      *'float({'*'"on"'*) echo true > "$d/floating" ;;
      *'float({'*'"off"'*) echo false > "$d/floating"; echo 1700,1000 > "$d/size" ;;
      *'pin({'*'"on"'*) echo true > "$d/pinned" ;;
      *'pin({'*'"off"'*) echo false > "$d/pinned" ;;
      *'resize({'*) xy "$c" > "$d/size" ;;
      *'move({'*) xy "$c" > "$d/at" ;;
    esac
    echo ok
  done ;;
esac
)SH").arg(QCoreApplication::applicationPid()).toUtf8());
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return script;
}

QStringList batches(const QString &dir) {
    QFile f(dir + "/log");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

} // namespace

class MenuTests : public QObject {
    Q_OBJECT

    QStringList m_warnings;

private slots:
    void init() {
        m_warnings.clear();
        qmlWarnings = &m_warnings;
        previousHandler = qInstallMessageHandler(collect);
    }
    void cleanup() {
        qInstallMessageHandler(previousHandler);
        qmlWarnings = nullptr;
    }

    void placementReadsHyprctl() {
        const qint64 pid = 4242;
        bool found = false;
        const QByteArray tiled = R"([{"pid":17,"floating":true,"fullscreen":0},{"pid":4242,"floating":false,"fullscreen":0}])";
        QVERIFY(WindowPlacement::managedIn(tiled, pid, &found));
        QVERIFY(found);
        QVERIFY(!WindowPlacement::managedIn(R"([{"pid":4242,"floating":true,"fullscreen":0}])", pid, &found));
        QVERIFY(found);
        // Fullscreen, even floating, is the compositor's size; old Hyprland says true.
        QVERIFY(WindowPlacement::managedIn(R"([{"pid":4242,"floating":true,"fullscreen":2}])", pid));
        QVERIFY(WindowPlacement::managedIn(R"([{"pid":4242,"floating":true,"fullscreen":true}])", pid));
        QVERIFY(!WindowPlacement::managedIn(R"([{"pid":1,"floating":false}])", pid, &found));
        QVERIFY(!found);
        QVERIFY(!WindowPlacement::managedIn("not json", pid, &found));
    }

    // The Lua dispatches for Hyprland 0.56: placed as the window rule
    // places a compact player, on the window's own monitor, and put back.
    void placementCommands() {
        const qint64 pid = 4242;
        const QByteArray clients = R"([{"address":"0x1","pid":17,"title":"x"},
            {"address":"0xa","pid":4242,"title":"Omamp","monitor":1,"floating":false},
            {"address":"0xb","pid":4242,"title":"Omamp mini","monitor":1,"floating":false}])";
        QCOMPARE(WindowPlacement::clientIn(clients, pid).value("address").toString(), QString("0xa"));
        QCOMPARE(WindowPlacement::clientIn(clients, pid, "Omamp mini").value("address").toString(), QString("0xb"));
        QVERIFY(WindowPlacement::clientIn(clients, 99).isEmpty());

        const QJsonArray monitors = QJsonDocument::fromJson(R"([{"id":0,"x":0,"y":0,"width":6144,"height":2560,"scale":1},
            {"id":1,"x":6144,"y":0,"width":3840,"height":2160,"scale":1.5,"transform":0},
            {"id":2,"x":0,"y":2560,"width":1080,"height":1920,"scale":1,"transform":1}])").array();
        QJsonObject client{{"address", "0xb"}, {"monitor", 0}};
        QCOMPARE(WindowPlacement::enterCommands(client, monitors, 420, 64), QStringList({
            R"(hl.dsp.window.float({ window = "address:0xb", action = "on" }))",
            R"(hl.dsp.window.resize({ window = "address:0xb", x = 420, y = 64, exact = true }))",
            R"(hl.dsp.window.move({ window = "address:0xb", x = 5700, y = 128, exact = true }))",
            R"(hl.dsp.window.pin({ window = "address:0xb", action = "on" }))"}));
        // Scaled: 2560 x 1440 in layout pixels, right of the first monitor.
        client["monitor"] = 1;
        QVERIFY(WindowPlacement::enterCommands(client, monitors, 275, 28).at(2).contains("x = 8405, y = 72,"));
        // Turned: 1920 wide.
        client["monitor"] = 2;
        QVERIFY(WindowPlacement::enterCommands(client, monitors, 420, 64).at(2).contains("x = 1476, y = 2614,"));
        client["monitor"] = 7;
        QVERIFY(WindowPlacement::enterCommands(client, monitors, 420, 64).isEmpty());

        WindowPlacement::Saved tiled;
        tiled.valid = true;
        QCOMPARE(WindowPlacement::leaveCommands("0xb", tiled), QStringList({
            R"(hl.dsp.window.pin({ window = "address:0xb", action = "off" }))",
            R"(hl.dsp.window.float({ window = "address:0xb", action = "off" }))"}));
        WindowPlacement::Saved floating{true, true, true, 100, 200, 1100, 760};
        QCOMPARE(WindowPlacement::leaveCommands("0xb", floating), QStringList({
            R"(hl.dsp.window.resize({ window = "address:0xb", x = 1100, y = 760, exact = true }))",
            R"(hl.dsp.window.move({ window = "address:0xb", x = 100, y = 200, exact = true }))"}));

        QCOMPARE(WindowPlacement::batch({"a()", "b()"}), QString("dispatch a(); dispatch b()"));
        QVERIFY(WindowPlacement::batchOk("ok\n\n\nok\n"));
        QVERIFY(!WindowPlacement::batchOk("ok\n\nerror: [string]:1: attempt to call a nil value\n"));
        QVERIFY(!WindowPlacement::batchOk(""));
    }

    // Ctrl+M on a tiled window on Hyprland: the title first, then Hyprland
    // floats, sizes, places and pins it; the content stays hidden until the
    // size arrives. Mini to micro keeps what the window was; back tiles it.
    void compactPlayersOnHyprlandTiled() {
        QTemporaryDir dir;
        writeConf(dir.filePath("window.conf"), false);
        const QString hyprctl = fakeHyprctl(dir.path(), false);
        Rig rig(dir, hyprctl);
        QVERIFY(rig.load());
        QVERIFY(rig.placement->available());
        rig.placement->refresh();
        QTRY_VERIFY(rig.placement->managed());
        rig.window->resize(1700, 1000);
        QTRY_COMPARE(rig.window->width(), 1700);
        QObject *modes = rig.modes();
        QQuickItem *root = qobject_cast<QQuickItem *>(rig.find("deckView"))->parentItem();

        rig.run("mini-player");
        QCOMPARE(rig.window->title(), QString("Omamp mini"));
        QVERIFY(modes->property("placing").toBool());
        QCOMPARE(root->property("opacity").toDouble(), 0.0);
        QCOMPARE(rig.window->width(), 1700); // never asks for a size itself
        QTRY_COMPARE(batches(dir.path()).size(), 1);
        QCOMPARE(batches(dir.path()).at(0), WindowPlacement::batch({
            R"(hl.dsp.window.float({ window = "address:0x5afe", action = "on" }))",
            R"(hl.dsp.window.resize({ window = "address:0x5afe", x = 420, y = 64, exact = true }))",
            R"(hl.dsp.window.move({ window = "address:0x5afe", x = 8260, y = 72, exact = true }))",
            R"(hl.dsp.window.pin({ window = "address:0x5afe", action = "on" }))"}));
        // Still hidden until Hyprland's size arrives.
        QTest::qWait(100);
        QVERIFY(modes->property("placing").toBool());
        rig.window->resize(420, 64); // as Hyprland would
        QTRY_VERIFY(!modes->property("placing").toBool());
        QCOMPARE(root->property("opacity").toDouble(), 1.0);
        QVERIFY(!rig.placement->managed());
        QObject *mini = rig.find("miniPlayer");
        QVERIFY(mini && !mini->property("large").toBool());
        QVERIFY(!rig.find("compactHint")->property("visible").toBool());

        rig.run("micro-player");
        QCOMPARE(rig.window->title(), QString("Omamp micro"));
        QTRY_COMPARE(batches(dir.path()).size(), 2);
        QVERIFY(batches(dir.path()).at(1).contains("x = 275, y = 28, exact"));
        QVERIFY(batches(dir.path()).at(1).contains("x = 8405, y = 72, exact"));
        rig.window->resize(275, 28);
        QTRY_VERIFY(!modes->property("placing").toBool());

        // Back: unpinned and tiled again, as it was before the mini player.
        rig.run("micro-player");
        QVERIFY(rig.window->title() != "Omamp micro");
        QVERIFY(modes->property("placing").toBool());
        QTRY_COMPARE(batches(dir.path()).size(), 3);
        QCOMPARE(batches(dir.path()).at(2), WindowPlacement::batch({
            R"(hl.dsp.window.pin({ window = "address:0x5afe", action = "off" }))",
            R"(hl.dsp.window.float({ window = "address:0x5afe", action = "off" }))"}));
        rig.window->resize(1700, 1000);
        QTRY_VERIFY(!modes->property("placing").toBool());
        QTRY_VERIFY(rig.placement->managed());
        QVERIFY(rig.bar()->property("visible").toBool());
        QCOMPARE(rig.window->width(), 1700);
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // A floating window goes back to its own size and place, unpinned.
    void compactPlayersOnHyprlandFloating() {
        QTemporaryDir dir;
        writeConf(dir.filePath("window.conf"), false);
        const QString hyprctl = fakeHyprctl(dir.path(), true);
        Rig rig(dir, hyprctl);
        QVERIFY(rig.load());
        QObject *modes = rig.modes();
        rig.run("mini-player");
        QTRY_COMPARE(batches(dir.path()).size(), 1);
        rig.window->resize(420, 64);
        QTRY_VERIFY(!modes->property("placing").toBool());
        rig.run("mini-player");
        QTRY_COMPARE(batches(dir.path()).size(), 2);
        QCOMPARE(batches(dir.path()).at(1), WindowPlacement::batch({
            R"(hl.dsp.window.pin({ window = "address:0x5afe", action = "off" }))",
            R"(hl.dsp.window.resize({ window = "address:0x5afe", x = 1100, y = 760, exact = true }))",
            R"(hl.dsp.window.move({ window = "address:0x5afe", x = 100, y = 200, exact = true }))"}));
        rig.window->resize(1100, 760);
        QTRY_VERIFY(!modes->property("placing").toBool());
        QVERIFY(!rig.placement->managed());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // `omamp mini`: the title and size are there from the first frame, for
    // the window rule; Hyprland is asked as well, for a desktop without the
    // rule; and going back tiles the window, Hyprland's default.
    void compactPlayerAtStartUp() {
        QTemporaryDir dir;
        writeConf(dir.filePath("window.conf"), false);
        const QString hyprctl = fakeHyprctl(dir.path(), true); // the rule floated it
        Rig rig(dir, hyprctl);
        rig.engine.rootContext()->setContextProperty("startMode", QString("mini"));
        QObject *created = nullptr;
        QString firstTitle;
        QSize firstSize;
        QObject::connect(&rig.engine, &QQmlApplicationEngine::objectCreated, [&](QObject *o) {
            created = o;
            firstTitle = o ? o->property("title").toString() : QString();
            firstSize = o ? QSize(o->property("width").toInt(), o->property("height").toInt()) : QSize();
        });
        QVERIFY(rig.load());
        QVERIFY(created);
        QCOMPARE(firstTitle, QString("Omamp mini"));
        QCOMPARE(firstSize, QSize(420, 64));
        QObject *modes = rig.modes();
        QCOMPARE(modes->property("compact").toString(), QString("mini"));
        QTRY_COMPARE(batches(dir.path()).size(), 1);
        QVERIFY(batches(dir.path()).at(0).contains("x = 420, y = 64, exact"));
        QTRY_VERIFY(!modes->property("placing").toBool());
        rig.run("mini-player");
        QTRY_COMPARE(batches(dir.path()).size(), 2);
        QCOMPARE(batches(dir.path()).at(1), WindowPlacement::batch({
            R"(hl.dsp.window.pin({ window = "address:0x5afe", action = "off" }))",
            R"(hl.dsp.window.float({ window = "address:0x5afe", action = "off" }))"}));
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // Hyprland says no (or there is no hyprctl and the window manager keeps
    // the window large): the strip lies along the top, full width, with a
    // line saying why; never a small strip in the middle of empty space.
    void compactPlayerFallsBackWhenKeptLarge() {
        QTemporaryDir dir;
        writeConf(dir.filePath("window.conf"), false);
        const QString hyprctl = fakeHyprctl(dir.path(), false);
        QFile(dir.filePath("fail")).open(QIODevice::WriteOnly);
        Rig rig(dir, hyprctl);
        QVERIFY(rig.load());
        rig.placement->refresh();
        QTRY_VERIFY(rig.placement->managed());
        rig.window->resize(1700, 1000);
        QTRY_COMPARE(rig.window->width(), 1700);
        QObject *modes = rig.modes();
        QSignalSpy placed(rig.placement, &WindowPlacement::compactPlaced);
        rig.run("mini-player");
        QTRY_COMPARE(placed.size(), 1);
        QCOMPARE(placed.at(0).at(0).toBool(), false);
        QTRY_VERIFY(!modes->property("placing").toBool());
        QCOMPARE(rig.window->width(), 1700);
        QCOMPARE(rig.window->title(), QString("Omamp mini"));
        auto *mini = qobject_cast<QQuickItem *>(rig.find("miniPlayer"));
        QVERIFY(mini && mini->property("large").toBool());
        QCOMPARE(mini->width(), 1700.0);
        auto *strip = qobject_cast<QQuickItem *>(rig.find("miniLayout"));
        QCOMPARE(strip->y(), 6.0);
        QCOMPARE(strip->height(), 52.0);
        QCOMPARE(strip->width(), 1700.0 - 12);
        auto *hint = qobject_cast<QQuickItem *>(rig.find("compactHint"));
        QVERIFY(hint->isVisible());
        QVERIFY(hint->property("text").toString().contains("keeps this window large"));
        QVERIFY(hint->y() >= 64 && hint->y() < 120);
        // The micro bar too.
        rig.run("micro-player");
        QTRY_COMPARE(placed.size(), 2);
        QTRY_VERIFY(!modes->property("placing").toBool());
        auto *micro = qobject_cast<QQuickItem *>(rig.find("microLayout"));
        QCOMPARE(micro->y(), 3.0);
        QCOMPARE(micro->height(), 22.0);
        QVERIFY(hint->isVisible());
        // Back to the deck: nothing had changed, nothing is undone.
        rig.run("micro-player");
        QTRY_VERIFY(!modes->property("placing").toBool());
        QCOMPARE(rig.window->width(), 1700);
        QVERIFY(rig.find("miniPlayer") == nullptr);

        // Without hyprctl at all, the window asks for the size itself.
        QTemporaryDir dir2;
        writeConf(dir2.filePath("window.conf"), false);
        Rig plain(dir2);
        QVERIFY(plain.load());
        QVERIFY(!plain.placement->available());
        plain.run("mini-player");
        QTRY_COMPARE(plain.window->size(), QSize(420, 64));
        QTRY_VERIFY(!plain.modes()->property("placing").toBool());
        QTRY_VERIFY(!plain.find("compactHint")->property("visible").toBool());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // Every entry, on the deck and in the library: greyed exactly when the
    // window says it can't run, and when it can, clicking it runs it.
    void everyEntryRunsItsAction() {
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        writeConf(dir.filePath("window.conf"), false);
        Rig rig(dir);
        QVERIFY(rig.load());
        QObject *bar = rig.bar();
        QVERIFY(bar && bar->property("visible").toBool());

        const QStringList palette = rig.library->palette()->actionIds();
        for (const bool library : {false, true}) {
            rig.run(library ? "view-library" : "view-player");
            QCOMPARE(rig.advanced(), library);
            QTRY_VERIFY(bar->property("visible").toBool());
            if (library) {
                QTRY_VERIFY(rig.library->ready());
                rig.library->tracks()->select(0);
            }
            QList<Entry> all = rig.entries();
            QVERIFY2(all.size() > 90, qPrintable(QString::number(all.size())));
            int ran = 0, greyed = 0;
            for (const Entry &e : all) {
                if (e.command.isEmpty())
                    continue; // a line of its own: a visualiser
                // Menu, keys and palette share their ids.
                QVERIFY2(palette.contains(e.command) || e.command.startsWith("spotify-section:") || e.command == "palette",
                         qPrintable("not in the palette: " + e.command));
                rig.call("updateActions");
                const bool can = rig.call("actionEnabled", e.command).toBool();
                const bool visible = e.item->property("shown").toBool();
                if (visible)
                    QVERIFY2(e.item->property("enabled").toBool() == can, qPrintable(e.command));
                if (!can || !visible) {
                    ++greyed;
                    continue;
                }
                if (unsafe.contains(e.command) || e.command.startsWith("radio:"))
                    continue;
                QVERIFY2(rig.trigger(e.item), qPrintable("didn't run: " + e.command));
                ++ran;
                // Back to where the pass started.
                rig.closePopups();
                if (rig.window->visibility() == QWindow::FullScreen)
                    rig.window->showNormal();
                if (e.command == "toggle-menu-bar")
                    rig.run("toggle-menu-bar");
                rig.run(library ? "view-library" : "view-player");
                QTRY_VERIFY(bar->property("visible").toBool());
            }
            qInfo("%s: %d entries ran, %d greyed or hidden", library ? "library" : "deck", ran, greyed);
            QVERIFY(ran > 50);
        }
        // Greyed when nothing can be undone; the entry names what it would undo.
        rig.call("updateActions");
        QVERIFY(!rig.call("actionEnabled", "undo").toBool() || !rig.library->undoText().isEmpty());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // View: Player, Library and Now Playing as one choice; the bar is gone
    // in the big view and the mini and micro players.
    void viewChoiceAndWhereTheBarShows() {
        QTemporaryDir dir;
        writeConf(dir.filePath("window.conf"), false);
        Rig rig(dir);
        QVERIFY(rig.load());
        QObject *bar = rig.bar();
        Entry player = rig.entry("menuViewPlayer"), lib = rig.entry("menuViewLibrary"), now = rig.entry("menuViewNowPlaying");
        QVERIFY(player.item && lib.item && now.item);
        QVERIFY(player.item->property("marked").toBool());
        QVERIFY(!lib.item->property("marked").toBool());

        QVERIFY(rig.trigger(lib.item));
        QVERIFY(rig.advanced());
        QVERIFY(lib.item->property("marked").toBool());
        QVERIFY(!player.item->property("marked").toBool());
        QVERIFY(bar->property("visible").toBool());

        QVERIFY(rig.trigger(now.item));
        QTRY_VERIFY(rig.modes()->property("bigView").toBool());
        QVERIFY(!bar->property("visible").toBool());
        rig.run("view-library");
        QVERIFY(!rig.modes()->property("bigView").toBool());
        QVERIFY(bar->property("visible").toBool());

        QVERIFY(rig.trigger(player.item));
        QVERIFY(!rig.advanced());
        QVERIFY(player.item->property("marked").toBool());

        rig.run("mini-player");
        QCOMPARE(rig.modes()->property("compact").toString(), QString("mini"));
        QVERIFY(!bar->property("visible").toBool());
        rig.run("micro-player");
        QCOMPARE(rig.modes()->property("compact").toString(), QString("micro"));
        QVERIFY(!bar->property("visible").toBool());
        rig.run("micro-player");
        QVERIFY(bar->property("visible").toBool());

        // Switched off: gone, but F10 shows it and opens File; Alt alone
        // shows it too. Switched on again from the palette.
        rig.run("toggle-menu-bar");
        QVERIFY(!bar->property("visible").toBool());
        QTest::keyClick(rig.window, Qt::Key_F10);
        QTRY_VERIFY(bar->property("visible").toBool());
        QObject *file = rig.find("fileMenu");
        QTRY_VERIFY(file->property("opened").toBool());
        QTest::keyClick(rig.window, Qt::Key_Escape);
        QTRY_VERIFY(!file->property("opened").toBool());
        QTRY_VERIFY(!bar->property("visible").toBool());
        QTest::keyClick(rig.window, Qt::Key_Alt);
        QTRY_VERIFY(bar->property("visible").toBool());
        QTest::keyClick(rig.window, Qt::Key_Alt);
        QTRY_VERIFY(!bar->property("visible").toBool());
        rig.run("toggle-menu-bar");
        QVERIFY(bar->property("visible").toBool());
        // With the bar on, F10 opens File.
        QTest::keyClick(rig.window, Qt::Key_F10);
        QTRY_VERIFY(file->property("opened").toBool());
        QTest::keyClick(rig.window, Qt::Key_Escape);
        QTRY_VERIFY(!file->property("opened").toBool());

        // The deck's Library and Now Playing buttons, and the strip's Player.
        QObject *libraryButton = rig.find("libraryButton");
        QVERIFY(libraryButton);
        QCOMPARE(libraryButton->property("label").toString(), QString("Library"));
        QMetaObject::invokeMethod(libraryButton, "clicked");
        QVERIFY(rig.advanced());
        QObject *playerButton = nullptr;
        QTRY_VERIFY((playerButton = rig.find("playerButton")) != nullptr);
        QMetaObject::invokeMethod(playerButton, "clicked");
        QVERIFY(!rig.advanced());
        QMetaObject::invokeMethod(rig.find("bigViewButton"), "clicked");
        QVERIFY(rig.modes()->property("bigView").toBool());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // Party mode greys what the guests may not do, in the menu as on the keys.
    void partyModeGreysLockedEntries() {
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        writeConf(dir.filePath("window.conf"), true);
        Rig rig(dir);
        QVERIFY(rig.load());
        QTRY_VERIFY(rig.library->ready());
        rig.library->tracks()->select(0);
        rig.call("updateActions");
        for (const char *id : {"settings", "edit-tags", "next", "volume-up", "organise", "rescan", "deck-style:classic"})
            QVERIFY2(rig.call("actionEnabled", QString(id)).toBool(), id);
        // Jukebox: the library, adding and playing; nothing else.
        QVERIFY(rig.library->dj()->enableParty("jukebox", {}, false));
        rig.call("updateActions");
        for (const char *id : {"settings", "edit-tags", "next", "volume-up", "organise", "rescan", "deck-style:classic",
                               "settings-spotify", "toggle-menu-bar"})
            QVERIFY2(!rig.call("actionEnabled", QString(id)).toBool(), id);
        for (const char *id : {"play-pause", "view-library", "search", "party", "shortcuts"})
            QVERIFY2(rig.call("actionEnabled", QString(id)).toBool(), id);
        Entry settings = rig.entry("menuShowMenuBar");
        QVERIFY(!settings.item->property("enabled").toBool());
        QVERIFY(rig.entry("menuParty").item->property("enabled").toBool());
        QVERIFY(rig.entry("menuParty").item->property("marked").toBool());
        // The action itself refuses as well.
        rig.run("volume-up");
        QCOMPARE(rig.window->property("lastAction").toString(), QString());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // Spotify off: one entry that leads to switching it on. On: search
    // from the deck in one click, with Spotify's own box taking the keys,
    // and the search box's Library / Spotify / Both choice.
    void spotifyMenu() {
        QTemporaryDir dir;
        // A fake spotify_player first on PATH, signed in, in temporary folders.
        QTemporaryDir fake;
        const QByteArray oldPath = qgetenv("PATH");
        QDir().mkpath(fake.filePath("cache"));
        QDir().mkpath(fake.filePath("config"));
        QDir().mkpath(fake.filePath("state"));
        QFile(fake.filePath("cache/credentials.json")).open(QIODevice::WriteOnly);
        QFile(fake.filePath("cache/0123456789abcdef_token.json")).open(QIODevice::WriteOnly);
        qputenv("PATH", QByteArray(OMAMP_TESTS_DIR) + "/fake-spotify:" + oldPath);
        qputenv("FAKE_SPOTIFY_DIR", fake.filePath("state").toUtf8());
        qputenv("OMAMP_SPOTIFY_PLAYER_CACHE", fake.filePath("cache").toUtf8());
        qputenv("OMAMP_SPOTIFY_PLAYER_CONFIG", fake.filePath("config").toUtf8());
        qputenv("OMAMP_SPOTIFY_OEMBED", "off");
        auto restore = qScopeGuard([&] {
            qputenv("PATH", oldPath);
            for (const char *v : {"FAKE_SPOTIFY_DIR", "OMAMP_SPOTIFY_PLAYER_CACHE", "OMAMP_SPOTIFY_PLAYER_CONFIG", "OMAMP_SPOTIFY_OEMBED"})
                qunsetenv(v);
        });
        writeConf(dir.filePath("window.conf"), false);
        Rig rig(dir);
        QVERIFY(rig.load());
        QObject *settings = rig.find("settingsSheet");
        Entry on = rig.entry("menuSpotifyOn"), search = rig.entry("menuSpotifySearch");
        QVERIFY(on.item && search.item);
        QVERIFY(on.item->property("shown").toBool());
        QVERIFY(!search.item->property("shown").toBool());
        QVERIFY(!rig.entry("menuSpotifyPlaylists").item->property("shown").toBool());
        QVERIFY(rig.trigger(on.item));
        QTRY_VERIFY(settings && settings->property("opened").toBool());
        rig.closePopups();
        QTRY_VERIFY(!settings->property("opened").toBool());

        rig.spotify->setStartDaemon(false);
        rig.spotify->setEnabled(true);
        QVERIFY(!on.item->property("shown").toBool());
        QVERIFY(search.item->property("shown").toBool());
        QVERIFY(rig.entry("menuSpotifyPlaylists").item->property("shown").toBool());
        QVERIFY(rig.entry("menuSpotifySettings").item->property("shown").toBool());
        // Like is greyed while Spotify plays nothing.
        rig.call("updateActions");
        QVERIFY(!rig.entry("menuSpotifyLike").item->property("enabled").toBool());

        // One click from the deck: the library, on Spotify, its box focused.
        QVERIFY(!rig.advanced());
        QVERIFY(search.item->property("enabled").toBool());
        QVERIFY(rig.trigger(search.item));
        QVERIFY(rig.advanced());
        QTRY_COMPARE(rig.library->currentKind(), QString("spotify"));
        QObject *box = nullptr;
        QTRY_VERIFY((box = rig.find("spotifySearch")) != nullptr);
        QTRY_VERIFY(box->property("activeFocus").toBool());

        // The scope choice beside the library's search box.
        QObject *scope = rig.find("searchScope");
        QVERIFY(scope);
        QTRY_VERIFY(scope->property("visible").toBool());
        QObject *both = rig.find("searchScope_both");
        QMetaObject::invokeMethod(both, "clicked");
        QVERIFY(rig.spotify->blendSearch());
        QVERIFY(rig.library->currentKind() != "spotify");
        QMetaObject::invokeMethod(rig.find("searchScope_local"), "clicked");
        QVERIFY(!rig.spotify->blendSearch());
        QMetaObject::invokeMethod(rig.find("searchScope_spotify"), "clicked");
        QTRY_COMPARE(rig.library->currentKind(), QString("spotify"));
        QTRY_VERIFY(box->property("activeFocus").toBool());
        rig.spotify->setEnabled(false);
        QVERIFY(on.item->property("shown").toBool());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // The hint about Tab shows on the deck until closed or the library is
    // opened, and never again after.
    void libraryHintShowsOnce() {
        QTemporaryDir dir;
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", false);
        }
        {
            Rig rig(dir);
            QVERIFY(rig.load());
            QObject *hint = rig.find("libraryHint");
            QVERIFY(hint && hint->property("visible").toBool());
            QMetaObject::invokeMethod(rig.find("libraryHintClose"), "clicked");
            QVERIFY(!hint->property("visible").toBool());
            rig.window->close();
        }
        {
            Rig rig(dir);
            QVERIFY(rig.load());
            QVERIFY(!rig.find("libraryHint")->property("visible").toBool());
        }
        // Opening the library counts as having seen it.
        QTemporaryDir second;
        writeConf(second.filePath("window.conf"), false, false);
        {
            Rig rig(second);
            QVERIFY(rig.load());
            QObject *hint = rig.find("libraryHint");
            QVERIFY(hint->property("visible").toBool());
            QTest::keyClick(rig.window, Qt::Key_Tab);
            QVERIFY(rig.advanced());
            QTest::keyClick(rig.window, Qt::Key_Tab);
            QVERIFY(!rig.advanced());
            QVERIFY(!hint->property("visible").toBool());
        }
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // Ctrl+, and the other window keys work wherever the keys are: the
    // deck, the library's list, its search box, the tree.
    void keysFromAnyFocus() {
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        writeConf(dir.filePath("window.conf"), false);
        Rig rig(dir);
        QVERIFY(rig.load());
        QObject *settings = rig.find("settingsSheet");
        QVERIFY(settings);
        auto settingsOpens = [&](const char *where) {
            QTest::keyClick(rig.window, Qt::Key_Comma, Qt::ControlModifier);
            const bool opened = QTest::qWaitFor([&] { return settings->property("opened").toBool(); }, 2000);
            rig.closePopups();
            QTest::qWaitFor([&] { return !settings->property("opened").toBool(); }, 2000);
            if (!opened)
                qWarning("Ctrl+, didn't open Settings from %s", where);
            return opened;
        };
        QVERIFY(settingsOpens("the deck"));
        QTest::keyClick(rig.window, Qt::Key_Tab);
        QVERIFY(rig.advanced());
        QTRY_VERIFY(rig.library->ready());
        QVERIFY(settingsOpens("the track list"));
        QTest::keyClick(rig.window, Qt::Key_Slash);
        QObject *search = rig.find("searchBox");
        QTRY_VERIFY(search->property("activeFocus").toBool());
        QVERIFY(settingsOpens("the search box"));
        QObject *view = rig.find("advancedView");
        QMetaObject::invokeMethod(view, "focusTree");
        QVERIFY(settingsOpens("the tree"));
        // The palette's and the menu's other keys from the search box.
        QTest::keyClick(rig.window, Qt::Key_Slash);
        QTRY_VERIFY(search->property("activeFocus").toBool());
        QTest::keyClick(rig.window, Qt::Key_K, Qt::ControlModifier);
        QObject *palette = rig.find("commandPalette");
        QTRY_VERIFY(palette->property("opened").toBool());
        rig.closePopups();
        QTRY_VERIFY(!palette->property("opened").toBool());
        // Edit → Find from the deck goes to the library's search box.
        rig.run("view-player");
        QVERIFY(rig.trigger(rig.entry("menuFind").item));
        QVERIFY(rig.advanced());
        QTRY_VERIFY(search->property("activeFocus").toBool());
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // A tiled window keeps the compositor's size when switching modes; a
    // floating one takes each mode's own remembered size.
    void tiledWindowNeverResizes() {
        QTemporaryDir dir;
        writeConf(dir.filePath("window.conf"), false);
        // A pretend hyprctl that lists this process's window as tiled.
        const QString hyprctl = dir.filePath("hyprctl");
        {
            QFile f(hyprctl);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QString("#!/bin/sh\necho '[{\"pid\":%1,\"floating\":false,\"fullscreen\":0}]'\n")
                        .arg(QCoreApplication::applicationPid())
                        .toUtf8());
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        {
            Rig rig(dir);
            QVERIFY(rig.load());
            // Floating (nothing asked yet): the library's own size.
            QCOMPARE(rig.window->width(), 500);
            rig.run("view-library");
            QTRY_COMPARE(rig.window->width(), 1360);
            rig.run("view-player");
            QTRY_COMPARE(rig.window->width(), 500);
        }
        {
            Rig rig(dir, hyprctl);
            QVERIFY(rig.load());
            rig.placement->refresh();
            QTRY_VERIFY(rig.placement->managed());
            QVERIFY(rig.window->property("sizeManaged").toBool());
            // As Hyprland would have it: the tile's size.
            rig.window->resize(1700, 1000);
            QTRY_COMPARE(rig.window->width(), 1700);
            rig.run("view-library");
            QVERIFY(rig.advanced());
            QTest::qWait(50);
            QCOMPARE(rig.window->width(), 1700);
            QCOMPARE(rig.window->height(), 1000);
            rig.run("mini-player");
            QTest::qWait(50);
            QCOMPARE(rig.window->width(), 1700);
            rig.run("mini-player");
            rig.run("view-player");
            QCOMPARE(rig.window->width(), 1700);
            rig.window->close();
        }
        // The tile's size wasn't kept as the floating sizes.
        QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
        QCOMPARE(conf.value("window/advancedWidth").toInt(), 1360);
        QCOMPARE(conf.value("window/deckWidth", 500).toInt(), 500);
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }

    // Tab into the library: built while the deck sat idle, the first switch
    // only shows it. Prints both times for the progress log.
    void tabIsQuickAfterStartUp() {
        auto timeSwitch = [](Rig &rig) {
            QElapsedTimer t;
            t.start();
            rig.run("view-library");
            rig.window->grabWindow(); // the frame, rendered
            return t.nsecsElapsed() / 1e6;
        };
        double cold = 0, warm = 0;
        {
            QTemporaryDir dir;
            QVERIFY(fillLibrary(dir.filePath("library.db")));
            writeConf(dir.filePath("window.conf"), false);
            Rig rig(dir);
            QVERIFY(rig.load());
            QVERIFY(!rig.find("advancedView"));
            cold = timeSwitch(rig); // before the idle build, as every first Tab was
        }
        {
            QTemporaryDir dir;
            QVERIFY(fillLibrary(dir.filePath("library.db")));
            writeConf(dir.filePath("window.conf"), false);
            Rig rig(dir);
            QVERIFY(rig.load());
            QTRY_VERIFY_WITH_TIMEOUT(rig.find("advancedView") != nullptr, 5000);
            QTRY_VERIFY(rig.library->ready());
            // Still on the deck, with the deck's keys.
            QVERIFY(!rig.advanced());
            QVERIFY(!rig.find("advancedView")->property("visible").toBool());
            warm = timeSwitch(rig);
        }
        qInfo("Tab into the library: %.1f ms on a cold start, %.1f ms after the idle build", cold, warm);
        QVERIFY(warm < cold);
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join("\n")));
    }
};

int runMenuTests(int argc, char **argv) {
    MenuTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "menu_tests.moc"
