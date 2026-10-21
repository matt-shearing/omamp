// The first-run setup, the existing-user banner and the sidebar now on by
// default (OMAMP_TESTS=setup).
//
// Offscreen, in software, with no audio device, each window with a
// window.conf in its own temporary folder. Nothing here reaches the user's
// own settings or the internet.
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "firstrun.h"
#include "layoutprefs.h"
#include "librarycontroller.h"
#include "nowplaying_context.h"
#include "player.h"
#include "theme.h"
#include "translator.h"

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

        explicit Window(QTemporaryDir &d)
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
    };

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
            QTRY_VERIFY(w.wizard()->property("y").toReal() < 20);
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
