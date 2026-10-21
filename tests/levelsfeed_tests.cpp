// The levels feed for the Omarchy bar plugin (THRUM_TESTS=levelsfeed).
//
// Each test opens its own socket in a temporary folder and reads it with a
// QLocalSocket, as Quickshell's Socket does. No audio, no window on screen,
// and nothing in the user's runtime folder or settings.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "levelsfeed.h"
#include "theme.h"

namespace {

// A client that collects the feed's lines as JSON objects.
struct Reader {
    QLocalSocket socket;
    QList<QJsonObject> frames;
    QByteArray buffer;

    explicit Reader(const QString &path) {
        QObject::connect(&socket, &QLocalSocket::readyRead, [this] {
            buffer += socket.readAll();
            int nl;
            while ((nl = buffer.indexOf('\n')) >= 0) {
                frames << QJsonDocument::fromJson(buffer.left(nl)).object();
                buffer.remove(0, nl + 1);
            }
        });
        socket.connectToServer(path);
    }
    QJsonObject last() const { return frames.isEmpty() ? QJsonObject() : frames.last(); }
};

// Levels that change every read, so no frame is skipped as a repeat.
struct Moving {
    int n = 0;
    QList<qreal> operator()() {
        ++n;
        QList<qreal> l;
        for (int i = 0; i < 24; ++i)
            l << ((n + i) % 10) / 10.0;
        return l;
    }
};

} // namespace

class LevelsFeedTests : public QObject {
    Q_OBJECT

private slots:
    void shrinkFoldsTheBandsToSixteenKeepingPeaks() {
        QList<qreal> l(24, 0.0);
        l[2] = 0.5;  // band 1 covers inputs 1 and 2
        l[23] = 2.0; // clamped
        const auto out = LevelsFeed::shrink(l);
        QCOMPARE(out.size(), 16);
        QCOMPARE(out[1], 50);
        QCOMPARE(out[0], 0);
        QCOMPARE(out[15], 100);
        QCOMPARE(out[7], 0);
        QVERIFY(LevelsFeed::shrink({}).isEmpty());
        QCOMPARE(LevelsFeed::shrink({0.25}, 4), (QList<int>{25, 25, 25, 25}));
    }

    void aFrameIsOneLineOfJson() {
        const QByteArray line = LevelsFeed::frame("synthwave", true, true, {1, 2, 3});
        QVERIFY(line.endsWith('\n'));
        QCOMPARE(line.count('\n'), 1);
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        QCOMPARE(o["style"].toString(), QString("synthwave"));
        QCOMPARE(o["reduceMotion"].toBool(), true);
        QCOMPARE(o["active"].toBool(), true);
        QCOMPARE(o["levels"].toArray().size(), 3);
    }

    // The work of one tick, without the socket: fold the bands and make the
    // line. Printed for docs/plan/progress.md.
    void aFrameCostsMicroseconds() {
        QList<qreal> l;
        for (int i = 0; i < 24; ++i)
            l << i / 24.0;
        QElapsedTimer t;
        t.start();
        const int n = 20000;
        qsizetype bytes = 0;
        for (int i = 0; i < n; ++i)
            bytes += LevelsFeed::frame("synthwave", false, true, LevelsFeed::shrink(l)).size();
        const double us = t.nsecsElapsed() / 1000.0 / n;
        qInfo("one frame: %.2f us, %lld bytes", us, (long long)(bytes / n));
        QVERIFY2(us < 100, qPrintable(QString::number(us)));
    }

    void nothingListensUntilStartedAndEnabled() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        QVERIFY(!feed.listening());
        feed.setEnabled(false);
        feed.start();
        QVERIFY(!feed.listening());
        QVERIFY(!QFile::exists(path));
        feed.setEnabled(true);
        QVERIFY(feed.listening());
        QVERIFY(QFile::exists(path));
        feed.setEnabled(false);
        QVERIFY(!feed.listening());
        QVERIFY(!QFile::exists(path));
    }

    void theSettingIsKeptInWindowConf() {
        QTemporaryDir dir;
        const QString conf = dir.filePath("window.conf");
        {
            LevelsFeed feed(conf, dir.filePath("a.sock"));
            QVERIFY(feed.enabled()); // on by default: it costs nothing until asked
            feed.setEnabled(false);
        }
        QCOMPARE(QSettings(conf, QSettings::IniFormat).value("appearance/shareWithBar").toBool(), false);
        LevelsFeed again(conf, dir.filePath("b.sock"));
        QVERIFY(!again.enabled());
    }

    void noClientsMeansNoClock() {
        QTemporaryDir dir;
        LevelsFeed feed({}, dir.filePath("levels.sock"));
        Moving source;
        feed.setLevelsSource(std::ref(source));
        feed.start();
        feed.setActive(true);
        QTest::qWait(200);
        QVERIFY(!feed.sending());
        QCOMPARE(feed.framesSent(), 0);
        QCOMPARE(source.n, 0); // the levels are not even read
    }

    void aClientGetsOneFrameAndNothingMoreWhileIdle() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        feed.setStyle("led");
        feed.start();
        Reader r(path);
        QTRY_COMPARE(feed.clients(), 1);
        QTRY_COMPARE(r.frames.size(), 1);
        QCOMPARE(r.last()["active"].toBool(), false);
        QCOMPARE(r.last()["style"].toString(), QString("led"));
        QVERIFY(r.last()["levels"].toArray().isEmpty());
        QVERIFY(!feed.sending());
        QTest::qWait(300);
        QCOMPARE(r.frames.size(), 1);
    }

    void playingSendsAtMostTwentyFramesASecond() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        Moving source;
        feed.setLevelsSource(std::ref(source));
        feed.start();
        Reader r(path);
        QTRY_COMPARE(r.frames.size(), 1);
        feed.setActive(true);
        QVERIFY(feed.sending());
        QCOMPARE(feed.interval(), LevelsFeed::intervalMs);
        QElapsedTimer t;
        t.start();
        QTest::qWait(1000);
        const int frames = r.frames.size() - 1;
        const qreal seconds = t.elapsed() / 1000.0;
        QVERIFY2(frames >= 10, qPrintable(QString::number(frames)));
        QVERIFY2(frames <= int(seconds * 20) + 2, qPrintable(QString("%1 in %2 s").arg(frames).arg(seconds)));
        QCOMPARE(r.last()["active"].toBool(), true);
        QCOMPARE(r.last()["levels"].toArray().size(), LevelsFeed::bands);
    }

    void identicalFramesAreSkipped() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        feed.setLevelsSource([] { return QList<qreal>(24, 0.0); }); // silence
        feed.start();
        Reader r(path);
        QTRY_COMPARE(r.frames.size(), 1);
        feed.setActive(true);
        QTRY_COMPARE(r.frames.size(), 2);
        QTest::qWait(300);
        QCOMPARE(r.frames.size(), 2);
    }

    void styleAndReduceMotionChangesArriveAtOnce() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        feed.start();
        Reader r(path);
        QTRY_COMPARE(r.frames.size(), 1);
        feed.setStyle("synthwave");
        QTRY_COMPARE(r.frames.size(), 2);
        QCOMPARE(r.last()["style"].toString(), QString("synthwave"));
        feed.setReduceMotion(true);
        QTRY_COMPARE(r.frames.size(), 3);
        QCOMPARE(r.last()["reduceMotion"].toBool(), true);
        feed.setActive(true);
        QCOMPARE(feed.interval(), LevelsFeed::calmIntervalMs);
    }

    void stoppingSendsOneIdleFrameAndStopsTheClock() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        Moving source;
        feed.setLevelsSource(std::ref(source));
        feed.start();
        Reader r(path);
        QTRY_COMPARE(r.frames.size(), 1);
        feed.setActive(true);
        QTRY_VERIFY(r.frames.size() >= 3);
        feed.setActive(false);
        QVERIFY(!feed.sending());
        QTRY_COMPARE(r.last()["active"].toBool(), false);
        const int count = r.frames.size();
        const int reads = source.n;
        QTest::qWait(300);
        QCOMPARE(r.frames.size(), count);
        QCOMPARE(source.n, reads);
    }

    void theClockStopsWhenTheLastClientLeaves() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed feed({}, path);
        Moving source;
        feed.setLevelsSource(std::ref(source));
        feed.start();
        feed.setActive(true);
        {
            Reader r(path);
            QTRY_COMPARE(feed.clients(), 1);
            QVERIFY(feed.sending());
            QTRY_VERIFY(r.frames.size() >= 2);
            r.socket.disconnectFromServer();
        }
        QTRY_COMPARE(feed.clients(), 0);
        QVERIFY(!feed.sending());
    }

    void aSecondThrumLeavesTheFirstOneFeeding() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        LevelsFeed first({}, path);
        first.start();
        QVERIFY(first.listening());
        LevelsFeed second({}, path);
        second.start();
        QVERIFY(!second.listening());
        Reader r(path);
        QTRY_COMPARE(first.clients(), 1);
    }

    void aStaleSocketFileIsReplaced() {
        QTemporaryDir dir;
        const QString path = dir.filePath("levels.sock");
        QFile stale(path);
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        LevelsFeed feed({}, path);
        feed.start();
        QVERIFY(feed.listening());
        Reader r(path);
        QTRY_COMPARE(feed.clients(), 1);
    }

    void theAppearanceCheckBoxTurnsItOff() {
        QTemporaryDir dir;
        LevelsFeed feed(dir.filePath("window.conf"), dir.filePath("levels.sock"));
        feed.start();
        Theme theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl"));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("levelsFeed", &feed);
        engine.rootContext()->setContextProperty("theme", &theme);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(THRUM_APP_DIR) + "/BarFeedSettings.qml"));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *box = root->findChild<QObject *>("shareWithBar");
        QVERIFY(box);
        QCOMPARE(box->property("checked").toBool(), true);
        QMetaObject::invokeMethod(box, "toggle");
        QMetaObject::invokeMethod(box, "clicked");
        QCOMPARE(feed.enabled(), false);
        QVERIFY(!feed.listening());
        feed.setEnabled(true);
        QCOMPARE(box->property("checked").toBool(), true);
    }
};

int runLevelsFeedTests(int argc, char **argv) {
    LevelsFeedTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "levelsfeed_tests.moc"
