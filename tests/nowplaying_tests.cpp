// Now playing: the playing track's cover for library files, loose files and
// server tracks; mpris:artUrl; the deck styles and the visualisers; and, in
// the real window, the big view, the mini and micro players and the style
// switch.
//
// Everything runs offscreen with the software renderer, in temporary
// folders (XDG_CACHE_HOME and XDG_CONFIG_HOME included), with no sound
// device and nothing on the session bus.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QNetworkProxy>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <functional>
#include <memory>

#include "appearance.h"
#include "artprovider.h"
#include "library.h"
#include "librarycontroller.h"
#include "mocksubsonic.h"
#include "mpris.h"
#include "nowplaying.h"
#include "nowplaying_context.h"
#include "player.h"
#include "playrecorder.h"
#include "remote.h"
#include "scanner.h"
#include "theme.h"
#include "translator.h"
#include "visualiser.h"
#include "volumes.h"

namespace {

bool haveFfmpeg() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }

bool run(const QStringList &args) {
    QProcess p;
    p.start("ffmpeg", QStringList{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"} + args);
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

// A quiet tagged tone; nothing is played.
bool tone(const QString &file, double seconds, const QString &title, const QString &album, int number) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    return run({"-f", "lavfi", "-i", QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds), "-af",
                "volume=0.05", "-map_metadata", "-1", "-metadata", "title=" + title, "-metadata", "artist=Neon Coast",
                "-metadata", "album=" + album, "-metadata", QString("track=%1").arg(number), file});
}

bool picture(const QString &file, const QColor &colour, int size = 300) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(colour);
    return image.save(file);
}

// An MP3 with the picture in its tags (an ID3 APIC frame).
bool embedded(const QString &file, const QColor &colour) {
    const QString raw = file + ".raw.mp3", pic = file + ".pic.png";
    if (!picture(pic, colour) || !tone(raw, 1, "Embedded", "Loose", 1))
        return false;
    const bool ok = run({"-i", raw, "-i", pic, "-map", "0", "-map", "1", "-c", "copy", "-id3v2_version", "3",
                         "-disposition:v", "attached_pic", file});
    QFile::remove(raw);
    QFile::remove(pic);
    return ok;
}

// Close enough: JPEG and scaling move colours a little.
bool near(const QColor &a, const QColor &b) {
    return qAbs(a.red() - b.red()) < 40 && qAbs(a.green() - b.green()) < 40 && qAbs(a.blue() - b.blue()) < 40;
}

QColor centreOf(const QString &file) {
    const QImage image(file);
    return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
}

QObject *findItem(QQuickItem *item, const QString &name) {
    if (item->objectName() == name)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QObject *o = findItem(child, name))
            return o;
    return nullptr;
}

// QML warnings from Omamp's own files, collected while a window runs.
QStringList *qmlWarnings = nullptr;
QtMessageHandler previousHandler = nullptr;
void collect(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (qmlWarnings && type == QtWarningMsg && message.contains("qrc:/"))
        qmlWarnings->append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

} // namespace

class NowPlayingTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_xdg;
    QByteArray m_oldCache, m_oldConfig;

    // Asks for a picture and waits for the answer.
    NowPlaying::Art lookUp(NowPlaying &now, const QString &path, qint64 trackId = 0) {
        QSignalSpy done(&now, &NowPlaying::loadingChanged);
        now.lookUp(path, trackId);
        if (!QTest::qWaitFor([&] { return !now.loading(); }, 20000))
            return {};
        NowPlaying::Art art;
        art.large = now.artPath();
        art.source = now.artSource();
        art.trackId = now.trackId();
        art.blur = QUrl(now.blurUrl()).toLocalFile();
        art.small = QUrl(now.thumbUrl()).toLocalFile();
        art.medium = QUrl(now.coverUrl()).toLocalFile();
        return art;
    }

private slots:
    void initTestCase() {
        m_oldCache = qgetenv("XDG_CACHE_HOME");
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_xdg.filePath("cache")));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_xdg.filePath("config")));
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }

    void cleanupTestCase() {
        for (const auto &[name, old] : {std::pair{"XDG_CACHE_HOME", m_oldCache}, std::pair{"XDG_CONFIG_HOME", m_oldConfig}}) {
            if (old.isEmpty())
                qunsetenv(name);
            else
                qputenv(name, old);
        }
        RemoteSources::clearSecretCache();
    }

    void deckStylesHaveEveryColourAndPersist() {
        QTemporaryDir dir;
        const QString conf = dir.filePath("window.conf");
        Theme theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl"));
        {
            Appearance a(conf, &theme);
            QCOMPARE(a.deckStyle(), QString("omarchy"));
            QCOMPARE(a.visualiser(), QString("peaks"));
            QVERIFY(a.coverInDeck());
            QCOMPARE(a.bigBackground(), QString("blur"));
            // The Omarchy style is the theme's own colours.
            QCOMPARE(a.deck().value("lcdText").toString(), theme.colors().value("green").toString());
            QCOMPARE(a.deck(), a.plain());
            // Every style names every colour the components read.
            const QStringList keys = a.plain().keys();
            for (const QString &style : Appearance::deckStyles()) {
                const QVariantMap p = Appearance::palette(style, theme.colors(), 0, false);
                for (const QString &key : keys)
                    QVERIFY2(p.contains(key), qPrintable(style + " lacks " + key));
            }
            QSignalSpy changed(&a, &Appearance::paletteChanged);
            a.setDeckStyle("classic");
            QCOMPARE(changed.count(), 1);
            QCOMPARE(a.deck().value("lcd").toString(), QString("#000000"));
            QCOMPARE(a.deck().value("lcdText").toString(), QString("#00e000"));
            // The library keeps the theme whatever the deck wears.
            QCOMPARE(a.plain().value("lcdText").toString(), theme.colors().value("green").toString());
            a.setDeckStyle("nonsense");
            QCOMPARE(a.deckStyle(), QString("classic"));
            a.setVisualiser("stars");
            QCOMPARE(a.nextVisualiser(), QString("bars"));
            a.setVisualiser("scope");
            a.setCoverInDeck(false);
            a.setBigBackground("theme");
            a.setBigLyrics(false);
        }
        Appearance again(conf, &theme);
        QCOMPARE(again.deckStyle(), QString("classic"));
        QCOMPARE(again.visualiser(), QString("scope"));
        QVERIFY(!again.coverInDeck());
        QCOMPARE(again.bigBackground(), QString("theme"));
        QVERIFY(!again.bigLyrics());
        QSettings raw(conf, QSettings::IniFormat);
        QCOMPARE(raw.value("appearance/deckStyle").toString(), QString("classic"));
    }

    void coversForLibraryAndLooseFiles() {
        if (!haveFfmpeg())
            QSKIP("ffmpeg is needed to make the test files");
        QTemporaryDir dir;
        const QString music = dir.filePath("music"), loose = dir.filePath("loose"), art = dir.filePath("art");
        // In the library: an album with a cover.jpg beside it.
        QVERIFY(tone(music + "/Night Drive/01 Highway.flac", 1, "Highway", "Night Drive", 1));
        QVERIFY(picture(music + "/Night Drive/cover.jpg", QColor(220, 30, 30)));
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            QVERIFY(Scanner(library, VolumeResolver()).scan({music}).ok);
        }
        // Outside it: a picture in the tags, a folder picture, a disc folder
        // with the picture one level up, and nothing at all.
        QVERIFY(embedded(loose + "/tagged/song.mp3", QColor(30, 30, 220)));
        QVERIFY(tone(loose + "/folder/song.flac", 1, "Folder", "Loose", 1));
        QVERIFY(picture(loose + "/folder/folder.png", QColor(30, 200, 30)));
        QVERIFY(tone(loose + "/Box/CD1/song.flac", 1, "Disc", "Box", 1));
        QVERIFY(picture(loose + "/Box/front.jpg", QColor(230, 200, 20)));
        QVERIFY(tone(loose + "/bare/song.flac", 1, "Bare", "Loose", 1));

        NowPlaying now(nullptr, dir.filePath("library.db"), art);

        NowPlaying::Art a = lookUp(now, music + "/Night Drive/01 Highway.flac");
        QCOMPARE(a.source, QString("library"));
        QVERIFY(a.trackId > 0);
        QVERIFY(a.large.endsWith("-600.jpg"));
        QVERIFY(QFileInfo::exists(a.medium) && a.medium.endsWith("-256.jpg"));
        QVERIFY(QFileInfo::exists(a.small) && a.small.endsWith("-96.jpg"));
        QVERIFY(near(centreOf(a.large), QColor(220, 30, 30)));
        // The blur: small, made once, the cover's colour.
        QVERIFY(QFileInfo::exists(a.blur));
        QCOMPARE(QImage(a.blur).size(), QSize(64, 64));
        QVERIFY(near(centreOf(a.blur), QColor(220, 30, 30)));
        QVERIFY(now.hasArt());
        QVERIFY(now.artUrl().startsWith("file://"));

        a = lookUp(now, loose + "/tagged/song.mp3");
        QCOMPARE(a.source, QString("embedded"));
        QCOMPARE(a.trackId, 0);
        QVERIFY(near(centreOf(a.large), QColor(30, 30, 220)));
        QVERIFY(a.large.startsWith(art + "/"));

        a = lookUp(now, loose + "/folder/song.flac");
        QCOMPARE(a.source, QString("folder"));
        QVERIFY(near(centreOf(a.large), QColor(30, 200, 30)));

        a = lookUp(now, loose + "/Box/CD1/song.flac");
        QCOMPARE(a.source, QString("folder"));
        QVERIFY(near(centreOf(a.medium), QColor(230, 200, 20)));

        QSignalSpy cleared(&now, &NowPlaying::artChanged);
        a = lookUp(now, loose + "/bare/song.flac");
        QVERIFY(a.large.isEmpty());
        QVERIFY(!now.hasArt());
        QVERIFY(now.artUrl().isEmpty());
        QCOMPARE(cleared.count(), 1);

        // Without a library file nothing is looked up there, and none is made.
        QTemporaryDir empty;
        NowPlaying alone(nullptr, empty.filePath("library.db"), art);
        a = lookUp(alone, loose + "/tagged/song.mp3");
        QCOMPARE(a.source, QString("embedded"));
        QVERIFY(!QFileInfo::exists(empty.filePath("library.db")));
    }

    void coversForServerTracks() {
        MockSubsonic server;
        server.realPictures = true;
        QTemporaryDir dir;
        qint64 highway = 0;
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            QString error;
            const qint64 id = RemoteSources::add(library, server.url(), "listener", "sesame", "token", "Home NAS", &error);
            QVERIFY2(id, qPrintable(error));
            QVERIFY(RemoteSources::sync(library, id).ok);
            QSqlQuery q(library.db());
            QVERIFY(q.exec("SELECT id FROM tracks WHERE rel_path = 's-1'") && q.next());
            highway = q.value(0).toLongLong();
        }
        QVERIFY(highway > 0);
        NowPlaying now(nullptr, dir.filePath("library.db"), dir.filePath("art"));
        const NowPlaying::Art a = lookUp(now, "subsonic://1/s-1", highway);
        QCOMPARE(a.source, QString("remote"));
        QCOMPARE(a.trackId, highway);
        QVERIFY(QFileInfo::exists(a.large));
        QVERIFY(QFileInfo::exists(a.blur));
        QCOMPARE(server.count("getCoverArt"), 1);
        // A server that has gone away: no picture, no hang.
        server.refuse = true;
        RemoteSources::clearSecretCache();
        const NowPlaying::Art gone = lookUp(now, "subsonic://1/s-2", highway + 1);
        QVERIFY(gone.large.isEmpty() || QFileInfo::exists(gone.large));
    }

    void mprisCarriesTheCover() {
        if (!haveFfmpeg())
            QSKIP("ffmpeg is needed to make the test files");
        QTemporaryDir dir;
        QVERIFY(embedded(dir.filePath("song.mp3"), QColor(200, 40, 160)));
        Player player;
        player.setMuted(true);
        NowPlaying now(&player, dir.filePath("library.db"), dir.filePath("art"));
        QObject host;
        // Never registered: nothing goes to the session bus.
        MprisPlayer mpris(&host, &player, &now);
        QVERIFY(!mpris.metadata().contains("mpris:artUrl"));
        QCOMPARE(player.addPaths({dir.filePath("song.mp3")}), 1);
        player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return now.hasArt() && !now.loading(); }, 20000));
        const QVariantMap meta = mpris.metadata();
        QVERIFY(meta.contains("mpris:artUrl"));
        const QString url = meta.value("mpris:artUrl").toString();
        QVERIFY(url.startsWith("file://"));
        QVERIFY(url.endsWith("-600.jpg"));
        QVERIFY(QFileInfo::exists(QUrl(url).toLocalFile()));
        player.stop();
        player.clear();
        QVERIFY(QTest::qWaitFor([&] { return !now.hasArt(); }, 5000));
        QVERIFY(!mpris.metadata().contains("mpris:artUrl"));
    }

    void visualiserStylesPaint() {
        QQuickWindow window;
        window.resize(400, 120);
        auto *vis = new Visualiser(window.contentItem());
        vis->setSize(QSizeF(400, 120));
        QList<qreal> levels, wave;
        for (int i = 0; i < 24; ++i)
            levels << (i % 5) / 4.0;
        for (int i = 0; i < 128; ++i)
            wave << std::sin(i / 6.0) * 0.3;
        vis->setPeaks(levels);
        vis->setWaveform(wave);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (const QString &style : Appearance::visualisers()) {
            vis->setStyle(style);
            const int before = vis->frames();
            for (int i = 0; i < 6; ++i) {
                vis->setLevels(levels);
                vis->setWaveform(wave);
                QTest::qWait(20);
            }
            QVERIFY2(QTest::qWaitFor([&] { return vis->frames() > before; }, 3000), qPrintable(style));
            // Something was drawn: the window isn't all one colour.
            const QImage shot = window.grabWindow();
            QSet<QRgb> colours;
            for (int y = 0; y < shot.height(); y += 4)
                for (int x = 0; x < shot.width(); x += 4)
                    colours.insert(shot.pixel(x, y));
            QVERIFY2(colours.size() > 2, qPrintable(style));
        }
        // The bars go from the low colour at the bottom to the high one.
        QCOMPARE(vis->colourAt(0), vis->low());
        QCOMPARE(vis->colourAt(1), vis->high());
    }

    void bigViewMiniPlayersAndStylesInTheWindow() {
        if (!haveFfmpeg())
            QSKIP("ffmpeg is needed to make the test files");
        QTemporaryDir dir;
        const QString music = dir.filePath("music/Night Drive");
        for (int i = 1; i <= 4; ++i)
            QVERIFY(tone(QString("%1/%2 Track %2.flac").arg(music).arg(i), 30, QString("Track %1").arg(i), "Night Drive", i));
        QVERIFY(picture(music + "/cover.jpg", QColor(240, 40, 150)));
        // Timed lyrics beside the first track.
        {
            QFile lrc(music + "/1 Track 1.lrc");
            QVERIFY(lrc.open(QIODevice::WriteOnly));
            lrc.write("[00:00.00]First line\n[00:05.00]Second line\n[00:10.00]Third line\n");
        }
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            QVERIFY(Scanner(library, VolumeResolver()).scan({dir.filePath("music")}).ok);
        }
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", true);
            conf.setValue("window/advancedWidth", 1360);
            conf.setValue("window/advancedHeight", 820);
        }
        QStringList warnings;
        qmlWarnings = &warnings;
        previousHandler = qInstallMessageHandler(collect);

        Translator translator(dir.filePath("language"));
        Theme theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl"));
        Player player;
        player.setMuted(true);
        LibraryController library(dir.filePath("library.db"));
        library.setPlayer(&player);
        library.setWatching(false);
        library.setArtCacheDir(dir.filePath("art"));
        library.setWriteSandbox(dir.path());
        library.recorder()->setFlushing(false);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &player);
        engine.rootContext()->setContextProperty("library", &library);
        engine.rootContext()->setContextProperty("commandPalette", library.palette());
        engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(dir.filePath("window.conf")));
        installNowPlaying(engine, &theme, &player, dir.filePath("library.db"), dir.filePath("art"),
                          dir.filePath("window.conf"));
        engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider(dir.filePath("library.db"), dir.filePath("art")));
        engine.load(QUrl("qrc:/Main.qml"));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *appearance = engine.rootContext()->contextProperty("appearance").value<QObject *>();
        auto *now = engine.rootContext()->contextProperty("nowPlayingArt").value<NowPlaying *>();
        QVERIFY(appearance && now);
        // Sheets live in the overlay, beside the content item.
        auto find = [&](const char *name) {
            QQuickItem *top = window->contentItem()->parentItem() ? window->contentItem()->parentItem() : window->contentItem();
            return findItem(top, QString::fromLatin1(name));
        };

        // Play the album; the strip shows its cover.
        QVERIFY(library.ready());
        QCOMPARE(player.addPaths({music}), 4);
        player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return now->hasArt() && !now->loading(); }, 20000));
        QVERIFY(now->trackId() > 0);
        QObject *stripCover = find("stripCover");
        QVERIFY(stripCover);
        QVERIFY(QTest::qWaitFor([&] { return stripCover->property("showing").toBool(); }, 5000));

        // N opens the big view: the cover, the title, three tracks up next
        // and the timed lyrics; Esc closes it.
        QTest::keyClick(window, Qt::Key_N);
        QObject *big = nullptr;
        QVERIFY(QTest::qWaitFor([&] { return (big = find("bigView")) != nullptr; }, 3000));
        QVERIFY(big->property("visible").toBool());
        QObject *bigCover = find("bigCover");
        QVERIFY(QTest::qWaitFor([&] { return bigCover->property("showing").toBool(); }, 5000));
        QCOMPARE(find("bigTitle")->property("text").toString(), QString("Track 1"));
        QCOMPARE(find("bigUpNext")->property("count").toInt(), 3);
        QObject *lyrics = find("bigLyrics");
        QVERIFY(lyrics);
        QVERIFY(QTest::qWaitFor([&] { return lyrics->property("hasLines").toBool(); }, 5000));
        QVERIFY(lyrics->property("visible").toBool());
        QCOMPARE(find("bigLyricsLines")->property("count").toInt(), 3);
        // Lyrics off (Shift+L) and on again.
        QTest::keyClick(window, Qt::Key_L, Qt::ShiftModifier);
        QVERIFY(!appearance->property("bigLyrics").toBool());
        QVERIFY(!lyrics->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_L, Qt::ShiftModifier);
        QVERIFY(appearance->property("bigLyrics").toBool());
        // Shift+V moves the visualiser on; V is still stop.
        const QString vis = appearance->property("visualiser").toString();
        QTest::keyClick(window, Qt::Key_V, Qt::ShiftModifier);
        QVERIFY(appearance->property("visualiser").toString() != vis);
        QObject *bigVis = find("bigVisualiser");
        QVERIFY(bigVis);
        QCOMPARE(bigVis->property("style").toString(), appearance->property("visualiser").toString());
        // A star sets the rating.
        auto *stars = qobject_cast<QQuickItem *>(find("bigStars"));
        QVERIFY(stars);
        QList<QQuickItem *> starItems;
        for (QQuickItem *child : stars->childItems())
            if (!child->property("text").toString().isEmpty())
                starItems << child;
        QCOMPARE(starItems.size(), 5);
        QQuickItem *fourth = starItems.at(3);
        QSignalSpy jobs(&library, &LibraryController::jobFinished);
        QTest::mouseClick(window, Qt::LeftButton, {}, fourth->mapToScene(QPointF(fourth->width() / 2, fourth->height() / 2)).toPoint());
        QVERIFY(jobs.wait(10000));
        QCOMPARE(library.trackInfo(now->trackId()).value("rating").toInt(), 80);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(QTest::qWaitFor([&] { return find("bigView") == nullptr; }, 3000));

        // Ctrl+M: the mini player, 420 x 64, titled for a window rule.
        const QSize full = window->size();
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QVERIFY(QTest::qWaitFor([&] { return window->size() == QSize(420, 64); }, 3000));
        QCOMPARE(window->title(), QString("Omamp mini"));
        QVERIFY(find("miniPlayer"));
        QVERIFY(find("miniLayout")->property("visible").toBool());
        // Its size is its own: a resized mini player comes back that size.
        window->resize(460, 70);
        QVERIFY(QTest::qWaitFor([&] { return window->size() == QSize(460, 70); }, 3000));
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QVERIFY(QTest::qWaitFor([&] { return window->size() == full; }, 3000));
        QVERIFY(window->title() != "Omamp mini");
        QVERIFY(find("miniPlayer") == nullptr);
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QVERIFY(QTest::qWaitFor([&] { return window->size() == QSize(460, 70); }, 3000));
        // Ctrl+Shift+M from there: the micro player.
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(QTest::qWaitFor([&] { return window->size() == QSize(275, 28); }, 3000));
        QCOMPARE(window->title(), QString("Omamp micro"));
        QVERIFY(find("microLayout")->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(QTest::qWaitFor([&] { return window->size() == full; }, 3000));
        // QML's Settings writes the file a moment later.
        QVERIFY(QTest::qWaitFor([&] {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            return conf.value("compact/miniWidth").toInt() == 460 && conf.value("compact/miniHeight").toInt() == 70;
        }, 5000));

        // The deck style: Classic from the palette, on the deck, kept.
        QTest::keyClick(window, Qt::Key_Tab); // to the deck
        QVERIFY(QTest::qWaitFor([&] { return !window->property("advanced").toBool(); }, 3000));
        QObject *deck = find("deckView");
        QVERIFY(deck && deck->property("visible").toBool());
        QMetaObject::invokeMethod(window, "runAction", Q_ARG(QVariant, QString("deck-style:classic")));
        QCOMPARE(appearance->property("deckStyle").toString(), QString("classic"));
        QCOMPARE(deck->property("skin").toMap().value("style").toString(), QString("classic"));
        QCOMPARE(window->color(), QColor("#1e1e26"));
        QVERIFY(find("deckCover")->property("visible").toBool());
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            QCOMPARE(conf.value("appearance/deckStyle").toString(), QString("classic"));
        }
        // Every visualiser draws on the deck.
        QObject *analyser = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            if (item->objectName() == "analyser" && item->isVisible())
                analyser = item;
            for (QQuickItem *child : item->childItems())
                walk(child);
        };
        walk(window->contentItem());
        QVERIFY(analyser);
        for (const QString &style : Appearance::visualisers()) {
            appearance->setProperty("visualiser", style);
            QCOMPARE(analyser->property("style").toString(), style);
            const int before = analyser->property("frames").toInt();
            QVERIFY2(QTest::qWaitFor([&] { return analyser->property("frames").toInt() > before; }, 3000), qPrintable(style));
        }
        // The style picker on the Appearance page.
        QMetaObject::invokeMethod(window, "runAction", Q_ARG(QVariant, QString("settings-appearance")));
        QObject *silver = nullptr;
        QVERIFY(QTest::qWaitFor([&] { return (silver = find("deckStyle_silver")) != nullptr; }, 3000));
        QMetaObject::invokeMethod(silver, "clicked");
        QCOMPARE(appearance->property("deckStyle").toString(), QString("silver"));

        player.stop();
        qInstallMessageHandler(previousHandler);
        qmlWarnings = nullptr;
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }
};

int runNowPlayingTests(int argc, char **argv) {
    NowPlayingTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "nowplaying_tests.moc"
