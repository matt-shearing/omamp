// The playback engines, measured without a sound card. The native engine
// runs into a BufferSink that pulls PCM into memory as fast as it is
// decoded; the Qt engine runs without an audio output. Every test asserts
// the sink is the in-memory one, and main() sets THRUM_NO_AUDIO_DEVICE so
// the device sink would abort rather than play.
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cmath>
#include <memory>

#include "decoder.h"
#include "equalizer.h"
#include "nativeengine.h"
#include "outputfollower.h"
#include "player.h"
#include "qtengine.h"

namespace {
constexpr qint64 kCut = 144123, kTotal = 288000;

QVector<float> decodeAll(const QString &path, int *rate = nullptr) {
    Decoder d;
    if (!d.open(path))
        return {};
    if (rate)
        *rate = d.sampleRate();
    QVector<float> out;
    float buf[4096 * 2];
    while (int n = d.read(buf, 4096))
        out.append(QVector<float>(buf, buf + n * 2));
    return out;
}

double rms(const QVector<float> &s, qint64 from, qint64 to, int channel = 0) {
    double sum = 0;
    to = qMin(to, qint64(s.size() / 2));
    for (qint64 i = from; i < to; ++i)
        sum += double(s[i * 2 + channel]) * s[i * 2 + channel];
    return std::sqrt(sum / qMax<qint64>(1, to - from));
}

// Amplitude of one frequency in a window (Goertzel), left channel.
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

// The shift (in samples) that best lines out up with ref over a window, and
// the largest difference at that shift.
int bestLag(const QVector<float> &out, const QVector<float> &ref, qint64 from, qint64 n, int range,
            double *maxErr = nullptr) {
    int best = 0;
    double bestErr = 1e30;
    for (int lag = -range; lag <= range; ++lag) {
        double err = 0;
        for (qint64 i = from; i < from + n; ++i) {
            const qint64 j = i + lag;
            if (j < 0 || j * 2 + 1 >= out.size() || i * 2 + 1 >= ref.size())
                continue;
            const double d0 = out[j * 2] - ref[i * 2], d1 = out[j * 2 + 1] - ref[i * 2 + 1];
            err += d0 * d0 + d1 * d1;
        }
        if (err < bestErr) {
            bestErr = err;
            best = lag;
        }
    }
    if (maxErr) {
        *maxErr = 0;
        for (qint64 i = from; i < from + n; ++i) {
            const qint64 j = i + best;
            if (j >= 0 && j * 2 + 1 < out.size() && i * 2 + 1 < ref.size())
                *maxErr = qMax(*maxErr, double(qAbs(out[j * 2] - ref[i * 2])));
        }
    }
    return best;
}

double maxDelta(const QVector<float> &s, qint64 from, qint64 to) {
    double worst = 0;
    to = qMin(to, qint64(s.size() / 2));
    for (qint64 i = qMax<qint64>(1, from); i < to; ++i)
        for (int c = 0; c < 2; ++c)
            worst = qMax(worst, double(qAbs(s[i * 2 + c] - s[(i - 1) * 2 + c])));
    return worst;
}

// A native engine on a BufferSink, driven by hand.
struct Rig {
    BufferSink *sink;
    std::unique_ptr<NativeEngine> engine;
    bool ended = false;

    explicit Rig(int rate = 48000) : sink(new BufferSink(rate)), engine(new NativeEngine(sink)) {
        QObject::connect(engine.get(), &AudioEngine::endOfQueue, [this] { ended = true; });
    }

    // Pulls until frames have been taken (or the queue ends).
    void pull(qint64 frames) {
        QElapsedTimer clock;
        clock.start();
        qint64 got = 0;
        while (got < frames && !ended && clock.elapsed() < 20000) {
            const qint64 n = sink->pull(qMin<qint64>(4096, frames - got));
            got += n;
            engine->poll();
            // Paused and the ramp done: nothing more will come.
            if (n == 0 && engine->state() != AudioEngine::Playing && !engine->ramping())
                break;
            if (n == 0)
                QThread::usleep(200);
        }
    }
    void pullToEnd() { pull(std::numeric_limits<qint64>::max()); }
};
} // namespace

// The sound server and Qt's device list, as the output follower sees them.
// Nothing here talks to PipeWire or opens a device.
class FakeDefaultSource : public DefaultOutputSource {
    Q_OBJECT

public:
    QString name;
    int starts = 0, queries = 0;
    bool live = false, startFails = false;
    QString defaultSinkName() override {
        ++queries;
        return name;
    }
    void startWatching() override {
        ++starts;
        live = !startFails;
    }
    void stopWatching() override { live = false; }
    bool watching() const override { return live; }
    // pactl exiting, as when pipewire-pulse restarts.
    void die() {
        live = false;
        emit watchStopped();
    }
    void announce() { emit changed(); }
};

class FakeOutputList : public OutputList {
    Q_OBJECT

public:
    QList<QByteArray> ids, afterRefresh;
    QByteArray qtDefault;
    int refreshes = 0;
    QList<QByteArray> outputIds() override { return ids; }
    QByteArray qtDefaultId() override { return qtDefault; }
    void refresh() override {
        ++refreshes;
        if (!afterRefresh.isEmpty())
            ids = afterRefresh;
    }
    void set(const QList<QByteArray> &list, const QByteArray &def) {
        ids = list;
        qtDefault = def;
        emit changed();
    }
};

class EngineTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString fx(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void initTestCase() {
        QVERIFY(qEnvironmentVariableIsSet("THRUM_NO_AUDIO_DEVICE"));
        QProcess make;
        make.start(QStringLiteral(THRUM_TESTS_DIR "/make-engine-fixtures"), {m_dir.path()});
        QVERIFY(make.waitForFinished(120000));
        QVERIFY2(make.exitCode() == 0, make.readAllStandardError().constData());
    }

    void neverUsesTheSoundCard() {
        Rig rig;
        QCOMPARE(rig.engine->sink()->kind(), QString("buffer"));
        // Without a sink and with THRUM_NO_AUDIO_DEVICE set, the engine
        // picks a silent buffer too.
        NativeEngine fallback;
        QCOMPARE(fallback.sink()->kind(), QString("buffer"));
        QtEngine qt;
        // No device, so nothing follows the default output either.
        QVERIFY(!qt.outputFollower());
        QVERIFY(!qt.hasDevice());
    }

    void decoderReadsTagsAndReplayGain() {
        Decoder d;
        QVERIFY(d.open(fx("rg.flac")));
        QCOMPARE(d.sampleRate(), 48000);
        QCOMPARE(d.replayGain().trackGain, -6.0);
        QCOMPARE(d.replayGain().albumGain, -3.0);
        QCOMPARE(d.replayGain().trackPeak, 0.25);
        QVERIFY(d.open(fx("a.flac")));
        QCOMPARE(d.metaData().value("album").toString(), QString("Album A"));
        QCOMPARE(d.metaData().value("albumArtist").toString(), QString("Fixture"));
        QCOMPARE(Decoder::parseGain("+1,5 dB"), 1.5);
        QVERIFY(std::isnan(Decoder::parseGain("loud")));
        QString error;
        QVERIFY(!d.open(fx("broken.mp3"), &error));
        QVERIFY(!error.isEmpty());
    }

    void decoderSeeksExactly() {
        Decoder d;
        QVERIFY(d.open(fx("ramp.flac")));
        float frame[2];
        QCOMPARE(d.read(frame, 1), 1);
        QCOMPARE(qRound(frame[0] * 32768) + 16384, 0);
        for (int ms : {1500, 500, 2000, 1, 0, 2999}) {
            QVERIFY(d.seek(ms));
            QCOMPARE(d.read(frame, 1), 1);
            QCOMPARE(qRound(frame[0] * 32768) + 16384, int(qRound64(ms * 44.1) % 32768));
        }
    }

    void gaplessJoin_data() {
        QTest::addColumn<QString>("ext");
        QTest::addColumn<int>("lengthTolerance"); // frames, either way
        QTest::addColumn<int>("lagTolerance");    // samples of misalignment after the join
        QTest::addColumn<double>("maxError");     // largest difference from the original after the join
        QTest::newRow("flac") << "flac" << 0 << 0 << 1e-4;
        QTest::newRow("mp3") << "mp3" << 0 << 0 << 0.05;
        QTest::newRow("opus") << "opus" << 0 << 0 << 0.05;
        QTest::newRow("aac") << "m4a" << 0 << 0 << 0.1;
    }

    // Two halves of one signal must come out as the signal: the right total
    // length, no shift after the join, and no jump at it.
    void gaplessJoin() {
        QFETCH(QString, ext);
        QFETCH(int, lengthTolerance);
        QFETCH(int, lagTolerance);
        QFETCH(double, maxError);
        const QVector<float> ref = decodeAll(fx("src.wav"));
        QCOMPARE(ref.size() / 2, kTotal);

        Rig rig;
        QSignalSpy advanced(rig.engine.get(), &AudioEngine::advanced);
        rig.engine->load(fx("part1." + ext));
        rig.engine->queueNext(fx("part2." + ext));
        rig.engine->play();
        rig.pullToEnd();
        QVERIFY(rig.ended);
        QCOMPARE(advanced.count(), 1);
        const QVector<float> &out = rig.sink->data();
        const qint64 frames = out.size() / 2;

        double errBefore = 0, errAfter = 0, errMid = 0;
        const int lagMid = bestLag(out, ref, 48000, 4800, 64, &errMid);
        const int lagBefore = bestLag(out, ref, kCut - 4800, 4700, 64, &errBefore);
        const int lagAfter = bestLag(out, ref, kCut + 1024, 4800, 64, &errAfter);
        double errJoin = 0;
        for (qint64 i = kCut - 64; i < kCut + 64; ++i)
            errJoin = qMax(errJoin, double(qAbs(out[i * 2] - ref[i * 2])));
        const double jump = maxDelta(out, kCut - 64, kCut + 64);
        const double natural = maxDelta(ref, kCut - 64, kCut + 64);
        qInfo("MEASURE gapless %s: frames %lld (expected %lld, %+lld); lag mid %d before %d after %d; "
              "max error mid %.5f before %.5f join %.5f after %.5f; largest step at join %.5f (signal's own %.5f)",
              qPrintable(ext), frames, kTotal, frames - kTotal, lagMid, lagBefore, lagAfter, errMid, errBefore,
              errJoin, errAfter, jump, natural);
        QVERIFY2(qAbs(frames - kTotal) <= lengthTolerance, "total length");
        QVERIFY(qAbs(lagMid) <= lagTolerance);
        QVERIFY(qAbs(lagAfter) <= lagTolerance);
        QVERIFY(errAfter < maxError);
        // Lossy halves each carry their encoder's own edge at the cut (measured
        // above); only the lossless join must be as smooth as the signal.
        if (ext == "flac")
            QVERIFY(jump < natural * 1.01);
    }

    // 44.1 kHz into a 48 kHz sink: the resampler runs straight across the
    // join, so there is no edge in the middle.
    void gaplessThroughTheResampler() {
        Rig rig;
        rig.engine->load(fx("rs1.flac"));
        rig.engine->queueNext(fx("rs2.flac"));
        rig.engine->play();
        rig.pullToEnd();
        const QVector<float> &out = rig.sink->data();
        const QVector<float> ref = decodeAll(fx("src.wav")); // the same tones at 48 kHz
        const qint64 frames = out.size() / 2;
        const qint64 join = 132413LL * 48000 / 44100;
        double errJoin = 0, errMid = 0;
        const int lag = bestLag(out, ref, join - 2400, 4800, 32, &errJoin);
        const int lagMid = bestLag(out, ref, 48000, 4800, 32, &errMid);
        const double jump = maxDelta(out, join - 64, join + 64);
        qInfo("MEASURE resampled 44.1k->48k: frames %lld (expected %lld); lag mid %d join %d; "
              "max error mid %.2e join %.2e; step at join %.5f",
              frames, kTotal, lagMid, lag, errMid, errJoin, jump);
        QVERIFY(qAbs(frames - kTotal) <= 2);
        QCOMPARE(lag, lagMid);
        QVERIFY(errJoin < 0.01);
        QVERIFY(jump < 0.07);
    }

    void crossfadeOverlapsTheTracks() {
        Rig rig;
        AudioEngine::Settings s;
        s.crossfadeMs = 1000;
        rig.engine->setSettings(s);
        QSignalSpy advanced(rig.engine.get(), &AudioEngine::advanced);
        rig.engine->load(fx("a.flac"));
        rig.engine->queueNext(fx("b.flac"));
        rig.engine->play();
        rig.pullToEnd();
        const QVector<float> &out = rig.sink->data();
        const qint64 frames = out.size() / 2, fade = 48000, a = 144000;
        // A (1 kHz) alone, then both at equal power, then B (3 kHz) alone.
        const double before1k = toneLevel(out, 24000, 4800, 1000, 48000);
        const double mid1k = toneLevel(out, a - fade / 2 - 2400, 4800, 1000, 48000);
        const double mid3k = toneLevel(out, a - fade / 2 - 2400, 4800, 3000, 48000);
        const double after3k = toneLevel(out, a + 24000, 4800, 3000, 48000);
        const double after1k = toneLevel(out, a + 24000, 4800, 1000, 48000);
        qInfo("MEASURE crossfade 1 s: frames %lld (expected %lld); 1 kHz before %.3f, mid-fade 1 kHz %.3f 3 kHz %.3f, "
              "after 3 kHz %.3f 1 kHz %.4f",
              frames, 2 * a - fade, before1k, mid1k, mid3k, after3k, after1k);
        QCOMPARE(frames, 2 * a - fade);
        QCOMPARE(advanced.count(), 1);
        QVERIFY(qAbs(mid1k - 0.5 * M_SQRT1_2) < 0.02);
        QVERIFY(qAbs(mid3k - 0.5 * M_SQRT1_2) < 0.02);
        QVERIFY(after1k < 0.001);
        QVERIFY(qAbs(after3k - 0.5) < 0.01);
    }

    void noCrossfadeWithinAnAlbum() {
        Rig rig;
        AudioEngine::Settings s;
        s.crossfadeMs = 2000;
        rig.engine->setSettings(s);
        rig.engine->load(fx("a.flac"));
        rig.engine->queueNext(fx("c.flac")); // also "Album A"
        rig.engine->play();
        rig.pullToEnd();
        QCOMPARE(rig.sink->frames(), qint64(288000));
    }

    void replayGain_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("mode");
        QTest::addColumn<double>("preamp");
        QTest::addColumn<double>("expectedDb");
        QTest::newRow("off") << "rg.flac" << int(AudioEngine::ReplayGainMode::Off) << 0.0 << 0.0;
        QTest::newRow("track") << "rg.flac" << int(AudioEngine::ReplayGainMode::Track) << 0.0 << -6.0;
        QTest::newRow("album") << "rg.flac" << int(AudioEngine::ReplayGainMode::Album) << 0.0 << -3.0;
        QTest::newRow("track+preamp") << "rg.flac" << int(AudioEngine::ReplayGainMode::Track) << 2.0 << -4.0;
        // +12 dB would clip a 0.5 peak; clip prevention stops at 1/0.5.
        QTest::newRow("clip prevention") << "loud.flac" << int(AudioEngine::ReplayGainMode::Track) << 0.0
                                         << 20 * std::log10(2.0);
        QTest::newRow("untagged") << "eq1000.flac" << int(AudioEngine::ReplayGainMode::Track) << 6.0 << 0.0;
    }

    void replayGain() {
        QFETCH(QString, file);
        QFETCH(int, mode);
        QFETCH(double, preamp);
        QFETCH(double, expectedDb);
        Rig rig;
        AudioEngine::Settings s;
        s.replayGain = AudioEngine::ReplayGainMode(mode);
        s.replayGainPreampDb = preamp;
        rig.engine->setSettings(s);
        rig.engine->load(fx(file));
        rig.engine->play();
        rig.pullToEnd();
        const double level = rms(rig.sink->data(), 4800, 43200);
        const double db = 20 * std::log10(level / (0.25 * M_SQRT1_2));
        qInfo("MEASURE replaygain %s: %.3f dB (expected %.3f)", QTest::currentDataTag(), db, expectedDb);
        QVERIFY(qAbs(db - expectedDb) < 0.05);
    }

    void equaliserResponse() {
        Equalizer eq;
        eq.setSampleRate(48000);
        QList<double> bands(10, 0.0);
        bands[5] = 6; // 1 kHz
        eq.configure(true, 0, bands);
        QVERIFY(qAbs(eq.responseDb(1000) - 6) < 0.05);
        QVERIFY(eq.responseDb(125) < 0.1);
        QVERIFY(eq.responseDb(16000) < 0.1);
        // Measured on real sine waves through the filters, every preset.
        for (const QString &name : Equalizer::presetNames()) {
            eq.reset();
            eq.configure(true, -3, Equalizer::preset(name));
            for (double hz : Equalizer::frequencies()) {
                QVector<float> sine(48000 * 2);
                for (int i = 0; i < 48000; ++i)
                    sine[i * 2] = sine[i * 2 + 1] = 0.1f * float(std::sin(2 * M_PI * hz * i / 48000));
                eq.reset();
                eq.process(sine.data(), 48000);
                const double gain = 20 * std::log10(rms(sine, 24000, 48000) / (0.1 * M_SQRT1_2));
                QVERIFY2(qAbs(gain - eq.responseDb(hz)) < 0.1, qPrintable(QString("%1 at %2 Hz").arg(name).arg(hz)));
            }
        }
        // Off is untouched, bit for bit.
        eq.configure(false, 6, Equalizer::preset("Rock"));
        QVector<float> x{0.1f, -0.2f, 0.3f, -0.4f};
        const QVector<float> copy = x;
        eq.process(x.data(), 2);
        QCOMPARE(x, copy);
    }

    void equaliserInTheEngine_data() {
        QTest::addColumn<int>("hz");
        QTest::addColumn<double>("expectedDb");
        QTest::newRow("1 kHz, boosted band") << 1000 << 6.0;
        QTest::newRow("125 Hz, three octaves away") << 125 << 0.0;
        QTest::newRow("4 kHz, two octaves away") << 4000 << 0.0;
    }

    void equaliserInTheEngine() {
        QFETCH(int, hz);
        QFETCH(double, expectedDb);
        Rig rig;
        AudioEngine::Settings s;
        s.eqEnabled = true;
        s.eqBandsDb = QList<double>(10, 0.0);
        s.eqBandsDb[5] = 6;
        rig.engine->setSettings(s);
        rig.engine->load(fx(QString("eq%1.flac").arg(hz)));
        rig.engine->play();
        rig.pullToEnd();
        const double db = 20 * std::log10(rms(rig.sink->data(), 9600, 43200) / (0.25 * M_SQRT1_2));
        qInfo("MEASURE eq +6 dB at 1 kHz, tone %d Hz: %.3f dB", hz, db);
        QVERIFY(qAbs(db - expectedDb) < 0.3);
    }

    void monoPlaysOnBothSides() {
        Rig rig;
        rig.engine->load(fx("mono.flac"));
        rig.engine->play();
        rig.pullToEnd();
        const QVector<float> &out = rig.sink->data();
        QCOMPARE(out.size() / 2, qint64(48000));
        QVERIFY(qAbs(rms(out, 4800, 43200, 0) - 0.5 * M_SQRT1_2) < 0.005);
        QCOMPARE(rms(out, 4800, 43200, 1), rms(out, 4800, 43200, 0));
    }

    // A sawtooth that counts samples: after a seek, the output says exactly
    // which sample it is.
    void seekIsSampleAccurate() {
        Rig rig(44100);
        rig.engine->load(fx("ramp.flac"));
        rig.engine->play();
        rig.pull(10000);
        rig.engine->seek(1500);
        QCOMPARE(rig.engine->position(), qint64(1500));
        const qint64 before = rig.sink->frames();
        rig.pull(4000);
        const QVector<float> &out = rig.sink->data();
        // Past the 5 ms fade-in, every sample is the one it should be.
        for (qint64 k = 2000; k < 4000; ++k) {
            const int value = qRound(out[(before + k) * 2] * 32768) + 16384;
            QCOMPARE(value, int((66150 + k) % 32768));
        }
        QVERIFY(qAbs(rig.engine->position() - (1500 + 4000 * 1000 / 44100)) <= 1);
    }

    void seekInLossyFiles_data() {
        QTest::addColumn<QString>("ext");
        QTest::newRow("mp3") << "mp3";
        QTest::newRow("opus") << "opus";
        QTest::newRow("aac") << "m4a";
    }

    void seekInLossyFiles() {
        QFETCH(QString, ext);
        const QVector<float> ref = decodeAll(fx("src.wav"));
        Rig rig;
        rig.engine->load(fx("part1." + ext));
        rig.engine->play();
        rig.pull(4800);
        rig.engine->seek(1500);
        const qint64 before = rig.sink->frames();
        rig.pull(9600);
        QVector<float> after(rig.sink->data().mid(before * 2));
        // Compare against the original from 1.5 s on.
        const QVector<float> refAfter = ref.mid(72000 * 2, 9600 * 2);
        double err = 0;
        const int lag = bestLag(after, refAfter, 1000, 4800, 200, &err);
        qInfo("MEASURE seek %s to 1500 ms: off by %d samples, max error %.4f", qPrintable(ext), lag, err);
        QCOMPARE(lag, 0);
    }

    void pauseAndResumeRamp() {
        Rig rig;
        rig.engine->load(fx("a.flac"));
        rig.engine->play();
        rig.pull(24000);
        rig.engine->pause();
        QCOMPARE(rig.engine->state(), AudioEngine::Paused);
        rig.pull(4800);
        const qint64 paused = rig.sink->frames();
        const qint64 ramp = paused - 24000;
        rig.pull(4800); // nothing more while paused
        QCOMPARE(rig.sink->frames(), paused);
        const qint64 position = rig.engine->position();
        rig.engine->play();
        rig.pull(4800);
        const QVector<float> &out = rig.sink->data();
        const double step = maxDelta(out, 0, out.size() / 2);
        const double natural = 0.5 * 2 * M_PI * 1000 / 48000;
        qInfo("MEASURE pause ramp %lld frames; largest step %.4f (a 1 kHz tone's own %.4f); last paused sample %.5f",
              ramp, step, natural, double(out[paused * 2 - 2]));
        QVERIFY(qAbs(ramp - rig.engine->pauseRampFrames()) <= 1);
        QVERIFY(qAbs(out[paused * 2 - 2]) < 0.01);
        QVERIFY(step < natural * 1.05);
        QCOMPARE(position, qint64((24000 + ramp) * 1000 / 48000));
    }

    void seekWhilePlayingDoesNotClick() {
        Rig rig;
        rig.engine->load(fx("a.flac"));
        rig.engine->play();
        rig.pull(10000);
        rig.engine->seek(2000);
        rig.pull(10000);
        const double step = maxDelta(rig.sink->data(), 0, rig.sink->data().size() / 2);
        qInfo("MEASURE largest step around a seek %.4f", step);
        QVERIFY(step < 0.1);
    }

    void unreadableFileIsReported() {
        Rig rig;
        QSignalSpy failed(rig.engine.get(), &AudioEngine::trackFailed);
        rig.engine->load(fx("broken.mp3"));
        QTRY_COMPARE_WITH_TIMEOUT((rig.engine->poll(), failed.count()), 1, 5000);
        QCOMPARE(failed.at(0).at(0).toString(), fx("broken.mp3"));
        QCOMPARE(rig.engine->state(), AudioEngine::Stopped);
    }

    void metadataPositionAndFormat() {
        Rig rig;
        QSignalSpy format(rig.engine.get(), &AudioEngine::formatChanged);
        rig.engine->load(fx("rs1.flac"));
        QTRY_VERIFY_WITH_TIMEOUT((rig.engine->poll(), format.count() == 1), 5000);
        QCOMPARE(format.at(0).at(0).toInt(), 44100);
        QCOMPARE(format.at(0).at(1).toInt(), 2);
        QVERIFY(rig.engine->duration() > 3000);
        QSignalSpy tapped(rig.engine.get(), &AudioEngine::pcmTapped);
        rig.engine->play();
        rig.pull(48000);
        QCOMPARE(rig.engine->position(), qint64(1000));
        rig.engine->poll();
        QVERIFY(tapped.count() > 0);
        rig.engine->stop();
        QCOMPARE(rig.engine->position(), qint64(0));
        QCOMPARE(rig.engine->state(), AudioEngine::Stopped);
    }

    // The Player on the native engine: plays through, skips a broken file
    // and stops at the end, with the sink pulling 20× faster than real time.
    void playerSkipsBrokenFiles() {
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        auto *engine = new NativeEngine(sink);
        QCOMPARE(engine->sink()->kind(), QString("buffer"));
        Player player(engine, QString());
        QCOMPARE(player.engineName(), QString("native"));
        QVERIFY(player.gapless());
        player.addPaths({fx("short1.flac"), fx("broken.mp3"), fx("short2.flac")});
        player.playIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 2, 5000);
        QVERIFY(player.error().contains("broken.mp3"));
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 5000);
        QCOMPARE(player.currentIndex(), 2);
    }

    void playerAdvancesGaplessly() {
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        Player player(new NativeEngine(sink), QString());
        QSignalSpy changed(&player, &Player::trackChanged);
        player.addPaths({fx("short1.flac"), fx("short2.flac")});
        player.playIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("short2"), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 5000);
        QCOMPARE(sink->pulledFrames(), qint64(96000));
    }

    void playerKeepsPlaybackSettings() {
        QTemporaryDir dir;
        const QString state = dir.filePath("state.json");
        {
            Player player(new NativeEngine(new BufferSink), state);
            player.setCrossfadeSeconds(4.5);
            player.setReplayGainMode("album");
            player.setReplayGainPreamp(3);
            player.setEqEnabled(true);
            QVERIFY(player.applyEqPreset("Rock"));
            player.setEqBand(9, 20); // clamped
            QVERIFY(!player.applyEqPreset("No such preset"));
            QCOMPARE(player.eqBands().at(9), 12.0);
        }
        Player again(new NativeEngine(new BufferSink), state);
        QCOMPARE(again.crossfadeSeconds(), 4.5);
        QCOMPARE(again.replayGainMode(), QString("album"));
        QCOMPARE(again.replayGainPreamp(), 3.0);
        QVERIFY(again.eqEnabled());
        QCOMPARE(again.eqBands().at(0), 4.0);
        QCOMPARE(again.eqFrequencies().size(), 10);
        QVERIFY(again.eqPresets().contains("Rock"));
    }

    void engineIsChosenBySetting() {
        QTemporaryDir dir;
        const QString state = dir.filePath("state.json");
        {
            Player player(state);
            QCOMPARE(player.engineName(), QString("qt"));
        }
        QSettings(dir.filePath("thrum.conf"), QSettings::IniFormat).setValue("playback/engine", "native");
        {
            Player player(state);
            QCOMPARE(player.engineName(), QString("native"));
            QCOMPARE(static_cast<NativeEngine *>(player.engine())->sink()->kind(), QString("buffer"));
        }
        qputenv("THRUM_ENGINE", "qt");
        {
            Player player(state);
            QCOMPARE(player.engineName(), QString("qt"));
        }
        qunsetenv("THRUM_ENGINE");
    }

    // CPU for three minutes of FLAC, decoded, levelled, equalised and
    // resampled as fast as possible. Off unless THRUM_ENGINE_BENCH=1 (it
    // takes a few seconds); the numbers go in docs/plan/progress.md.
    void benchmarkCpu_data() {
        QTest::addColumn<int>("sinkRate");
        QTest::addColumn<bool>("eq");
        QTest::newRow("44.1k sink, EQ off") << 44100 << false;
        QTest::newRow("44.1k sink, EQ Rock") << 44100 << true;
        QTest::newRow("48k sink (resampling), EQ Rock") << 48000 << true;
    }

    void benchmarkCpu() {
        if (!qEnvironmentVariableIsSet("THRUM_ENGINE_BENCH"))
            QSKIP("set THRUM_ENGINE_BENCH=1 to measure");
        QFETCH(int, sinkRate);
        QFETCH(bool, eq);
        const QString music = m_dir.filePath("bench.flac");
        if (!QFile::exists(music)) {
            // Noise under a chord, so FLAC has real work to do.
            QProcess ff;
            ff.start("ffmpeg", {"-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i",
                                "anoisesrc=color=pink:amplitude=0.2:sample_rate=44100:duration=180", "-f", "lavfi",
                                "-i", "aevalsrc=0.2*sin(2*PI*220*t)+0.1*sin(2*PI*277*t)|0.2*sin(2*PI*330*t):s=44100:d=180",
                                "-filter_complex", "[0][1]amix=inputs=2:normalize=0,aformat=channel_layouts=stereo",
                                "-c:a", "flac", "-sample_fmt", "s16", music});
            QVERIFY(ff.waitForFinished(120000) && ff.exitCode() == 0);
        }
        auto *sink = new BufferSink(sinkRate, false);
        NativeEngine engine(sink);
        AudioEngine::Settings s;
        s.eqEnabled = eq;
        s.eqBandsDb = Equalizer::preset("Rock");
        s.replayGain = AudioEngine::ReplayGainMode::Track;
        engine.setSettings(s);
        engine.load(music);
        engine.play();
        timespec t0, t1;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0);
        QElapsedTimer wall;
        wall.start();
        bool ended = false;
        connect(&engine, &AudioEngine::endOfQueue, this, [&] { ended = true; });
        while (!ended && wall.elapsed() < 120000) {
            if (sink->pull(8192) == 0)
                QThread::usleep(100);
            engine.poll();
        }
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
        const double cpu = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        qInfo("MEASURE cpu %s: %lld frames, %.3f s CPU, %.3f s wall for 180 s of audio = %.2f%% of one core",
              QTest::currentDataTag(), sink->pulledFrames(), cpu, wall.elapsed() / 1000.0, cpu / 180 * 100);
        QVERIFY(ended);
    }

    // Ten seconds in real time, pulled every 10 ms like a device, with the
    // engine's timers and threads waking as they would while playing.
    void benchmarkRealtimeCpu() {
        if (!qEnvironmentVariableIsSet("THRUM_ENGINE_BENCH"))
            QSKIP("set THRUM_ENGINE_BENCH=1 to measure");
        const QString music = m_dir.filePath("bench.flac");
        QVERIFY(QFile::exists(music)); // made by benchmarkCpu
        auto *sink = new BufferSink(48000, false);
        NativeEngine engine(sink);
        AudioEngine::Settings s;
        s.eqEnabled = true;
        s.eqBandsDb = Equalizer::preset("Rock");
        engine.setSettings(s);
        engine.load(music);
        engine.play();
        sink->setAutoPull(1.0);
        QTest::qWait(500);
        timespec t0, t1;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0);
        QTest::qWait(10000);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
        const double cpu = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        qInfo("MEASURE realtime cpu (48k, EQ Rock, resampling): %.3f s CPU in 10 s = %.2f%% of one core; position %lld ms",
              cpu, cpu / 10 * 100, engine.position());
        QVERIFY(engine.position() > 9000);
    }

    // The Qt engine follows the default output only while it plays or is
    // paused, and moves its QAudioOutput when told to.
    void qtEngineFollowsWhilePlaying() {
        QtEngine engine(false);
        auto *source = new FakeDefaultSource;
        auto *outputs = new FakeOutputList;
        source->name = QStringLiteral("jabra");
        outputs->ids = {"jabra", "arctis"};
        auto *follower = new OutputFollower(outputs, source);
        engine.setOutputFollower(follower);
        QCOMPARE(engine.outputFollower(), follower);
        QVERIFY(!follower->active());
        QCOMPARE(source->starts, 0);
        engine.load(fx("short1.flac"));
        engine.play();
        QTRY_VERIFY_WITH_TIMEOUT(follower->active(), 3000);
        QCOMPARE(source->starts, 1);
        engine.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!follower->active(), 3000);
        QVERIFY(!source->live);
    }

    // The same interface on QMediaPlayer, with no audio output: it plays in
    // real time, so these files are short.
    void qtEngineThroughTheInterface() {
        QtEngine engine(false);
        QVERIFY(!engine.hasDevice());
        QSignalSpy advanced(&engine, &AudioEngine::advanced);
        QSignalSpy ended(&engine, &AudioEngine::endOfQueue);
        QSignalSpy tapped(&engine, &AudioEngine::pcmTapped);
        QSignalSpy format(&engine, &AudioEngine::formatChanged);
        engine.load(fx("short1.flac"));
        QCOMPARE(engine.source(), fx("short1.flac"));
        QCOMPARE(engine.state(), AudioEngine::Stopped);
        engine.queueNext(fx("short2.flac"));
        engine.play();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), AudioEngine::Playing, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(advanced.count(), 1, 8000);
        QCOMPARE(engine.source(), fx("short2.flac"));
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 8000);
        QVERIFY(tapped.count() > 0);
        QVERIFY(format.count() > 0);
        QCOMPARE(format.first().at(0).toInt(), 48000);
        QSignalSpy failed(&engine, &AudioEngine::trackFailed);
        engine.load(fx("broken.mp3"));
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 5000);
    }
};

// Following the desktop's default output (outputfollower.h), with a fake
// sound server and a fake Qt device list. The ids are sink names, as Qt's
// PipeWire and PulseAudio backends report them.
class OutputFollowerTests : public QObject {
    Q_OBJECT

    struct Rig {
        FakeDefaultSource *source = new FakeDefaultSource;
        FakeOutputList *outputs = new FakeOutputList;
        std::unique_ptr<OutputFollower> follower;
        std::unique_ptr<QSignalSpy> switched;
        Rig(const QList<QByteArray> &ids, const QString &serverDefault, const QByteArray &qtDefault,
            const QByteArray &current) {
            outputs->ids = ids;
            outputs->qtDefault = qtDefault;
            source->name = serverDefault;
            follower = std::make_unique<OutputFollower>(outputs, source);
            OutputFollower::Timing timing;
            timing.debounceMs = 10;
            timing.secondMs = 60;
            timing.pollMs = 40;
            timing.retryMs = {30, 30, 30};
            timing.restartMinMs = 50;
            timing.restartMaxMs = 200;
            follower->setTiming(timing);
            follower->setCurrent(current);
            switched = std::make_unique<QSignalSpy>(follower.get(), &OutputFollower::switchTo);
            // The engine's side: it reopens and reports the new device.
            QObject::connect(follower.get(), &OutputFollower::switchTo, follower.get(),
                             [f = follower.get()](const QByteArray &id) { f->setCurrent(id); });
        }
        QByteArray last() const { return switched->isEmpty() ? QByteArray() : switched->last().at(0).toByteArray(); }
    };

private slots:
    // Matt's evening: the Jabra dongle (the default) drops off, the server
    // falls back to the Arctis, and the stream follows.
    void defaultDisappearsFallsBack() {
        Rig rig({"jabra", "arctis"}, "jabra", "jabra", "jabra");
        rig.follower->setActive(true);
        QCOMPARE(rig.switched->count(), 0);
        rig.source->name = QStringLiteral("arctis");
        rig.outputs->set({"arctis"}, "arctis");
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("arctis"));
        QCOMPARE(rig.follower->current(), QByteArray("arctis"));
    }

    // The dongle comes back and is the default again, but Qt's default is
    // still the Arctis (the bug). The server's answer wins.
    void defaultReturnsMovesBack() {
        Rig rig({"arctis"}, "arctis", "arctis", "arctis");
        rig.follower->setActive(true);
        rig.source->name = QStringLiteral("jabra");
        rig.outputs->set({"arctis", "jabra"}, "arctis");
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("jabra"));
        // The second look a moment later finds nothing more to do.
        QTest::qWait(150);
        QCOMPARE(rig.switched->count(), 1);
    }

    // Without a sound server to ask, Qt's default is used, as before.
    void withoutTheServerQtDecides() {
        Rig rig({"jabra", "arctis"}, QString(), "jabra", "jabra");
        rig.follower->setActive(true);
        rig.outputs->set({"arctis"}, "arctis");
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("arctis"));
        // Nothing listed at all: stay put rather than switch to nothing.
        Rig empty({}, QString(), QByteArray(), "x");
        QCOMPARE(empty.follower->resolve(), QByteArray());
    }

    // Qt's list doesn't have the default yet; asking Qt again does.
    void staleListIsRefreshed() {
        Rig rig({"arctis"}, "jabra", "arctis", "arctis");
        rig.outputs->afterRefresh = {"arctis", "jabra"};
        QCOMPARE(rig.follower->resolve(), QByteArray("jabra"));
        QCOMPARE(rig.outputs->refreshes, 1);
        // A fresh list isn't asked again.
        QCOMPARE(rig.follower->resolve(), QByteArray("jabra"));
        QCOMPARE(rig.outputs->refreshes, 1);
    }

    // Qt is still behind after refreshing: stay on the device that is
    // heard, and retry until Qt lists the default, with no signal from Qt.
    void staleListIsRetried() {
        Rig rig({"arctis"}, "jabra", "arctis", "arctis");
        rig.follower->setActive(true);
        QCOMPARE(rig.switched->count(), 0);
        QVERIFY(rig.outputs->refreshes >= 1);
        rig.outputs->ids = {"arctis", "jabra"};
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("jabra"));
        // The current device has gone too while Qt lags: Qt's default, if
        // it lists it, else anything listed.
        Rig lost({"hdmi"}, "jabra", "hdmi", "arctis");
        QCOMPARE(lost.follower->resolve(), QByteArray("hdmi"));
        Rig lost2({"hdmi", "speakers"}, "jabra", "arctis", "arctis");
        QCOMPARE(lost2.follower->resolve(), QByteArray("hdmi"));
    }

    // A new default with the same set of devices: only the server says so.
    void defaultChangeWithoutListChange() {
        Rig rig({"jabra", "arctis"}, "jabra", "jabra", "jabra");
        rig.follower->setActive(true);
        rig.source->name = QStringLiteral("arctis");
        rig.source->announce();
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("arctis"));
    }

    // The stream failed on its device (Qt marks it on none): the next check
    // reopens it, even on the same device.
    void lostStreamIsReopened() {
        Rig rig({"jabra", "arctis"}, "jabra", "jabra", "jabra");
        rig.follower->setActive(true);
        rig.follower->setCurrent({});
        rig.follower->checkSoon();
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("jabra"));
    }

    // pactl dies: it is started again with a back-off, and polled meanwhile.
    void subscriptionRestarts() {
        Rig rig({"jabra", "arctis"}, "jabra", "jabra", "jabra");
        rig.follower->setActive(true);
        QCOMPARE(rig.source->starts, 1);
        QVERIFY(rig.source->live);
        rig.source->startFails = true;
        rig.source->die();
        // While it is down, a change is still noticed, by polling.
        rig.source->name = QStringLiteral("arctis");
        QTRY_COMPARE(rig.switched->count(), 1);
        QCOMPARE(rig.last(), QByteArray("arctis"));
        QTRY_COMPARE(rig.source->starts, 2);
        // A start that doesn't stick leaves polling on; the next stop backs
        // off further.
        rig.source->die();
        QTRY_COMPARE_WITH_TIMEOUT(rig.source->starts, 3, 2000);
        rig.source->startFails = false;
        rig.source->die();
        QTRY_COMPARE_WITH_TIMEOUT(rig.source->starts, 4, 2000);
        QVERIFY(rig.source->live);
    }

    // An idle player runs no pactl and switches nothing.
    void inactiveDoesNothing() {
        Rig rig({"jabra", "arctis"}, "jabra", "jabra", "jabra");
        rig.follower->setActive(true);
        rig.follower->setActive(false);
        QVERIFY(!rig.source->live);
        const int queries = rig.source->queries;
        rig.source->name = QStringLiteral("arctis");
        rig.outputs->set({"arctis"}, "arctis");
        rig.source->die();
        QTest::qWait(200);
        QCOMPARE(rig.switched->count(), 0);
        QCOMPARE(rig.source->queries, queries);
        QCOMPARE(rig.source->starts, 1);
    }

    void pactlLinesAreClassified() {
        using E = PactlDefaultSource::Event;
        QCOMPARE(PactlDefaultSource::classify("Event 'change' on server #-1"), E::Server);
        QCOMPARE(PactlDefaultSource::classify("Event 'change' on server"), E::Server);
        QCOMPARE(PactlDefaultSource::classify("Event 'new' on sink #2194460"), E::Sinks);
        QCOMPARE(PactlDefaultSource::classify("Event 'remove' on sink #185"), E::Sinks);
        QCOMPARE(PactlDefaultSource::classify("Event 'change' on sink #185"), E::None);
        QCOMPARE(PactlDefaultSource::classify("Event 'new' on sink-input #12"), E::None);
        QCOMPARE(PactlDefaultSource::classify("Event 'remove' on client #2204751"), E::None);
        QCOMPARE(PactlDefaultSource::classify("garbage"), E::None);
    }
};

int runEngineTests(int argc, char **argv) {
    EngineTests tests;
    OutputFollowerTests follower;
    return QTest::qExec(&tests, argc, argv) + QTest::qExec(&follower, argc, argv);
}

#include "engine_tests.moc"
