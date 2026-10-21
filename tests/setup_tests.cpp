// The first-run setup, the existing-user banner and the sidebar now on by
// default (OMAMP_TESTS=setup).
//
// Offscreen, in software, with no audio device, each window with a
// window.conf in its own temporary folder. Nothing here reaches the user's
// own settings or the internet.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "fake_hyprctl.h"
#include "firstrun.h"
#include "layoutprefs.h"
#include "librarycontroller.h"
#include "nowplaying_context.h"
#include "player.h"
#include "theme.h"
#include "translator.h"
#include "windowplacement.h"

class SetupTests : public QObject {
    Q_OBJECT

    // The real window over a window.conf in dir, with a FirstRun that reads
    // it first, as main() does.
    struct Window {
        QTemporaryDir &dir;
        FirstRun firstRun;
        Translator translator;
        Theme theme;
        Player player;
        LibraryController library;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;
        WindowPlacement *placement = nullptr;

        // hyprctl: a pretend one (fake_hyprctl.h); none means no Hyprland.
        explicit Window(QTemporaryDir &d, const QString &hyprctl = {})
            : dir(d), firstRun(d.filePath("window.conf")), translator(d.filePath("language")),
              theme(d.filePath("theme"), nullptr, d.filePath("no-hyprctl")), library(d.filePath("library.db")) {
            player.setMuted(true);
            library.setPlayer(&player);
            library.setWatching(false);
            library.setArtCacheDir(d.filePath("art"));
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("backend", &player);
            engine.rootContext()->setContextProperty("library", &library);
            engine.rootContext()->setContextProperty("commandPalette", library.palette());
            engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(d.filePath("window.conf")));
            installNowPlaying(engine, &theme, &player, d.filePath("library.db"), d.filePath("art"),
                              d.filePath("window.conf"));
            engine.rootContext()->setContextProperty("firstRun", &firstRun);
            if (!hyprctl.isEmpty()) {
                placement = new WindowPlacement(&engine, hyprctl);
                engine.rootContext()->setContextProperty("windowPlacement", placement);
            }
        }
        bool load() {
            engine.load(QUrl("qrc:/Main.qml"));
            if (engine.rootObjects().size() != 1)
                return false;
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            return window && QTest::qWaitForWindowExposed(window);
        }
        static QObject *findIn(QObject *o, const QString &name) {
            if (o->objectName() == name)
                return o;
            for (QObject *child : o->children())
                if (QObject *found = findIn(child, name))
                    return found;
            if (auto *item = qobject_cast<QQuickItem *>(o))
                for (QQuickItem *child : item->childItems())
                    if (QObject *found = findIn(child, name))
                        return found;
            return nullptr;
        }
        QObject *find(const QString &name) const {
            if (QObject *o = findIn(window, name))
                return o;
            return findIn(window->contentItem(), name);
        }
        QObject *wizard() const { return find("setupWizard"); }
        bool wizardOpen() const {
            QObject *w = wizard();
            return w && w->property("opened").toBool();
        }
        QVariant saved(const QString &key) const {
            return QSettings(dir.filePath("window.conf"), QSettings::IniFormat).value(key);
        }
        QObject *appearance() const { return engine.rootContext()->contextProperty("appearance").value<QObject *>(); }
        LayoutPrefs *layout() const { return appearance()->property("layout").value<LayoutPrefs *>(); }
        // Main.qml's Settings { id: prefs }, as the setup holds it.
        QObject *prefsObject() const { return wizard()->property("prefs").value<QObject *>(); }
        bool advanced() const { return window->property("advanced").toBool(); }
        bool sidebar() const { return prefsObject()->property("infoVisible").toBool(); }
        void run(const QString &action) {
            QMetaObject::invokeMethod(window, "runAction", Q_ARG(QVariant, QVariant(action)));
        }
        // A real mouse click in the middle of a named item.
        bool click(const QString &name) {
            auto *item = qobject_cast<QQuickItem *>(find(name));
            if (!item || !item->isVisible())
                return false;
            const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
            return true;
        }
        int step() const { return wizard()->property("step").toInt(); }
        QSize size() const { return window->size(); }
        // The window's content (menu bar and all) fills it exactly: no gap
        // below or beside it, and nothing hanging off.
        QString gap() const {
            QQuickItem *content = window->contentItem();
            auto *bar = qobject_cast<QQuickItem *>(window->property("menuBar").value<QObject *>());
            const qreal barHeight = bar && bar->isVisible() ? bar->height() : 0;
            if (qAbs(content->width() - window->width()) > 0.5 || qAbs(content->height() - window->height()) > 0.5)
                return QString("content %1 x %2 in a %3 x %4 window")
                    .arg(content->width()).arg(content->height()).arg(window->width()).arg(window->height());
            // The window's root item (the deck or the library inside it)
            // fills what the menu bar leaves.
            if (!rootItem)
                rootItem = qobject_cast<QQuickItem *>(find("windowRoot"));
            QQuickItem *root = rootItem;
            if (!root || qAbs(root->width() - window->width()) > 0.5
                || qAbs(root->height() + barHeight - window->height()) > 0.5)
                return QString("root %1 x %2 + bar %3 in a %4 x %5 window").arg(root ? root->width() : -1)
                    .arg(root ? root->height() : -1).arg(barHeight).arg(window->width()).arg(window->height());
            return {};
        }
        mutable QPointer<QQuickItem> nextButton, rootItem;
        // The panel, and its Next button, inside the window.
        QString panelOutside() const {
            QObject *w = wizard();
            const qreal x = w->property("x").toReal(), y = w->property("y").toReal();
            const qreal width = w->property("width").toReal(), height = w->property("height").toReal();
            // Found once: walking the whole tree on every try is slow.
            if (!nextButton)
                nextButton = qobject_cast<QQuickItem *>(find("setupNext"));
            QQuickItem *next = nextButton;
            const QPointF corner = next ? next->mapToScene(QPointF(next->width(), next->height())) : QPointF(1e9, 1e9);
            if (x < 0 || y < 0 || x + width > window->width() + 0.5 || y + height > window->height() + 0.5
                || corner.x() > window->width() + 0.5 || corner.y() > window->height() + 0.5 || !next->isVisible())
                return QString("panel %1,%2 %3 x %4, Next's corner %5,%6, in %7 x %8 (step %9)")
                    .arg(x).arg(y).arg(width).arg(height).arg(corner.x()).arg(corner.y())
                    .arg(window->width()).arg(window->height()).arg(step());
            return {};
        }
    };

    // The pretend Hyprland's idea of the window's size.
    static QSize hyprSize(const QString &dir) {
        QFile f(dir + "/size");
        f.open(QIODevice::ReadOnly);
        const QStringList wh = QString::fromUtf8(f.readAll()).trimmed().split(',');
        return QSize(wh.value(0).toInt(), wh.value(1).toInt());
    }
    static QStringList resizeBatches(const QString &dir) {
        QStringList out;
        for (const QString &b : batches(dir))
            if (b.contains("resize(") || b.contains("move("))
                out << b;
        return out;
    }

    static void writeConf(QTemporaryDir &dir, const QMap<QString, QVariant> &values) {
        QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            conf.setValue(it.key(), it.value());
    }

private slots:
    // A new install: the setup is due and the sidebar comes on, once.
    void newInstallNeedsSetup() {
        QTemporaryDir dir;
        const QString conf = dir.filePath("omamp/window.conf");
        {
            FirstRun run(conf);
            QCOMPARE(run.origin(), QString("new"));
            QVERIFY(run.wizardPending());
            QVERIFY(!run.bannerPending());
            QVERIFY(!run.completed());
        }
        QSettings s(conf, QSettings::IniFormat);
        QCOMPARE(s.value("window/infoVisible").toBool(), true);
        QCOMPARE(s.value("setup/migration").toInt(), FirstRun::migrationVersion);
        {
            // Quit half-way: still a new install, still due.
            FirstRun again(conf);
            QCOMPARE(again.origin(), QString("new"));
            QVERIFY(again.wizardPending());
            QSignalSpy changed(&again, &FirstRun::changed);
            again.markCompleted();
            QCOMPARE(changed.count(), 1);
            QVERIFY(!again.wizardPending());
        }
        FirstRun done(conf);
        QVERIFY(done.completed());
        QVERIFY(!done.wizardPending());
        QVERIFY(!done.bannerPending());
    }

    // An older install: no setup forced, a banner instead, the sidebar on
    // once and then the person's own choice, and nothing else touched.
    void existingInstallGetsBannerAndSidebarOnce() {
        QTemporaryDir dir;
        writeConf(dir, {{"window/infoVisible", false}, {"window/advanced", true},
                        {"appearance/deckStyle", "classic"}, {"layout/barSize", "large"}});
        const QString conf = dir.filePath("window.conf");
        {
            FirstRun run(conf);
            QCOMPARE(run.origin(), QString("existing"));
            QVERIFY(!run.wizardPending());
            QVERIFY(run.bannerPending());
        }
        {
            QSettings s(conf, QSettings::IniFormat);
            QCOMPARE(s.value("window/infoVisible").toBool(), true);
            QCOMPARE(s.value("window/advanced").toBool(), true);
            QCOMPARE(s.value("appearance/deckStyle").toString(), QString("classic"));
            QCOMPARE(s.value("layout/barSize").toString(), QString("large"));
            // They switch it off again: it stays off from now on.
            s.setValue("window/infoVisible", false);
        }
        {
            FirstRun run(conf);
            QVERIFY(run.bannerPending());
            run.dismissBanner();
            QVERIFY(!run.bannerPending());
            QVERIFY(!run.completed());
        }
        FirstRun later(conf);
        QVERIFY(!later.bannerPending());
        QVERIFY(!later.wizardPending());
        QCOMPARE(QSettings(conf, QSettings::IniFormat).value("window/infoVisible").toBool(), false);
    }

    // Without a window.conf (tests, mostly) there is nothing to set up.
    void inMemoryIsAlreadySetUp() {
        FirstRun run;
        QVERIFY(run.completed());
        QVERIFY(!run.wizardPending());
        QVERIFY(!run.bannerPending());
    }

    // The first start opens the setup; every step shows in the window as it
    // is chosen; Finish keeps it all and it doesn't open again.
    void wizardOnFirstRunAppliesLiveAndFinishes() {
        QTemporaryDir dir;
        {
            Window w(dir);
            QVERIFY(w.load());
            QTRY_VERIFY(w.wizardOpen());
            QVERIFY(w.window->property("sheetOpen").toBool());
            QCOMPARE(w.step(), 0);
            // The sidebar is on for a new install.
            QVERIFY(w.sidebar());

            // 1 Start: the Library card shows the library at once.
            QVERIFY(!w.advanced());
            QVERIFY(w.click("setupStart_library"));
            QTRY_VERIFY(w.advanced());
            QCOMPARE(w.wizard()->property("startView").toString(), QString("library"));
            QVERIFY(w.click("setupStart_player"));
            QTRY_VERIFY(!w.advanced());
            QVERIFY(w.click("setupStart_library"));
            QTRY_VERIFY(w.advanced());

            // Enter is Next. 2 Look shows the deck.
            QTest::keyClick(w.window, Qt::Key_Return);
            QTRY_COMPARE(w.step(), 1);
            QTRY_VERIFY(!w.advanced());
            QVERIFY(w.click("setupDeckStyle_classic"));
            QTRY_COMPARE(w.appearance()->property("deckStyle").toString(), QString("classic"));
            // Every visualiser the build has, each drawing its own preview.
            const QStringList styles = w.appearance()->property("visualisers").toStringList();
            QVERIFY(styles.size() >= 5);
            for (const QString &style : styles)
                QVERIFY2(w.find("setupVisualiser_" + style), qPrintable(style));
            QVERIFY(w.click("setupVisualiser_" + styles.value(3)));
            QTRY_COMPARE(w.appearance()->property("visualiser").toString(), styles.value(3));
            auto *preview = w.find("setupVisualiserPreview");
            QVERIFY(preview);
            const int frames = preview->property("frames").toInt();
            QTRY_VERIFY(preview->property("frames").toInt() > frames + 3);

            // 3 Layout shows the library; Spotify-like sets the sidebar, the
            // large bar at the bottom and the covers, and the panel moves up.
            QVERIFY(w.click("setupNext"));
            QTRY_COMPARE(w.step(), 2);
            QTRY_VERIFY(w.advanced());
            QVERIFY(w.click("setupPreset_classic"));
            QTRY_VERIFY(!w.sidebar());
            QVERIFY(!w.layout()->artColumn());
            QVERIFY(w.prefsObject()->property("browserVisible").toBool());
            QVERIFY(w.click("setupPreset_spotify"));
            QTRY_VERIFY(w.sidebar());
            QCOMPARE(w.layout()->barSize(), QString("large"));
            QCOMPARE(w.layout()->barPosition(), QString("bottom"));
            QVERIFY(w.layout()->artColumn());
            QVERIFY(!w.prefsObject()->property("browserVisible").toBool());
            QTRY_VERIFY(w.find("setupPreset_spotify")->property("chosen").toBool());
            QTRY_VERIFY(w.wizard()->property("y").toReal() < 80);
            QTRY_VERIFY(w.find("bigBar"));
            // The quick switches.
            QVERIFY(w.click("setupBarBottom"));
            QTRY_COMPARE(w.layout()->barPosition(), QString("top"));
            QTRY_VERIFY(w.wizard()->property("y").toReal() > w.window->height() / 3);
            QVERIFY(!w.find("setupPreset_spotify")->property("chosen").toBool());

            // 4 Your music, then 5 Done; Back goes back.
            QVERIFY(w.click("setupNext"));
            QTRY_COMPARE(w.step(), 3);
            QVERIFY(w.find("setupAddFolder"));
            QVERIFY(w.click("setupBack"));
            QTRY_COMPARE(w.step(), 2);
            QTest::keyClick(w.window, Qt::Key_Return);
            QTest::keyClick(w.window, Qt::Key_Return);
            QTRY_COMPARE(w.step(), 4);
            QCOMPARE(w.find("setupNext")->property("text").toString(), QString("Finish"));
            QTest::keyClick(w.window, Qt::Key_Return);
            QTRY_VERIFY(!w.wizardOpen());
            QVERIFY(!w.window->property("sheetOpen").toBool());
            // It ends in the chosen start view, everything kept.
            QTRY_VERIFY(w.advanced());
            QVERIFY(w.firstRun.completed());
            QCOMPARE(w.saved("setup/completed").toBool(), true);
            QTRY_COMPARE(w.saved("appearance/deckStyle").toString(), QString("classic"));
            QCOMPARE(w.saved("appearance/visualiser").toString(), styles.value(3));
            QCOMPARE(w.saved("layout/barSize").toString(), QString("large"));
            QCOMPARE(w.saved("layout/barPosition").toString(), QString("top"));
            QTRY_COMPARE(w.saved("window/advanced").toBool(), true);
            QTRY_COMPARE(w.saved("window/infoVisible").toBool(), true);
            QVERIFY(!w.find("setupBanner")->property("visible").toBool());
            w.window->close();
        }
        // The next start: no setup, the choices as they were left.
        Window again(dir);
        QVERIFY(again.load());
        QTest::qWait(200);
        QVERIFY(!again.wizardOpen());
        QVERIFY(again.advanced());
        QCOMPARE(again.appearance()->property("deckStyle").toString(), QString("classic"));
        QCOMPARE(again.layout()->barSize(), QString("large"));
    }

    // Esc asks; skipping puts back what was there and counts as done.
    void skipKeepsDefaults() {
        QTemporaryDir dir;
        Window w(dir);
        QVERIFY(w.load());
        QTRY_VERIFY(w.wizardOpen());
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.step(), 1);
        QVERIFY(w.click("setupDeckStyle_silver"));
        QTRY_COMPARE(w.appearance()->property("deckStyle").toString(), QString("silver"));
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(w.find("setupSkipConfirm")->property("visible").toBool());
        // Esc again changes its mind.
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!w.find("setupSkipConfirm")->property("visible").toBool());
        QVERIFY(w.wizardOpen());
        QVERIFY(w.click("setupSkip"));
        QTRY_VERIFY(w.find("setupSkipConfirm")->property("visible").toBool());
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!w.wizardOpen());
        QCOMPARE(w.appearance()->property("deckStyle").toString(), QString("omarchy"));
        QCOMPARE(w.appearance()->property("visualiser").toString(), QString("peaks"));
        QVERIFY(w.sidebar());
        QVERIFY(!w.advanced());
        QCOMPARE(w.layout()->barSize(), QString("compact"));
        QVERIFY(w.firstRun.completed());
        QCOMPARE(w.saved("setup/completed").toBool(), true);
    }

    // An older install: no setup at start, the banner once, the person's
    // choices untouched; the banner's button and Help both open the setup.
    void existingInstallBannerAndRerun() {
        QTemporaryDir dir;
        writeConf(dir, {{"window/advanced", true}, {"window/libraryHintDone", true}, {"window/infoVisible", false},
                        {"appearance/deckStyle", "classic"}, {"appearance/visualiser", "fire"}});
        {
            Window w(dir);
            QVERIFY(w.load());
            QTest::qWait(200);
            QVERIFY(!w.wizardOpen());
            QVERIFY(w.advanced());
            QCOMPARE(w.appearance()->property("deckStyle").toString(), QString("classic"));
            QCOMPARE(w.appearance()->property("visualiser").toString(), QString("fire"));
            // The sidebar came on once.
            QVERIFY(w.sidebar());
            QTRY_VERIFY(w.find("setupBanner")->property("visible").toBool());
            QVERIFY(w.click("setupBannerClose"));
            QTRY_VERIFY(!w.find("setupBanner")->property("visible").toBool());
            QCOMPARE(w.saved("setup/bannerDismissed").toBool(), true);
            // Their choice from now on.
            w.run("info");
            QTRY_VERIFY(!w.sidebar());
            w.window->close();
        }
        {
            Window w(dir);
            QVERIFY(w.load());
            QTest::qWait(200);
            QVERIFY(!w.find("setupBanner")->property("visible").toBool());
            QVERIFY(!w.wizardOpen());
            QVERIFY(!w.sidebar());
            // Help → Run setup again… (and the palette's "setup").
            QVERIFY(w.find("menuRunSetup"));
            w.run("setup");
            QTRY_VERIFY(w.wizardOpen());
            QCOMPARE(w.step(), 0);
            QCOMPARE(w.wizard()->property("startView").toString(), QString("library"));
            // Skipping a re-run changes nothing.
            QTest::keyClick(w.window, Qt::Key_Escape);
            QTest::keyClick(w.window, Qt::Key_Return);
            QTRY_VERIFY(!w.wizardOpen());
            QVERIFY(w.advanced());
            QCOMPARE(w.appearance()->property("deckStyle").toString(), QString("classic"));
            QVERIFY(!w.sidebar());
        }
    }

    // The banner's Run setup opens it, and finishing clears the banner.
    void bannerRunsSetup() {
        QTemporaryDir dir;
        writeConf(dir, {{"window/libraryHintDone", true}});
        Window w(dir);
        QVERIFY(w.load());
        QTRY_VERIFY(w.find("setupBanner")->property("visible").toBool());
        QVERIFY(w.click("setupBannerRun"));
        QTRY_VERIFY(w.wizardOpen());
        QVERIFY(!w.find("setupBanner")->property("visible").toBool());
        for (int i = 0; i < 5; ++i)
            QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!w.wizardOpen());
        QVERIFY(!w.firstRun.bannerPending());
        QVERIFY(!w.find("setupBanner")->property("visible").toBool());
    }

    // The Lua that sizes a floating window: kept within the monitor's free
    // area, grown only when asked to grow, and moved back on when it would
    // hang off.
    void floatingResizeCommands() {
        const QJsonArray monitors = QJsonDocument::fromJson(R"([
            {"id":1,"x":6144,"y":0,"width":3840,"height":2160,"scale":1.5,"transform":0,"reserved":[0,32,0,0]}])").array();
        auto client = [](int x, int y, int w, int h) {
            return QJsonObject{{"address", "0x5afe"}, {"monitor", 1}, {"floating", true},
                               {"at", QJsonArray{x, y}}, {"size", QJsonArray{w, h}}};
        };
        const QString resize = R"(hl.dsp.window.resize({ window = "address:0x5afe", x = %1, y = %2, exact = true }))";
        const QString move = R"(hl.dsp.window.move({ window = "address:0x5afe", x = %1, y = %2, exact = true }))";
        QSize size;
        QCOMPARE(WindowPlacement::resizeCommands(client(6244, 200, 500, 680), monitors, 1360, 820, false, &size),
                 QStringList{resize.arg(1360).arg(820)});
        QCOMPARE(size, QSize(1360, 820));
        // Already that size: nothing to send.
        QVERIFY(WindowPlacement::resizeCommands(client(6244, 200, 1360, 820), monitors, 1360, 820).isEmpty());
        // Growing only: each side at least as large as now.
        QCOMPARE(WindowPlacement::resizeCommands(client(6244, 200, 1100, 600), monitors, 900, 760, true, &size),
                 QStringList{resize.arg(1100).arg(760)});
        QVERIFY(WindowPlacement::resizeCommands(client(6244, 200, 1688, 1137), monitors, 900, 760, true).isEmpty());
        // Larger than the free area (2560 x 1408 below a 32 px bar): the
        // area's size, in its middle.
        QCOMPARE(WindowPlacement::resizeCommands(client(6244, 200, 500, 680), monitors, 3000, 2000, false, &size),
                 (QStringList{resize.arg(2560).arg(1408), move.arg(6144).arg(32)}));
        QCOMPARE(size, QSize(2560, 1408));
        // Would hang off the right edge: centred.
        QCOMPARE(WindowPlacement::resizeCommands(client(8400, 900, 500, 680), monitors, 1360, 820),
                 (QStringList{resize.arg(1360).arg(820), move.arg(6144 + (2560 - 1360) / 2).arg(32 + (1408 - 820) / 2)}));
        // Unknown monitor: nothing.
        QJsonObject lost = client(0, 0, 500, 680);
        lost["monitor"] = 7;
        QVERIFY(WindowPlacement::resizeCommands(lost, monitors, 1360, 820).isEmpty());
    }

    // The panel and its Next button stay inside the window at every step,
    // small or large, with the panel at the top or the bottom and with the
    // skip question showing.
    void panelStaysInsideAtEveryStep() {
        QTemporaryDir dir;
        Window w(dir);
        QVERIFY(w.load());
        QTRY_VERIFY(w.wizardOpen());
        for (const QSize size : {QSize(440, 460), QSize(520, 560), QSize(900, 760), QSize(1600, 1000)}) {
            w.window->resize(size);
            QTRY_COMPARE(w.size(), size);
            for (int step = 0; step < 5; ++step) {
                QMetaObject::invokeMethod(w.wizard(), "goTo", Q_ARG(QVariant, step));
                QTRY_COMPARE(w.step(), step);
                QTRY_VERIFY2(w.panelOutside().isEmpty(), qPrintable(w.panelOutside()));
                if (step == 2) {
                    // The large bar at the bottom puts the panel at the top.
                    w.layout()->setBarPosition("bottom");
                    QTRY_VERIFY(w.wizard()->property("dockTop").toBool());
                    QTRY_VERIFY2(w.panelOutside().isEmpty(), qPrintable(w.panelOutside()));
                    w.layout()->setBarPosition("top");
                }
                w.wizard()->setProperty("confirmingSkip", true);
                QTRY_VERIFY2(w.panelOutside().isEmpty(), qPrintable(w.panelOutside()));
                w.wizard()->setProperty("confirmingSkip", false);
                // The window doesn't change size under the setup.
                QCOMPARE(w.size(), size);
            }
        }
    }

    // A floating window on Hyprland, too small for the setup: Hyprland is
    // asked to grow it, and it then keeps that size through every step (the
    // deck's preview shows in it); Finish asks Hyprland for the start view's
    // own size, and the content fills the window it gets.
    void floatingWindowGrowsThenSettlesOnFinish() {
        QTemporaryDir dir;
        const QString hyprctl = fakeHyprctl(dir.path(), true, "500,680", "6244,200");
        Window w(dir, hyprctl);
        QVERIFY(w.load());
        QTRY_VERIFY(w.wizardOpen());
        QTRY_COMPARE(w.size(), QSize(900, 760));
        QTRY_COMPARE(hyprSize(dir.path()), QSize(900, 760));
        QVERIFY(resizeBatches(dir.path()).value(0).contains("x = 900, y = 760, exact"));
        QVERIFY(!w.placement->managed());
        QVERIFY(w.click("setupStart_library"));
        QTRY_VERIFY(w.advanced());
        for (int step = 0; step < 5; ++step) {
            if (step > 0) {
                QVERIFY(w.click("setupNext"));
                QTRY_COMPARE(w.step(), step);
            }
            QTest::qWait(100);
            QCOMPARE(w.size(), QSize(900, 760));
            QCOMPARE(hyprSize(dir.path()), QSize(900, 760));
            QVERIFY2(w.panelOutside().isEmpty(), qPrintable(w.panelOutside()));
            QVERIFY2(w.gap().isEmpty(), qPrintable(w.gap()));
        }
        // Only the one resize while the setup was open.
        QCOMPARE(resizeBatches(dir.path()).size(), 1);
        QVERIFY(w.click("setupNext")); // Finish
        QTRY_VERIFY(!w.wizardOpen());
        QVERIFY(w.advanced());
        QTRY_COMPARE(hyprSize(dir.path()), QSize(1360, 820));
        QTRY_COMPARE(w.size(), QSize(1360, 820));
        QTRY_VERIFY2(w.gap().isEmpty(), qPrintable(w.gap()));
        QTRY_COMPARE(w.saved("window/advancedWidth").toInt(), 1360);
        QCOMPARE(w.saved("window/advancedHeight").toInt(), 820);
        // The deck kept its own size from before the setup.
        QCOMPARE(w.saved("window/deckWidth").toInt(), 500);
        QCOMPARE(w.saved("window/deckHeight").toInt(), 680);
        // Tab back to the deck: Hyprland sizes it too, no gap.
        QTest::keyClick(w.window, Qt::Key_Tab);
        QTRY_VERIFY(!w.advanced());
        QTRY_COMPARE(hyprSize(dir.path()), QSize(500, 680));
        QTRY_COMPARE(w.size(), QSize(500, 680));
        QTRY_VERIFY2(w.gap().isEmpty(), qPrintable(w.gap()));
    }

    // Skip, floating: back to the view and size from before the setup.
    void floatingWindowSettlesOnSkip() {
        QTemporaryDir dir;
        const QString hyprctl = fakeHyprctl(dir.path(), true, "500,680", "6244,200");
        Window w(dir, hyprctl);
        QVERIFY(w.load());
        QTRY_VERIFY(w.wizardOpen());
        QTRY_COMPARE(w.size(), QSize(900, 760));
        QVERIFY(w.click("setupStart_library"));
        QTRY_VERIFY(w.advanced());
        QVERIFY(w.click("setupNext"));
        QTRY_COMPARE(w.step(), 1);
        QVERIFY(w.click("setupSkip"));
        QVERIFY(w.click("setupSkipYes"));
        QTRY_VERIFY(!w.wizardOpen());
        QVERIFY(!w.advanced());
        QTRY_COMPARE(hyprSize(dir.path()), QSize(500, 680));
        QTRY_COMPARE(w.size(), QSize(500, 680));
        QTRY_VERIFY2(w.gap().isEmpty(), qPrintable(w.gap()));
    }

    // A large floating window isn't shrunk while the setup is open, and
    // Hyprland's own size wins: asked for more than the monitor has, the
    // window takes what Hyprland gives and the content fills it.
    void floatingWindowTakesHyprlandsSize() {
        QTemporaryDir dir;
        // Matt's window: 1688 x 1137 on a floating workspace; the library
        // remembered taller than the monitor's free area.
        writeConf(dir, {{"window/deckWidth", 1688}, {"window/deckHeight", 1137},
                        {"window/advancedWidth", 1500}, {"window/advancedHeight", 1900}});
        const QString hyprctl = fakeHyprctl(dir.path(), true, "1688,1137", "6300,100");
        Window w(dir, hyprctl);
        QVERIFY(w.load());
        QTRY_COMPARE(w.size(), QSize(1688, 1137));
        // An older install: run it from Help.
        w.run("setup");
        QTRY_VERIFY(w.wizardOpen());
        QTest::qWait(300);
        QCOMPARE(w.size(), QSize(1688, 1137));
        QVERIFY(resizeBatches(dir.path()).isEmpty());
        QVERIFY(w.click("setupStart_library"));
        for (int i = 0; i < 5; ++i)
            QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!w.wizardOpen());
        QVERIFY(w.advanced());
        // 1500 x 1900 doesn't fit the 2560 x 1408 free area.
        QTRY_COMPARE(hyprSize(dir.path()), QSize(1500, 1408));
        QTRY_COMPARE(w.size(), QSize(1500, 1408));
        QTRY_VERIFY2(w.gap().isEmpty(), qPrintable(w.gap()));
        QTRY_COMPARE(w.saved("window/advancedHeight").toInt(), 1408);
    }

    // Tiled: the compositor decides; the setup never asks for a size.
    void tiledWindowKeepsItsSize() {
        QTemporaryDir dir;
        const QString hyprctl = fakeHyprctl(dir.path(), false);
        Window w(dir, hyprctl);
        QVERIFY(w.load());
        w.placement->refresh();
        QTRY_VERIFY(w.placement->managed());
        // As Hyprland would have it: the tile's size.
        w.window->resize(1700, 1000);
        QTRY_COMPARE(w.size(), QSize(1700, 1000));
        QTRY_VERIFY(w.wizardOpen());
        QVERIFY(w.click("setupStart_library"));
        for (int i = 0; i < 5; ++i) {
            QTest::keyClick(w.window, Qt::Key_Return);
            QTest::qWait(50);
            QCOMPARE(w.size(), QSize(1700, 1000));
        }
        QTRY_VERIFY(!w.wizardOpen());
        QTest::qWait(200);
        QCOMPARE(w.size(), QSize(1700, 1000));
        QVERIFY(w.gap().isEmpty());
        QVERIFY(batches(dir.path()).isEmpty());
        // The tile's size isn't kept as the library's floating size.
        QVERIFY(w.saved("window/advancedWidth").toInt() != 1700);
    }

    // The setup fits the deck's smallest window.
    void fitsTheSmallestWindow() {
        QTemporaryDir dir;
        Window w(dir);
        QVERIFY(w.load());
        QTRY_VERIFY(w.wizardOpen());
        w.window->resize(440, 460);
        QTRY_COMPARE(w.window->width(), 440);
        auto *panel = qobject_cast<QQuickItem *>(w.wizard()->property("contentItem").value<QObject *>());
        QVERIFY(panel);
        QTRY_VERIFY(w.wizard()->property("width").toReal() <= 440);
        const qreal width = w.wizard()->property("width").toReal();
        const qreal height = w.wizard()->property("height").toReal();
        QVERIFY2(width <= 440 && height <= 460, qPrintable(QString("%1 x %2 in %3 x %4").arg(width).arg(height)
                                                            .arg(w.window->width()).arg(w.window->height())));
        auto *next = qobject_cast<QQuickItem *>(w.find("setupNext"));
        const QPointF corner = next->mapToScene(QPointF(next->width(), next->height()));
        QVERIFY(corner.x() <= 440 && corner.y() <= 460);
        QVERIFY(w.click("setupNext"));
        QTRY_COMPARE(w.step(), 1);
    }
};

int runSetupTests(int argc, char **argv) {
    SetupTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "setup_tests.moc"
