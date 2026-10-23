// Spotify played by Thrum itself: the sidecar client (SpotifyAudio), a
// Spotify track through the native engine, and through a native engine of
// its own beside an engine that can't open it (EnginePlayback). The sidecar
// is tests/fake-spotify/thrum-spotify-audio, which "decodes" every track to
// a tone and never reaches Spotify.
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <memory>

#include "handoffengine.h"
#include "nativeengine.h"
#include "pcmsink.h"
#include "player.h"
#include "spotify_fakes.h"
#include "spotifyaudio.h"

namespace {

const QString kTrackA = "AAAAAAAAAAAAAAAAAAAAAA";
const QString kTrackB = "BBBBBBBBBBBBBBBBBBBBBB";

QString fakeSidecar() { return QString(THRUM_TESTS_DIR) + "/fake-spotify/thrum-spotify-audio"; }

bool alive(pid_t pid) { return pid > 0 && ::kill(pid, 0) == 0; }

// The fake's state folder, in the environment while the test runs, and the
// installed SpotifyAudio taken away again at the end.
struct AudioWorld {
    QTemporaryDir dir;
    QByteArray saved;
    explicit AudioWorld() {
        saved = qgetenv("FAKE_SPOTIFY_AUDIO_DIR");
        qputenv("FAKE_SPOTIFY_AUDIO_DIR", dir.path().toUtf8());
    }
    ~AudioWorld() {
        SpotifyAudio::install(nullptr);
        qputenv("FAKE_SPOTIFY_AUDIO_DIR", saved);
    }
    void write(const QString &name, const QByteArray &data = {}) {
        QFile f(dir.filePath(name));
        f.open(QIODevice::WriteOnly);
        f.write(data);
    }
    QList<pid_t> pids() const {
        QFile f(dir.filePath("pids"));
        QList<pid_t> out;
        if (f.open(QIODevice::ReadOnly))
            for (const QByteArray &line : f.readAll().split('\n'))
                if (!line.trimmed().isEmpty())
                    out << pid_t(line.trimmed().toInt());
        return out;
    }
    QStringList calls() const {
        QFile f(dir.filePath("calls.log"));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    }
    std::shared_ptr<SpotifyAudio> install() {
        auto audio = std::make_shared<SpotifyAudio>(fakeSidecar(), dir.filePath("credentials.json"));
        SpotifyAudio::install(audio);
        return audio;
    }
};

// A second of a 1 kHz tone as a 16-bit stereo WAV: a local file for mixed
// queues.
QString writeWav(const QString &file, int ms = 1000) {
    const int rate = 44100, frames = rate * ms / 1000;
    QByteArray pcm;
    for (int i = 0; i < frames; ++i) {
        const qint16 v = qint16(8000 * std::sin(2 * M_PI * 1000 * i / rate));
        for (int c = 0; c < 2; ++c)
            pcm.append(reinterpret_cast<const char *>(&v), 2);
    }
    QByteArray h;
    auto u32 = [&](quint32 v) { h.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](quint16 v) { h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + pcm.size());
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    h.append("data");
    u32(pcm.size());
    QFile f(file);
    f.open(QIODevice::WriteOnly);
    f.write(h + pcm);
    return file;
}

qreal loudest(const QList<qreal> &levels) {
    qreal m = 0;
    for (qreal v : levels)
        m = qMax(m, v);
    return m;
}

// The app's resolver hands a Spotify reference over as it is.
QUrl asItIs(const QString &path, qint64, QString *) { return QUrl(path); }

NativeEngine *realTimeEngine() {
    auto *sink = new BufferSink(48000, false);
    sink->setAutoPull(1.0);
    return new NativeEngine(sink);
}

} // namespace

class SpotifyAudioTests : public QObject {
    Q_OBJECT

private slots:
    void init() {
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("python3 isn't installed, and the fake sidecar needs it");
    }

    // One sidecar for every request, started when first asked; quitting
    // (or destroying the client) leaves no process behind.
    void sidecarStartsOnceAndLeavesNoChild() {
        AudioWorld world;
        pid_t pid = 0;
        {
            SpotifyAudio audio(fakeSidecar(), world.dir.filePath("credentials.json"));
            QCOMPARE(audio.starts(), 0);
            QVERIFY(audio.ask({{"cmd", "ping"}}, 5000).ok());
            QVERIFY(audio.ask({{"cmd", "ping"}}, 5000).ok());
            QCOMPARE(audio.starts(), 1);
            pid = audio.pid();
            QVERIFY(alive(pid));
            QCOMPARE(world.pids(), QList<pid_t>{pid});
            // An unknown stream is an answer, not a hang.
            const SpotifyAudio::Answer a = audio.ask({{"cmd", "read"}, {"stream", 99}, {"frames", 10}}, 5000);
            QVERIFY(!a.ok());
            QCOMPARE(a.error(), QString("no such stream"));
        }
        // The destructor quit it and reaped it.
        QVERIFY(!alive(pid));

        SpotifyAudio audio(fakeSidecar(), world.dir.filePath("credentials.json"));
        QVERIFY(audio.ask({{"cmd", "ping"}}, 5000).ok());
        const pid_t second = audio.pid();
        QVERIFY(alive(second));
        audio.shutdown();
        QVERIFY(!alive(second));
        QCOMPARE(audio.pid(), 0);
        // Nothing starts after shutdown.
        QVERIFY(!audio.ask({{"cmd", "ping"}}, 1000).ok());
        QCOMPARE(audio.starts(), 1);
    }

    // A sign-in the sidecar can't make is reported, and not tried again
    // for every track that follows.
    void refusedSignInIsRemembered() {
        AudioWorld world;
        world.write("refuse", "can't read the Spotify sign-in");
        SpotifyAudio audio(fakeSidecar(), world.dir.filePath("credentials.json"));
        SpotifyAudio::Answer a = audio.ask({{"cmd", "ping"}}, 5000);
        QVERIFY(!a.ok());
        QVERIFY2(a.error().contains("sign-in"), qPrintable(a.error()));
        a = audio.ask({{"cmd", "ping"}}, 5000);
        QVERIFY(!a.ok());
        QCOMPARE(audio.starts(), 1);
    }

    void trackIdsAndRouting() {
        QCOMPARE(SpotifyAudio::trackId("spotify://track/" + kTrackA), kTrackA);
        QCOMPARE(SpotifyAudio::trackId("spotify://track/" + kTrackA + "?x=1"), kTrackA);
        QVERIFY(SpotifyAudio::trackId("spotify://playlist/" + kTrackA).isEmpty());
        QVERIFY(SpotifyAudio::trackId("/music/a.flac").isEmpty());
        AudioWorld world;
        // Without the sidecar installed, the engines leave Spotify alone.
        QVERIFY(!SpotifyAudio::canPlay("spotify://track/" + kTrackA));
        NativeEngine engine(new BufferSink);
        QVERIFY(!engine.canPlay("spotify://track/" + kTrackA));
        auto audio = world.install();
        QVERIFY(engine.canPlay("spotify://track/" + kTrackA));
        // A whole playlist played as one stays with spotify_player.
        QVERIFY(!engine.canPlay("spotify://playlist/" + kTrackA));
        audio->setEnabled(false);
        QVERIFY(!engine.canPlay("spotify://track/" + kTrackA));
    }

    // The native engine plays a Spotify track like a file: the visualiser
    // moves, pausing lets it rest, seeking works, and at the end the next
    // entry (a local file) follows. Nothing is left running afterwards.
    void nativeEnginePlaysSpotify() {
        AudioWorld world;
        QTemporaryDir dir;
        world.write("duration-" + kTrackA, "4000");
        const QString wav = writeWav(dir.filePath("local.wav"));
        auto audio = world.install();
        {
            Player player(realTimeEngine(), QString(), asItIs);
            player.playTracks({{"spotify://track/" + kTrackA, "Tone A", "Fake", 4000, 0}, {wav, "Local", "Me", 1000, 0}},
                              0);
            QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("playing"), 5000);
            QVERIFY(!player.playsElsewhere());
            QTRY_VERIFY_WITH_TIMEOUT(loudest(player.levels()) > 0.3, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Tone AAAA"), 3000);
            QCOMPARE(player.sampleRate(), 44100);

            player.pause();
            QTRY_VERIFY_WITH_TIMEOUT(loudest(player.levels()) < 0.02, 3000);
            const qint64 at = player.position();
            QTest::qWait(300);
            QVERIFY(qAbs(player.position() - at) < 50);
            player.play();
            QTRY_VERIFY_WITH_TIMEOUT(loudest(player.levels()) > 0.3, 3000);

            player.seek(3000);
            QTRY_VERIFY_WITH_TIMEOUT(player.position() >= 3000, 2000);
            QTRY_VERIFY2_WITH_TIMEOUT(world.calls().contains("seek 1  3000"), qPrintable(world.calls().join(" | ")), 3000);

            // The end: the local file plays next, in the same engine.
            QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 5000);
        }
        // The engine let go of the stream.
        QTRY_VERIFY_WITH_TIMEOUT(world.calls().contains("close 1"), 2000);
        const QList<pid_t> pids = world.pids();
        QCOMPARE(pids.size(), 1);
        QVERIFY(alive(pids.first()));
        // Quitting: the installed sidecar goes with its last user.
        SpotifyAudio::install(nullptr);
        audio.reset();
        QVERIFY(!alive(pids.first()));
    }

    // Two Spotify tracks in a row join without stopping: the next one is
    // opened while the first plays.
    void spotifyTracksFollowEachOther() {
        AudioWorld world;
        world.write("duration-" + kTrackA, "1200");
        world.write("duration-" + kTrackB, "1200");
        world.write("hz-" + kTrackB, "880");
        auto audio = world.install();
        Player player(realTimeEngine(), QString(), asItIs);
        player.playTracks({{"spotify://track/" + kTrackA, "A", "Fake", 1200, 0},
                           {"spotify://track/" + kTrackB, "B", "Fake", 1200, 0}},
                          0);
        // B is opened ahead, on stream 2, while A still plays.
        QTRY_VERIFY_WITH_TIMEOUT(world.calls().contains("open 2 " + kTrackB + " 0"), 3000);
        QCOMPARE(player.currentIndex(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 4000);
        QCOMPARE(player.state(), QString("playing"));
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("stopped"), 4000);
        QCOMPARE(audio->starts(), 1);
    }

    // A Free account can't stream: the track fails with a message saying so.
    void freeAccountIsTold() {
        AudioWorld world;
        world.write("premium");
        auto audio = world.install();
        Player player(realTimeEngine(), QString(), asItIs);
        player.playTracks({{"spotify://track/" + kTrackA, "A", "Fake", 3000, 0}}, 0);
        QTRY_VERIFY2_WITH_TIMEOUT(player.error().contains("Premium"),
                                  qPrintable(player.error() + " | " + world.calls().join(" | ") + " | " + player.state()), 5000);
    }

    // Beside an engine that can't open Spotify (the Qt engine; here a fake),
    // a native engine of its own plays Spotify's tracks, and the queue moves
    // on to local files in the first engine.
    void sideEnginePlaysSpotifyBesideAnother() {
        AudioWorld world;
        world.write("duration-" + kTrackA, "1500");
        auto audio = world.install();
        auto *local = new FakeEngine;
        NativeEngine *side = nullptr;
        auto *playback = new EnginePlayback([&side] { return side = realTimeEngine(); });
        auto *own = new HandoffEngine(local, playback);
        playback->setParent(own);
        Player player(new HandoffEngine(own, nullptr), QString(), asItIs);
        QVERIFY(own->canPlay("spotify://track/" + kTrackA));
        QVERIFY(!local->canPlay("spotify://track/" + kTrackA));
        player.setVolume(50);
        player.playTracks({{"spotify://track/" + kTrackA, "A", "Fake", 1500, 0}, {"/music/b.flac", "B", "Me", 1000, 0}},
                          0);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("playing"), 5000);
        QVERIFY(side);
        QVERIFY(!player.playsElsewhere());
        QTRY_VERIFY_WITH_TIMEOUT(loudest(player.levels()) > 0.3, 3000);
        QVERIFY(local->source().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 5000);
        QCOMPARE(local->source(), QString("/music/b.flac"));
        QCOMPARE(local->state(), AudioEngine::Playing);
        QCOMPARE(side->state(), AudioEngine::Stopped);
    }
};

int runSpotifyAudioTests(int argc, char **argv) {
    SpotifyAudioTests t;
    return QTest::qExec(&t, argc, argv);
}

#include "spotifyaudio_tests.moc"
