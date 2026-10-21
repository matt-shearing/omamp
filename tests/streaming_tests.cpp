// Playing tracks from a server (a Navidrome or other Subsonic stream) and
// the native engine's device handling, without a sound card and without a
// real server. A local HTTP server (audiohttp.h) serves the engine test
// fixtures with and without byte ranges, slowly, in chunks, late, or
// breaking off part-way. Every engine runs into an in-memory sink;
// main() sets THRUM_NO_AUDIO_DEVICE so a device sink would abort.
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cmath>
#include <memory>

#include "audiohttp.h"
#include "decoder.h"
#include "nativeengine.h"
#include "player.h"
#include "qtengine.h"
#include "streaming.h"

namespace {
const QString kToken = QStringLiteral("u=fixture&t=5ecr3770ken&s=sa1t&v=1.16.1&c=thrum");

QVector<float> decodeAll(Decoder &d) {
    QVector<float> out;
    float buf[4096 * 2];
    while (int n = d.read(buf, 4096))
        out.append(QVector<float>(buf, buf + n * 2));
    return out;
}

QVector<float> decodeFile(const QString &path) {
    Decoder d;
    if (!d.open(path))
        return {};
    return decodeAll(d);
}

double maxDiff(const QVector<float> &a, const QVector<float> &b) {
    if (a.size() != b.size())
        return 1e9;
    double worst = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        worst = qMax(worst, double(qAbs(a[i] - b[i])));
    return worst;
}

// Amplitude of one frequency over a window (Goertzel), left channel.
double toneLevel(const QVector<float> &s, qint64 from, qint64 n, double hz, int rate) {
    const double w = 2 * M_PI * hz / rate, coeff = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (qint64 i = 0; i < n; ++i) {
        const double x = s[(from + i) * 2] + coeff * s1 - s2;
        s2 = s1;
        s1 = x;
    }
    return 2 * std::sqrt(qMax(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / n;
}

// A native engine on a sink pulled by hand.
struct Rig {
    BufferSink *buffer;
    PcmSink *sink;
    std::unique_ptr<NativeEngine> engine;
    bool ended = false;

    explicit Rig(int rate = 48000, qint64 latency = -1) : buffer(new BufferSink(rate)) {
        sink = latency >= 0 ? static_cast<PcmSink *>(new LatencySink(buffer, latency)) : buffer;
        engine.reset(new NativeEngine(sink));
        QObject::connect(engine.get(), &AudioEngine::endOfQueue, [this] { ended = true; });
    }

    void pull(qint64 frames, int timeoutMs = 20000) {
        QElapsedTimer clock;
        clock.start();
        qint64 got = 0;
        while (got < frames && !ended && clock.elapsed() < timeoutMs) {
            const qint64 n = buffer->pull(qMin<qint64>(4096, frames - got));
            got += n;
            engine->poll();
            if (n == 0 && engine->state() != AudioEngine::Playing && !engine->ramping())
                break;
            if (n == 0)
                QThread::usleep(200);
        }
    }
    void pullToEnd() { pull(std::numeric_limits<qint64>::max()); }
};

Playlist::Track remote(const QString &name, const QString &title, qint64 id = 0) {
    Playlist::Track t;
    t.path = "subsonic://1/" + name;
    t.title = title;
    t.artist = "Fixture";
    t.trackId = id;
    return t;
}

Playlist::Track local(const QString &path) { return Playlist::trackFromFile(path); }
} // namespace

class StreamingTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<AudioHttp> m_http;
    QString fx(const QString &name) const { return m_dir.filePath(name); }
    QUrl url(const QString &name) const { return QUrl(m_http->url(name, kToken)); }
    // The Player's resolver: subsonic://1/<file> is that file on the server.
    QStringList m_resolved;
    QList<qint64> m_resolvedIds;
    Player::Resolver resolver() {
        return [this](const QString &path, qint64 id, QString *error) -> QUrl {
            m_resolved << path;
            m_resolvedIds << id;
            const QString name = path.section('/', 3);
            if (name == "gone") {
                *error = "no longer in the library";
                return {};
            }
            return url(name);
        };
    }

private slots:
    void initTestCase() {
        QVERIFY(qEnvironmentVariableIsSet("THRUM_NO_AUDIO_DEVICE"));
        QProcess make;
        make.start(QStringLiteral(THRUM_TESTS_DIR "/make-engine-fixtures"), {m_dir.path()});
        QVERIFY(make.waitForFinished(120000));
        QVERIFY2(make.exitCode() == 0, make.readAllStandardError().constData());
        QVERIFY(QFile::copy(fx("a.flac"), fx("a3.flac")));
        m_http = std::make_unique<AudioHttp>();
        for (const QString &name : {"short1.flac", "short2.flac", "part1.flac", "part2.flac", "ramp.flac",
                                    "a.flac", "b.flac", "a3.flac"})
            m_http->addFile(name, fx(name));
        // A dead server fails in seconds here, not the real fifteen.
        Streaming::Options o;
        o.rwTimeoutMs = 3000;
        o.reconnectDelayMaxS = 1;
        Streaming::setOptions(o);
    }

    void init() {
        m_resolved.clear();
        m_resolvedIds.clear();
        for (const QString &name : {"short1.flac", "short2.flac", "part1.flac", "part2.flac", "ramp.flac"})
            m_http->setBehaviour(name, {});
        m_http->clearRequests();
    }

    void cleanupTestCase() { m_http.reset(); }

    void redactsSecrets() {
        const QString u = "http://nas:4533/rest/stream.view?id=7&u=matt&t=abc123&s=xyz&apiKey=K3Y&p=enc:7365&f=json";
        const QString r = AudioEngine::redact("Failed to open " + u);
        for (const char *secret : {"abc123", "xyz", "K3Y", "enc:7365"})
            QVERIFY2(!r.contains(secret), qPrintable(r));
        QVERIFY(r.contains("id=7") && r.contains("u=matt") && r.contains("f=json"));
        QCOMPARE(AudioEngine::redact("https://matt:hunter2@nas/x"), QString("https://matt:***@nas/x"));
        QCOMPARE(AudioEngine::redact("/music/Artist - t=1.flac"), QString("/music/Artist - t=1.flac"));
        QVERIFY(Playlist::isReference("subsonic://1/abc"));
        QVERIFY(Playlist::isReference("https://example.com/a.mp3"));
        QVERIFY(!Playlist::isReference("/music/a.flac"));
        QVERIFY(!Playlist::isReference("file:///music/a.flac"));
        QVERIFY(Streaming::userAgent().startsWith("Thrum/"));
    }

    void decoderOpensAStream() {
        Decoder d;
        QString error;
        QVERIFY2(d.open("subsonic://1/part1", url("part1.flac"), &error), qPrintable(error));
        QVERIFY(d.isNetwork());
        QVERIFY(d.seekable());
        QCOMPARE(d.path(), QString("subsonic://1/part1"));
        QCOMPARE(d.sampleRate(), 48000);
        QVERIFY(d.durationMs() > 2900);
        QCOMPARE(maxDiff(decodeAll(d), decodeFile(fx("part1.flac"))), 0.0);
        const auto requests = m_http->requests();
        QVERIFY(!requests.isEmpty());
        QVERIFY2(requests.first().headers.value("user-agent").startsWith("Thrum/"),
                 requests.first().headers.value("user-agent").constData());
        QVERIFY(requests.first().query.contains("t=5ecr3770ken"));
    }

    void decoderReportsAServerErrorWithoutTheToken() {
        Decoder d;
        QString error;
        QVERIFY(!d.open("subsonic://1/missing", url("missing.flac"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(!error.contains("5ecr3770ken") && !error.contains("sa1t"), qPrintable(error));
        // Nobody listening at all.
        QVERIFY(!d.open("x", QUrl("http://127.0.0.1:1/stream/a.flac?" + kToken), &error));
        QVERIFY2(!error.contains("5ecr3770ken"), qPrintable(error));
    }

    void decoderSeeksWithRanges() {
        Decoder d;
        QVERIFY(d.open("ramp", url("ramp.flac")));
        QVERIFY(d.seekable());
        float frame[2];
        for (int ms : {1500, 500, 2000, 0, 2999}) {
            m_http->clearRequests();
            QVERIFY(d.seek(ms));
            QCOMPARE(d.read(frame, 1), 1);
            QCOMPARE(qRound(frame[0] * 32768) + 16384, int(qRound64(ms * 44.1) % 32768));
        }
        // The read-ahead had the whole small file; a seek to the start of
        // a fresh decoder asks the server for a range.
        Decoder far;
        m_http->clearRequests();
        QVERIFY(far.open("part2", url("part2.flac")));
        QVERIFY(far.seek(2500));
        QCOMPARE(far.read(frame, 1), 1);
    }

    void decoderSeeksWithoutRanges() {
        AudioHttp::Behaviour b;
        b.ranges = false;
        m_http->setBehaviour("ramp.flac", b);
        Decoder d;
        QVERIFY(d.open("ramp", url("ramp.flac")));
        float frame[2];
        // Forward reads on; backward opens the stream again from the start.
        for (int ms : {1500, 2000, 500, 2999, 1}) {
            QVERIFY(d.seek(ms));
            QCOMPARE(d.read(frame, 1), 1);
            QCOMPARE(qRound(frame[0] * 32768) + 16384, int(qRound64(ms * 44.1) % 32768));
        }
        for (const auto &r : m_http->requests())
            QVERIFY(!r.headers.contains("range") || r.headers.value("range") == "bytes=0-");
    }

    void slowChunkedDelivery_data() {
        QTest::addColumn<bool>("chunked");
        QTest::addColumn<bool>("readAhead");
        QTest::newRow("chunked, read-ahead") << true << true;
        QTest::newRow("chunked, direct") << true << false;
        QTest::newRow("slow, read-ahead") << false << true;
        QTest::newRow("slow, direct") << false << false;
    }
    void slowChunkedDelivery() {
        QFETCH(bool, chunked);
        QFETCH(bool, readAhead);
        const Streaming::Options before = Streaming::options();
        Streaming::Options o = before;
        o.readAhead = readAhead;
        Streaming::setOptions(o);
        AudioHttp::Behaviour b;
        b.chunked = chunked;
        b.ranges = false;
        b.pieceBytes = 8192;
        b.pieceDelayMs = 5;
        m_http->setBehaviour("part1.flac", b);
        Decoder d;
        QVERIFY(d.open("part1", url("part1.flac")));
        QVERIFY(!d.seekable());
        const QVector<float> got = decodeAll(d);
        Streaming::setOptions(before);
        QCOMPARE(maxDiff(got, decodeFile(fx("part1.flac"))), 0.0);
        QVERIFY2(d.readError().isEmpty(), qPrintable(d.readError()));
    }

    void disconnectIsRepairedWithRanges() {
        AudioHttp::Behaviour b;
        b.dropAfter = 150000;
        b.pieceBytes = 16384;
        m_http->setBehaviour("part2.flac", b);
        Decoder d;
        QVERIFY(d.open("part2", url("part2.flac")));
        QCOMPARE(maxDiff(decodeAll(d), decodeFile(fx("part2.flac"))), 0.0);
        QVERIFY2(d.readError().isEmpty(), qPrintable(d.readError()));
        // FFmpeg went back for the rest with a Range request.
        bool resumed = false;
        for (const auto &r : m_http->requests())
            if (r.headers.value("range").startsWith("bytes=") && r.headers.value("range") != "bytes=0-")
                resumed = true;
        QVERIFY(resumed);
        QVERIFY(m_http->requestsFor("part2.flac") >= 2);
    }

    void disconnectWithoutRangesIsAnError() {
        AudioHttp::Behaviour b;
        b.ranges = false;
        b.dropAfter = 150000;
        b.dropOnce = false;
        b.pieceBytes = 16384;
        m_http->setBehaviour("part2.flac", b);
        Decoder d;
        QVERIFY(d.open("part2", url("part2.flac")));
        QElapsedTimer clock;
        clock.start();
        const QVector<float> got = decodeAll(d);
        QVERIFY(clock.elapsed() < 10000);
        QVERIFY(got.size() < decodeFile(fx("part2.flac")).size());
        QVERIFY2(!d.readError().isEmpty(), "a stream that broke off must say so");
        QVERIFY(!d.readError().contains("5ecr3770ken"));
    }

    // Local file into a stream, and stream into stream: one signal cut in
    // two must come out whole, sample for sample.
    void gaplessIntoAndBetweenStreams_data() {
        QTest::addColumn<bool>("firstRemote");
        QTest::newRow("local to remote") << false;
        QTest::newRow("remote to remote") << true;
    }
    void gaplessIntoAndBetweenStreams() {
        QFETCH(bool, firstRemote);
        Rig rig;
        QCOMPARE(rig.engine->sink()->kind(), QString("buffer"));
        if (firstRemote)
            rig.engine->load("subsonic://1/part1", url("part1.flac"));
        else
            rig.engine->load(fx("part1.flac"));
        rig.engine->queueNext("subsonic://1/part2", url("part2.flac"));
        QSignalSpy advanced(rig.engine.get(), &AudioEngine::advanced);
        rig.engine->play();
        rig.pullToEnd();
        QCOMPARE(advanced.count(), 1);
        const QVector<float> ref = decodeFile(fx("src.wav"));
        QCOMPARE(rig.buffer->frames(), qint64(288000));
        // After the 10 ms ramp of pressing play.
        double worst = 0;
        for (qsizetype i = 2000; i < ref.size(); ++i)
            worst = qMax(worst, double(qAbs(rig.buffer->data()[i] - ref[i])));
        QVERIFY2(worst < 1e-4, qPrintable(QString::number(worst)));
    }

    void engineSeeksAStream() {
        Rig rig(44100);
        rig.engine->load("ramp", url("ramp.flac"));
        rig.engine->play();
        rig.pull(4410);
        rig.engine->seek(2000);
        rig.buffer->clear();
        rig.pull(3000);
        QVERIFY(rig.buffer->frames() >= 3000);
        // Past the fade-in and the old audio's decay, every sample is the right one.
        for (int i = 1000; i < 3000; ++i)
            QCOMPARE(qRound(rig.buffer->data()[i * 2] * 32768) + 16384, int((88200 + i) % 32768));
        QVERIFY(qAbs(rig.engine->position() - 2068) <= 2);
    }

    void playerPlaysStreamsThroughTheResolver() {
        QTemporaryDir dir;
        const QString state = dir.filePath("state.json");
        {
            auto *sink = new BufferSink(48000, false);
            sink->setAutoPull(20);
            Player player(new NativeEngine(sink), state, resolver());
            QCOMPARE(player.appendTracks({remote("short1.flac", "Short One", 11), remote("short2.flac", "Short Two", 12)}), 2);
            player.playIndex(0);
            QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 8000);
            QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 8000);
            QCOMPARE(sink->pulledFrames(), qint64(96000));
            QVERIFY(player.error().isEmpty());
            QVERIFY(m_resolved.contains("subsonic://1/short1.flac"));
            QVERIFY(m_resolved.contains("subsonic://1/short2.flac"));
            QVERIFY(m_resolvedIds.contains(11) && m_resolvedIds.contains(12));
            // Nothing the window can read carries the token.
            QCOMPARE(player.engine()->source().contains("5ecr3770ken"), false);
            QCOMPARE(player.trackAt(1).value("path").toString(), QString("subsonic://1/short2.flac"));
        }
        QFile saved(state);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray json = saved.readAll();
        QVERIFY(json.contains("subsonic://1/short1.flac"));
        QVERIFY(!json.contains("5ecr3770ken") && !json.contains("sa1t") && !json.contains("http"));
        // Back with its tags, and resolved again when played.
        Player again(new NativeEngine(new BufferSink), state, resolver());
        QCOMPARE(again.playlist()->count(), 2);
        // The stream's own tags replaced the library's while it played.
        QCOMPARE(again.playlist()->at(0).title, QString("short1"));
        QCOMPARE(again.playlist()->at(0).artist, QString("Fixture"));
        QCOMPARE(again.playlist()->at(1).trackId, qint64(12));
    }

    void playerSkipsAStreamThatFails() {
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        Player player(new NativeEngine(sink), QString(), resolver());
        player.appendTracks({remote("missing.flac", "Missing Song"), remote("gone", "Gone Song"), local(fx("short2.flac"))});
        QStringList errors;
        connect(&player, &Player::errorChanged, this, [&] { errors << player.error(); });
        player.playIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 8000);
        const QString all = errors.join('\n');
        QVERIFY2(all.contains("Can't play Fixture - Missing Song"), qPrintable(all));
        QVERIFY2(all.contains("Can't play Fixture - Gone Song: no longer in the library"), qPrintable(all));
        QVERIFY2(!all.contains("5ecr3770ken") && !all.contains("sa1t"), qPrintable(all));
    }

    void playerSkipsAStreamThatBreaksOff() {
        AudioHttp::Behaviour b;
        b.ranges = false;
        b.dropAfter = 150000;
        b.dropOnce = false;
        b.pieceBytes = 16384;
        m_http->setBehaviour("part2.flac", b);
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        Player player(new NativeEngine(sink), QString(), resolver());
        player.appendTracks({remote("part2.flac", "Broken Stream"), local(fx("short2.flac"))});
        player.playIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 15000);
        QVERIFY2(player.error().startsWith("Can't play Fixture - Broken Stream"), qPrintable(player.error()));
        QVERIFY(!player.error().contains("5ecr3770ken"));
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 8000);
    }

    void bufferingIsReported() {
        AudioHttp::Behaviour b;
        b.delayMs = 1200;
        m_http->setBehaviour("short1.flac", b);
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(1);
        Player player(new NativeEngine(sink), QString(), resolver());
        QSignalSpy buffering(&player, &Player::bufferingChanged);
        player.appendTracks({remote("short1.flac", "Late")});
        player.playIndex(0);
        QCOMPARE(player.state(), QString("playing"));
        QTRY_VERIFY_WITH_TIMEOUT(player.buffering(), 1100);
        QCOMPARE(player.statusText(), QString("Buffering…"));
        QCOMPARE(player.position(), qint64(0));
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 100, 3000);
        QVERIFY(buffering.count() >= 2);
    }

    // A skip while the old stream waits on a hung server doesn't wait for
    // the server's timeout.
    void aSkipAbandonsAHungStream() {
        Streaming::Options slow = Streaming::options();
        Streaming::Options o = slow;
        o.rwTimeoutMs = 30000;
        Streaming::setOptions(o);
        AudioHttp::Behaviour b;
        b.delayMs = 20000;
        m_http->setBehaviour("short1.flac", b);
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(1);
        {
            NativeEngine engine(sink);
            engine.load("subsonic://1/short1.flac", url("short1.flac"));
            engine.play();
            QTest::qWait(300);
            QElapsedTimer clock;
            clock.start();
            engine.load(fx("short2.flac"));
            engine.play();
            QTRY_VERIFY_WITH_TIMEOUT(engine.position() > 50, 3000);
            QVERIFY(clock.elapsed() < 3000);
            QCOMPARE(engine.source(), fx("short2.flac"));
            // Closing doesn't wait for the server either.
            clock.restart();
            engine.queueNext("subsonic://1/short1.flac", url("short1.flac"));
            QTest::qWait(100);
        }
        Streaming::setOptions(slow);
        m_http->setBehaviour("short1.flac", {});
    }

    void qtEnginePlaysStreams() {
        QtEngine engine(false);
        QVERIFY(!engine.hasDevice());
        QSignalSpy advanced(&engine, &AudioEngine::advanced);
        QSignalSpy ended(&engine, &AudioEngine::endOfQueue);
        QSignalSpy failed(&engine, &AudioEngine::trackFailed);
        engine.load("subsonic://1/short1.flac", url("short1.flac"));
        QCOMPARE(engine.source(), QString("subsonic://1/short1.flac"));
        engine.queueNext("subsonic://1/short2.flac", url("short2.flac"));
        engine.play();
        QTRY_COMPARE_WITH_TIMEOUT(advanced.count(), 1, 10000);
        QCOMPARE(engine.source(), QString("subsonic://1/short2.flac"));
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(m_http->requestsFor("short1.flac") >= 1);
        QVERIFY(m_http->requestsFor("short2.flac") >= 1);
        engine.load("subsonic://1/missing.flac", url("missing.flac"));
        engine.play();
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 10000);
        QCOMPARE(failed.first().at(0).toString(), QString("subsonic://1/missing.flac"));
        QVERIFY(!failed.first().at(1).toString().contains("5ecr3770ken"));
    }

    // The device's buffer is taken off the position, and a track change
    // is announced when its first sample is heard, not when it is taken.
    void positionFollowsTheDeviceLatency() {
        QCOMPARE(DeviceSink::queuedEstimate(48000, 900000, 48000), qint64(4800));
        QCOMPARE(DeviceSink::queuedEstimate(1000, 900000, 48000), qint64(0));
        QCOMPARE(DeviceSink::queuedEstimate(480000, 0, 48000), qint64(24000));

        Rig rig(48000, 4800); // 100 ms in the "device"
        QCOMPARE(rig.engine->sink()->kind(), QString("buffer"));
        rig.engine->load(fx("short1.flac"));
        rig.engine->queueNext(fx("short2.flac"));
        QSignalSpy advanced(rig.engine.get(), &AudioEngine::advanced);
        rig.engine->play();
        rig.pull(4800);
        QCOMPARE(rig.engine->position(), qint64(0));
        rig.pull(24000 - 4800);
        QCOMPARE(rig.engine->position(), qint64(400));
        // short1 is 48,000 frames: taken, but 100 ms of it not yet heard.
        rig.pull(48000 + 2400 - 24000);
        QCOMPARE(advanced.count(), 0);
        QCOMPARE(rig.engine->source(), fx("short1.flac"));
        QCOMPARE(rig.engine->position(), qint64(950));
        rig.pull(2400);
        QCOMPARE(advanced.count(), 1);
        QCOMPARE(rig.engine->source(), fx("short2.flac"));
        QCOMPARE(rig.engine->position(), qint64(0));
        rig.pull(9600);
        QCOMPARE(rig.engine->position(), qint64(200));
        // After a seek the clock holds until the new audio is heard.
        rig.engine->seek(500);
        rig.pull(2400);
        QCOMPARE(rig.engine->position(), qint64(500));
        rig.pull(4800);
        QCOMPARE(rig.engine->position(), qint64(550));
    }

    void manualSkipCrossfades() {
        Rig rig;
        AudioEngine::Settings s;
        s.crossfadeMs = 1000;
        rig.engine->setSettings(s);
        rig.engine->load(fx("a.flac")); // 1 kHz
        rig.engine->play();
        rig.pull(24000);
        QSignalSpy advanced(rig.engine.get(), &AudioEngine::advanced);
        rig.engine->skipTo(fx("b.flac")); // 3 kHz
        QCOMPARE(rig.engine->state(), AudioEngine::Playing);
        rig.pull(120000);
        QCOMPARE(advanced.count(), 0);
        QCOMPARE(rig.engine->source(), fx("b.flac"));
        const QVector<float> &d = rig.buffer->data();
        QVERIFY(rig.buffer->frames() >= 144000);
        // Just before the skip: only the first track.
        QVERIFY(qAbs(toneLevel(d, 20000, 2400, 1000, 48000) - 0.5) < 0.02);
        // Halfway through the fade: both at cos 45°.
        const double a = toneLevel(d, 48000 - 1200, 2400, 1000, 48000);
        const double b = toneLevel(d, 48000 - 1200, 2400, 3000, 48000);
        QVERIFY2(qAbs(a - 0.354) < 0.03 && qAbs(b - 0.354) < 0.03, qPrintable(QString("%1 %2").arg(a).arg(b)));
        // After it: only the new track.
        QVERIFY(toneLevel(d, 76000, 2400, 1000, 48000) < 0.01);
        QVERIFY(qAbs(toneLevel(d, 76000, 2400, 3000, 48000) - 0.5) < 0.02);
        // The old track carried on from where it was, with no jump back.
        double worst = 0;
        for (qint64 i = 23900; i < 24200; ++i)
            worst = qMax(worst, double(qAbs(d[i * 2] - d[(i - 1) * 2])));
        QVERIFY2(worst < 0.1, qPrintable(QString::number(worst)));
    }

    void manualSkipWithoutCrossfadeCuts() {
        Rig rig;
        rig.engine->load(fx("a.flac"));
        rig.engine->play();
        rig.pull(24000);
        rig.engine->skipTo(fx("b.flac"));
        rig.pull(24000);
        const QVector<float> &d = rig.buffer->data();
        QVERIFY(toneLevel(d, 24000 + 600, 2400, 1000, 48000) < 0.01);
        QVERIFY(qAbs(toneLevel(d, 24000 + 600, 2400, 3000, 48000) - 0.5) < 0.02);
    }

    // A crossfade used to swallow the requeue that came while it ran, so a
    // third track never played.
    void crossfadeKeepsTheQueueGoing() {
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        Player player(new NativeEngine(sink), QString());
        player.setCrossfadeSeconds(1);
        player.addPaths({fx("a.flac"), fx("b.flac"), fx("a3.flac")});
        player.playIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 10000);
        // Three tracks, two 1 s fades. The second fade may start a few
        // milliseconds short when the third track's open (on its own
        // thread) lands after the fade point; at 20× real time it can.
        const qint64 expected = 3 * 144000 - 2 * 48000;
        QVERIFY2(sink->pulledFrames() >= expected && sink->pulledFrames() < expected + 4800,
                 qPrintable(QString::number(sink->pulledFrames())));
    }

    void playerSkipCrossfadesAndCounts() {
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(5);
        Player player(new NativeEngine(sink), QString());
        player.setCrossfadeSeconds(1);
        player.addPaths({fx("a.flac"), fx("b.flac")});
        QStringList started;
        connect(&player, &Player::playStarted, this, [&](const QString &p) { started << p; });
        player.playIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 300, 5000);
        player.next();
        QCOMPARE(player.currentIndex(), 1);
        QCOMPARE(player.state(), QString("playing"));
        QCOMPARE(started, QStringList({fx("a.flac"), fx("b.flac")}));
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("b"), 3000);
    }
};

int runStreamingTests(int argc, char **argv) {
    StreamingTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "streaming_tests.moc"
