// The visualisers, and the large now playing bar's visualiser
// (OMAMP_TESTS=visualiser).
//
// Offscreen, in software, with no audio device and a temporary XDG home.
// Every style is drawn at several sizes from four sounds pushed through the
// analyser: silence, a sine, white noise and a decoded FLAC made by ffmpeg.
//
// OMAMP_VIS_BENCH=1 also measures each style's CPU on a 1,600 x 60 strip,
// playing and idle, and prints a table. OMAMP_VIS_SHOTS=<folder> saves a
// picture of each style on that strip.
#include <QAudioBuffer>
#include <QAudioFormat>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <ctime>

#include "appearance.h"
#include "decoder.h"
#include "librarycontroller.h"
#include "nowplaying_context.h"
#include "player.h"
#include "spectrum.h"
#include "theme.h"
#include "translator.h"
#include "visualiser.h"

namespace {

// Warnings caught while drawing: any at all fails the test.
QStringList g_warnings;
QtMessageHandler g_previous = nullptr;
void catchWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg)
        g_warnings << message;
    if (g_previous)
        g_previous(type, context, message);
}

constexpr int kRate = 48000;
constexpr int kChunk = kRate / 30; // one analyser update's worth

// A sound as interleaved float stereo at kRate.
struct Sound {
    QString name;
    QList<float> samples;
    int frames() const { return int(samples.size() / 2); }
};

Sound synth(const QString &name, int frames) {
    Sound s{name, QList<float>(frames * 2, 0.f)};
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        if (name == "sine") {
            l = 0.5f * std::sin(2 * M_PI * 440 * i / kRate);
            r = 0.5f * std::sin(2 * M_PI * 660 * i / kRate);
        } else if (name == "noise") {
            l = float(rng->generateDouble() * 2 - 1) * 0.5f;
            r = float(rng->generateDouble() * 2 - 1) * 0.5f;
        }
        s.samples[2 * i] = l;
        s.samples[2 * i + 1] = r;
    }
    return s;
}

// Something like music: a bass line with a kick twice a second, a chord,
// and a wandering lead, different on each side. Made by ffmpeg as FLAC and
// decoded by Omamp's own Decoder, as a real track would be.
Sound decodedFixture(const QString &dir, QString *error) {
    const QString flac = dir + "/fixture.flac";
    QProcess ff;
    const QString left = "0.35*exp(-mod(t\\,0.5)*14)*sin(2*PI*55*t)+0.18*sin(2*PI*110*t)*(0.6+0.4*sin(2*PI*0.5*t))"
                         "+0.08*sin(2*PI*330*t)+0.06*sin(2*PI*440*t)+0.05*sin(2*PI*(880+220*sin(2*PI*0.7*t))*t)"
                         "+0.02*sin(2*PI*5200*t)*exp(-mod(t+0.25\\,0.5)*30)+0.04*(random(0)*2-1)";
    const QString right = "0.35*exp(-mod(t\\,0.5)*14)*sin(2*PI*55*t)+0.18*sin(2*PI*110*t)*(0.6+0.4*sin(2*PI*0.5*t))"
                          "+0.08*sin(2*PI*392*t)+0.06*sin(2*PI*523*t)+0.05*sin(2*PI*(660+180*sin(2*PI*0.9*t))*t)"
                          "+0.02*sin(2*PI*7000*t)*exp(-mod(t+0.25\\,0.5)*30)+0.04*(random(1)*2-1)";
    ff.start("ffmpeg", {"-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i",
                        QString("aevalsrc=%1|%2:s=%3:d=4").arg(left, right).arg(kRate), "-c:a", "flac", flac});
    if (!ff.waitForFinished(30000) || ff.exitCode() != 0) {
        *error = "ffmpeg: " + QString::fromUtf8(ff.readAllStandardError());
        return {};
    }
    Decoder decoder;
    if (!decoder.open(flac, error))
        return {};
    Sound s{"fixture", {}};
    QList<float> chunk(4096 * 2);
    for (;;) {
        const int got = decoder.read(chunk.data(), 4096);
        if (got <= 0)
            break;
        s.samples.append(chunk.mid(0, got * 2));
    }
    if (decoder.sampleRate() != kRate)
        *error = QString("decoded at %1 Hz").arg(decoder.sampleRate());
    return s;
}

// Feeds the analyser the next chunk of a sound and passes what it hears to
// the visualiser, as the player does about 30 times a second.
void feed(Spectrum &spectrum, Visualiser *vis, const Sound &sound, int &at) {
    QAudioFormat format;
    format.setSampleRate(kRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    if (sound.frames() >= kChunk) {
        if (at + kChunk > sound.frames())
            at = 0;
        const QByteArray bytes(reinterpret_cast<const char *>(sound.samples.constData() + at * 2),
                               kChunk * 2 * sizeof(float));
        spectrum.feed(QAudioBuffer(bytes, format));
        at += kChunk;
    }
    spectrum.decay();
    vis->setLevels(spectrum.levels());
    vis->setPeaks(spectrum.peaks());
    vis->setWaveform(spectrum.waveform());
    vis->setStereoWaveform(spectrum.stereoWaveform());
}

int colours(const QImage &shot) {
    QSet<QRgb> seen;
    for (int y = 0; y < shot.height(); y += 2)
        for (int x = 0; x < shot.width(); x += 3)
            seen.insert(shot.pixel(x, y));
    return seen.size();
}

double processCpuSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Night Ride-ish theme colours, for the pictures and the measurement.
void nightColours(Visualiser *vis) {
    vis->setLow(QColor("#5fd7a7"));
    vis->setMid(QColor("#5ec4ff"));
    vis->setHigh(QColor("#ff6ac1"));
    vis->setPeak(QColor("#e8e8ff"));
}

} // namespace

class VisualiserTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QList<Sound> m_sounds;
    QByteArray m_oldConfig, m_oldData, m_oldCache, m_oldState;

    struct Window {
        QTemporaryDir &dir;
        Translator translator;
        Theme theme;
        Player player;
        LibraryController library;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;

        explicit Window(QTemporaryDir &d)
            : dir(d), translator(d.filePath("language")), theme(d.filePath("theme"), nullptr, d.filePath("no-hyprctl")),
              library(d.filePath("library.db")) {
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
        }
        bool load() {
            engine.load(QUrl("qrc:/Main.qml"));
            if (engine.rootObjects().size() != 1)
                return false;
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            return window && QTest::qWaitForWindowExposed(window);
        }
        static void findAll(QQuickItem *item, const QString &name, QList<QQuickItem *> *out) {
            if (item->objectName() == name)
                out->append(item);
            for (QQuickItem *child : item->childItems())
                findAll(child, name, out);
        }
        QQuickItem *find(const QString &name) const {
            QList<QQuickItem *> out;
            findAll(window->contentItem(), name, &out);
            return out.isEmpty() ? nullptr : out.first();
        }
        bool shown(const QString &name) const {
            QQuickItem *i = find(name);
            return i && i->isVisible() && i->width() > 0 && i->height() > 0;
        }
        Appearance *appearance() const {
            return qobject_cast<Appearance *>(engine.rootContext()->contextProperty("appearance").value<QObject *>());
        }
    };

    void setEnv(const char *name, const QString &value) { qputenv(name, value.toUtf8()); }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        // A throwaway XDG home: nothing here reads or writes the real one.
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        m_oldData = qgetenv("XDG_DATA_HOME");
        m_oldCache = qgetenv("XDG_CACHE_HOME");
        m_oldState = qgetenv("XDG_STATE_HOME");
        setEnv("XDG_CONFIG_HOME", m_dir.filePath("config"));
        setEnv("XDG_DATA_HOME", m_dir.filePath("data"));
        setEnv("XDG_CACHE_HOME", m_dir.filePath("cache"));
        setEnv("XDG_STATE_HOME", m_dir.filePath("state"));
        m_sounds << synth("silence", kRate) << synth("sine", kRate) << synth("noise", kRate);
        QString error;
        const Sound fixture = decodedFixture(m_dir.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(fixture.frames() > kRate * 3);
        m_sounds << fixture;
    }
    void cleanupTestCase() {
        auto restore = [](const char *name, const QByteArray &old) {
            if (old.isEmpty())
                qunsetenv(name);
            else
                qputenv(name, old);
        };
        restore("XDG_CONFIG_HOME", m_oldConfig);
        restore("XDG_DATA_HOME", m_oldData);
        restore("XDG_CACHE_HOME", m_oldCache);
        restore("XDG_STATE_HOME", m_oldState);
    }

    // The analyser keeps both channels apart for the ribbon.
    void stereoWaveformKeepsTheChannels() {
        Spectrum spectrum;
        auto *vis = new Visualiser;
        int at = 0;
        feed(spectrum, vis, m_sounds[1], at); // 440 Hz left, 660 Hz right
        const QList<qreal> both = spectrum.stereoWaveform();
        QCOMPARE(both.size(), 2 * Spectrum::wavePoints);
        qreal left = 0, right = 0, differ = 0;
        for (int i = 0; i < Spectrum::wavePoints; ++i) {
            left = qMax(left, qAbs(both[i]));
            right = qMax(right, qAbs(both[Spectrum::wavePoints + i]));
            differ = qMax(differ, qAbs(both[i] - both[Spectrum::wavePoints + i]));
        }
        QVERIFY(left > 0.3 && right > 0.3);
        QVERIFY(differ > 0.1);
        delete vis;
    }

    // Every style, at four sizes, from silence, a sine, noise and the
    // decoded fixture: it paints, it draws something when there is sound,
    // and nothing warns.
    void everyStyleRendersAtEverySize() {
        QQuickWindow window;
        window.resize(1600, 200);
        auto *vis = new Visualiser(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        g_warnings.clear();
        g_previous = qInstallMessageHandler(catchWarnings);
        const QList<QSizeF> sizes = {{1600, 60}, {400, 120}, {140, 36}, {1200, 200}};
        for (const QString &style : Appearance::visualisers()) {
            vis->setStyle(style);
            for (const QSizeF &size : sizes) {
                vis->setSize(size);
                for (const Sound &sound : std::as_const(m_sounds)) {
                    Spectrum spectrum;
                    int at = 0;
                    const int before = vis->frames();
                    for (int i = 0; i < 24; ++i) {
                        feed(spectrum, vis, sound, at);
                        if (vis->animated()) {
                            vis->advance(1.0 / 60);
                            vis->advance(1.0 / 60);
                        }
                    }
                    const QImage shot = window.grabWindow().copy(QRect(QPoint(0, 0), size.toSize()));
                    const QString what = QString("%1 %2x%3 %4").arg(style).arg(size.width()).arg(size.height()).arg(sound.name);
                    QVERIFY2(vis->frames() > before, qPrintable(what + ": nothing painted"));
                    if (sound.name != "silence")
                        QVERIFY2(colours(shot) > 2, qPrintable(what + ": one colour"));
                }
            }
        }
        qInstallMessageHandler(g_previous);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join("\n")));
        // A strip too small to draw in is left alone.
        vis->setSize(QSizeF(1, 1));
        vis->update();
        QTest::qWait(30);
    }

    // Idle (paused, stopped, Spotify): the animated styles keep a slow
    // clock and still draw; with reduced motion the clock stops on a still
    // frame. The old styles never run a clock.
    void idleAndReducedMotion() {
        QQuickWindow window;
        window.resize(800, 60);
        auto *vis = new Visualiser(window.contentItem());
        vis->setSize(QSizeF(800, 60));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        vis->setStyle("peaks");
        QVERIFY(!vis->ticking());
        vis->setStyle("skyline");
        QTRY_VERIFY(vis->ticking());
        vis->setIdle(true);
        QVERIFY(vis->ticking());
        int before = vis->frames();
        QTRY_VERIFY(vis->frames() > before + 3); // the idle animation runs
        vis->setReduceMotion(true);
        QVERIFY(!vis->ticking());
        before = vis->frames();
        QTest::qWait(200);
        QVERIFY(vis->frames() <= before + 1); // one still frame at most
        vis->setIdle(false);
        QVERIFY(vis->ticking());
        // Hidden, nothing ticks.
        vis->setVisible(false);
        QVERIFY(!vis->ticking());
        vis->setVisible(true);
        QVERIFY(vis->ticking());
    }

    // The analyser's QML follows the player: idle while paused, stopped, or
    // while a Spotify track plays elsewhere.
    void analyserIsIdleDuringSpotify() {
        QQmlEngine engine;
        Appearance appearance;
        QQmlComponent fake(&engine);
        fake.setData("import QtQuick\nQtObject { property string state: 'playing'; property bool playsElsewhere: false;"
                     " property var levels: []; property var peaks: []; property var waveform: [];"
                     " property var stereoWaveform: [] }",
                     QUrl());
        std::unique_ptr<QObject> backend(fake.create());
        QVERIFY2(backend, qPrintable(fake.errorString()));
        engine.rootContext()->setContextProperty("backend", backend.get());
        engine.rootContext()->setContextProperty("appearance", &appearance);
        QQmlComponent analyser(&engine, QUrl("qrc:/Analyser.qml"));
        std::unique_ptr<QObject> vis(analyser.create());
        QVERIFY2(vis, qPrintable(analyser.errorString()));
        QCOMPARE(vis->property("idle").toBool(), false);
        backend->setProperty("playsElsewhere", true);
        QCOMPARE(vis->property("idle").toBool(), true);
        backend->setProperty("playsElsewhere", false);
        backend->setProperty("state", "paused");
        QCOMPARE(vis->property("idle").toBool(), true);
        backend->setProperty("state", "playing");
        appearance.setReduceMotion(true);
        QCOMPARE(vis->property("reduceMotion").toBool(), true);
        // The bar's own: its style, and hidden while idle when asked.
        QQmlComponent barComponent(&engine, QUrl("qrc:/BarVisualiser.qml"));
        std::unique_ptr<QObject> bar(barComponent.create());
        QVERIFY2(bar, qPrintable(barComponent.errorString()));
        // In a window, so its visibility means something.
        QQuickWindow window;
        qobject_cast<QQuickItem *>(bar.get())->setParentItem(window.contentItem());
        appearance.setBarVisualiserStyle("synthwave");
        QCOMPARE(bar->property("style").toString(), QString("synthwave"));
        QVERIFY(bar->property("visible").toBool());
        appearance.setIdleVisualiser("hide");
        backend->setProperty("playsElsewhere", true);
        QVERIFY(!bar->property("visible").toBool());
        appearance.setIdleVisualiser("calm");
        QVERIFY(bar->property("visible").toBool());
    }

    // The large bar's settings persist; bad values are refused; the bar's
    // style follows the player's until it has its own.
    void barSettingsPersist() {
        QTemporaryDir dir;
        const QString conf = dir.filePath("window.conf");
        {
            Appearance a(conf);
            QCOMPARE(a.barVisualiser(), QString("behind"));
            QCOMPARE(a.barVisualiserStyle(), QString());
            QCOMPARE(a.idleVisualiser(), QString("calm"));
            QVERIFY(!a.reduceMotion());
            a.setVisualiser("ribbon");
            QCOMPARE(a.barStyle(), QString("ribbon"));
            // Shared: the bar's next is everyone's next.
            QCOMPARE(a.nextBarVisualiser(), QString("line"));
            QCOMPARE(a.visualiser(), QString("line"));
            a.setBarVisualiser("edge");
            a.setBarVisualiser("sideways"); // refused
            a.setBarVisualiserStyle("led");
            a.setBarVisualiserStyle("lava"); // refused
            a.setIdleVisualiser("hide");
            a.setIdleVisualiser("dance"); // refused
            a.setReduceMotion(true);
            QCOMPARE(a.barStyle(), QString("led"));
            // Its own: only the bar's moves on.
            QCOMPARE(a.cycleVisualiser(true), QString("synthwave"));
            QCOMPARE(a.visualiser(), QString("line"));
            QCOMPARE(a.cycleVisualiser(false), QString("pulse"));
            QCOMPARE(a.barStyle(), QString("synthwave"));
            // Round the end of the list.
            a.setBarVisualiserStyle("stars");
            QCOMPARE(a.nextBarVisualiser(), QString("bars"));
        }
        Appearance again(conf);
        QCOMPARE(again.barVisualiser(), QString("edge"));
        QCOMPARE(again.barVisualiserStyle(), QString("bars"));
        QCOMPARE(again.idleVisualiser(), QString("hide"));
        QVERIFY(again.reduceMotion());
        QCOMPARE(again.visualiser(), QString("pulse"));
        // Off: Shift+V in the library moves the shared style on.
        again.setBarVisualiser("off");
        QCOMPARE(again.cycleVisualiser(true), QString("led"));
        QCOMPARE(again.barVisualiserStyle(), QString("bars"));
        // Back to sharing.
        again.setBarVisualiserStyle("");
        QCOMPARE(QSettings(conf, QSettings::IniFormat).value("appearance/barVisualiserStyle").toString(), QString());
        QCOMPARE(Appearance(conf).barStyle(), QString("led"));
    }

    // In the window: the large bar shows its visualiser in each place, a
    // click and Shift+V move its style on, and Settings has the choices.
    void largeBarInTheWindow() {
        QTemporaryDir dir;
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", true);
            conf.setValue("window/libraryHintDone", true);
            conf.setValue("window/advancedWidth", 1400);
            conf.setValue("window/advancedHeight", 800);
            conf.setValue("layout/barSize", "large");
            conf.setValue("layout/barPosition", "bottom");
        }
        Window w(dir);
        QVERIFY(w.load());
        Appearance *appearance = w.appearance();
        QVERIFY(appearance);
        QTRY_VERIFY(w.find("bigBar"));
        // Behind the seek bar, by default; stopped, the calm idle animation.
        QTRY_VERIFY(w.shown("barVisualiserBehind"));
        QVERIFY(!w.shown("barVisualiserEdge"));
        QVERIFY(!w.shown("barVisualiserPanel"));
        auto *behind = qobject_cast<Visualiser *>(w.find("barVisualiserBehind"));
        QVERIFY(behind->idle());
        QCOMPARE(behind->style(), appearance->visualiser());
        // It spans the middle of the bar, not the cover or the volume.
        auto *bar = w.find("bigBar");
        const QPointF at = behind->mapToItem(bar, QPointF(0, 0));
        QVERIFY(at.x() > bar->width() * 0.2);
        QVERIFY(at.x() + behind->width() < bar->width() * 0.85);

        // Shift+V with a shared style: everyone's style moves on.
        appearance->setVisualiser("skyline");
        QTest::keyClick(w.window, Qt::Key_V, Qt::ShiftModifier);
        QTRY_COMPARE(appearance->visualiser(), QString("ribbon"));
        QCOMPARE(behind->style(), QString("ribbon"));
        // With its own style, only the bar's.
        appearance->setBarVisualiserStyle("led");
        QCOMPARE(behind->style(), QString("led"));
        QTest::keyClick(w.window, Qt::Key_V, Qt::ShiftModifier);
        QTRY_COMPARE(appearance->barVisualiserStyle(), QString("synthwave"));
        QCOMPARE(appearance->visualiser(), QString("ribbon"));
        // A click on it does the same: its top left corner is clear of the
        // buttons and the seek bar.
        QTest::mouseClick(w.window, Qt::LeftButton, {}, behind->mapToScene(QPointF(6, 4)).toPoint());
        QTRY_COMPARE(appearance->barVisualiserStyle(), QString("stars"));
        QCOMPARE(appearance->visualiser(), QString("ribbon"));

        // Along the edge: at the top of a bar at the bottom, and the bar
        // grows to make room.
        const qreal plain = bar->height();
        appearance->setBarVisualiser("edge");
        QTRY_VERIFY(w.shown("barVisualiserEdge"));
        QVERIFY(!w.shown("barVisualiserBehind"));
        QTRY_VERIFY(bar->height() > plain);
        QVERIFY(w.find("barVisualiserEdge")->y() < 8);
        appearance->layout()->setBarPosition("top");
        QTRY_VERIFY(w.find("bigBar") && w.find("bigBar") != bar);
        bar = w.find("bigBar");
        QTRY_VERIFY(w.shown("barVisualiserEdge"));
        QTRY_VERIFY(w.find("barVisualiserEdge")->y() > bar->height() / 2);

        // Beside the cover: a panel of its own.
        appearance->setBarVisualiser("beside");
        QTRY_VERIFY(w.shown("barVisualiserPanel"));
        QVERIFY(w.shown("barVisualiserBeside"));
        QVERIFY(!w.shown("barVisualiserEdge"));
        const QPointF panel = w.find("barVisualiserPanel")->mapToItem(bar, QPointF(0, 0));
        QVERIFY(panel.x() < bar->width() / 2);

        // Hidden while idle when asked; off.
        appearance->setIdleVisualiser("hide");
        QTRY_VERIFY(!w.shown("barVisualiserPanel"));
        appearance->setIdleVisualiser("calm");
        QTRY_VERIFY(w.shown("barVisualiserPanel"));
        appearance->setBarVisualiser("off");
        QTRY_VERIFY(!w.shown("barVisualiserPanel"));
        QVERIFY(!w.shown("barVisualiserBehind"));
        QVERIFY(!w.shown("barVisualiserEdge"));
        // Off: Shift+V moves the shared style on.
        QTest::keyClick(w.window, Qt::Key_V, Qt::ShiftModifier);
        QTRY_COMPARE(appearance->visualiser(), QString("line"));

        // The palette's actions.
        QMetaObject::invokeMethod(w.window, "runAction", Q_ARG(QVariant, QVariant("bar-visualiser:behind")));
        QCOMPARE(appearance->barVisualiser(), QString("behind"));
        QMetaObject::invokeMethod(w.window, "runAction", Q_ARG(QVariant, QVariant("reduce-motion")));
        QVERIFY(appearance->reduceMotion());
        QTRY_VERIFY(qobject_cast<Visualiser *>(w.find("barVisualiserBehind"))->reduceMotion());

        // Settings > Appearance has the choices, and they apply.
        QMetaObject::invokeMethod(w.window, "runAction", Q_ARG(QVariant, QVariant("settings-appearance")));
        QTRY_VERIFY(w.window->findChild<QObject *>("barVisualiserSettings"));
        QObject *edge = w.find("barVisualiser_edge");
        QVERIFY(edge);
        QMetaObject::invokeMethod(edge, "clicked");
        QCOMPARE(appearance->barVisualiser(), QString("edge"));
        QObject *hide = w.find("idleVisualiserHide");
        QVERIFY(hide);
        QMetaObject::invokeMethod(hide, "clicked");
        QCOMPARE(appearance->idleVisualiser(), QString("hide"));
        QTRY_COMPARE(QSettings(dir.filePath("window.conf"), QSettings::IniFormat).value("appearance/barVisualiser").toString(),
                     QString("edge"));
    }

    // OMAMP_VIS_BENCH=1: each style's CPU on a 1,600 x 60 strip, playing
    // the fixture and idle. OMAMP_VIS_SHOTS=<folder>: a picture of each.
    void cpuOnAWideStrip() {
        const bool bench = qEnvironmentVariableIntValue("OMAMP_VIS_BENCH") > 0;
        const QString shots = qEnvironmentVariable("OMAMP_VIS_SHOTS");
        if (!bench && shots.isEmpty())
            QSKIP("OMAMP_VIS_BENCH=1 measures the CPU; OMAMP_VIS_SHOTS=<folder> saves pictures");
        QQuickWindow window;
        window.setColor(QColor("#1b1e2b"));
        window.resize(1600, 60);
        auto *vis = new Visualiser(window.contentItem());
        vis->setSize(QSizeF(1600, 60));
        nightColours(vis);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const Sound &music = m_sounds.last();
        const int seconds = qMax(1, qEnvironmentVariableIntValue("OMAMP_VIS_BENCH_SECONDS") > 0
                                        ? qEnvironmentVariableIntValue("OMAMP_VIS_BENCH_SECONDS") : 3);
        QTextStream out(stdout);
        if (bench)
            out << "\nstyle        playing: CPU %  fps  paint ms   idle: CPU %  fps\n";
        for (const QString &style : Appearance::visualisers()) {
            vis->setStyle(style);
            QString line = style.leftJustified(12);
            for (bool idle : {false, true}) {
                if (!bench && idle)
                    continue;
                vis->setIdle(idle);
                Spectrum spectrum;
                int at = 0;
                QTimer data;
                data.setInterval(33);
                QObject::connect(&data, &QTimer::timeout, vis, [&] {
                    if (idle) {
                        spectrum.decay();
                        vis->setLevels(spectrum.levels());
                    } else {
                        feed(spectrum, vis, music, at);
                    }
                });
                data.start();
                QTest::qWait(400); // settle
                vis->resetPaintStats();
                const int frames = vis->frames();
                const double cpu = processCpuSeconds();
                QElapsedTimer wall;
                wall.start();
                QTest::qWait(bench ? seconds * 1000 : 900);
                const double elapsed = wall.nsecsElapsed() / 1e9;
                const double used = processCpuSeconds() - cpu;
                const int painted = vis->frames() - frames;
                data.stop();
                if (!idle && !shots.isEmpty()) {
                    QDir().mkpath(shots);
                    window.grabWindow().save(shots + "/" + style + ".png");
                }
                if (bench) {
                    line += QString("  %1 %2").arg(100 * used / elapsed, 8, 'f', 1).arg(painted / elapsed, 5, 'f', 0);
                    if (!idle)
                        line += QString(" %1  ").arg(painted ? vis->paintNanos() / 1e6 / painted : 0.0, 8, 'f', 2);
                }
            }
            if (bench)
                out << line << "\n";
        }
        out.flush();
    }
};

int runVisualiserTests(int argc, char **argv) {
    VisualiserTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "visualiser_tests.moc"
