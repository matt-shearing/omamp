// Spotify played by Thrum itself: the sidecar client (SpotifyAudio), a
// Spotify track through the native engine, and through a native engine of
// its own beside an engine that can't open it (EnginePlayback); Like and
// "playing now" following Thrum's own track; whole playlists going into Now
// Playing as their tracks; one session for all of it; and Thrum as a
// Spotify Connect device. The sidecar is tests/fake-spotify/
// thrum-spotify-audio, which "decodes" every track to a tone, answers the
// catalogue from recorded JSON and never reaches Spotify.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include "spotifycontroller.h"

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

QString readFile(const QString &file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// The window's arrangement: the fake spotify_player and the fake sidecar,
// one SpotifyAudio installed (the session the engines and the controller
// share), the controller switched on, and a Player whose native engine
// plays Spotify itself.
struct Thrum {
    FakeSpotify fake;
    AudioWorld world;
    QTemporaryDir dir;
    std::shared_ptr<SpotifyAudio> audio;
    std::unique_ptr<SpotifyController> c;
    std::unique_ptr<Player> player;
    Thrum() {
        fake.set("THRUM_SPOTIFY_SESSION", fakeSidecar());
        audio = world.install();
        c = std::make_unique<SpotifyController>(dir.filePath("spotify.conf"), dir.filePath("art"));
        c->setEnabled(true);
        player = std::make_unique<Player>(new HandoffEngine(realTimeEngine(), c->playback()), QString(), asItIs);
        c->setPlayer(player.get());
    }
    ~Thrum() {
        player.reset();
        c.reset();
        SpotifyAudio::install(nullptr);
        audio.reset();
    }
    QString currentPath() const {
        const int cur = player->currentIndex();
        return cur >= 0 ? player->playlist()->at(cur).path : QString();
    }
    // The fake's Connect events, as the phone would cause them.
    void phone(const QList<QJsonObject> &events) {
        QByteArray lines;
        for (const QJsonObject &e : events)
            lines += QJsonDocument(e).toJson(QJsonDocument::Compact) + '\n';
        world.write("connect-in", lines);
    }
    QStringList connectCalls(const QString &action) const {
        QStringList out;
        for (const QString &line : world.calls())
            if (line.startsWith("connect ") && line.endsWith(" " + action))
                out << line;
        return out;
    }
};

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

    // Like acts on the Spotify track Thrum plays itself, by its id, through
    // the session: never spotify_player's `like`, which can only save what
    // spotify_player plays. The "playing now" line shows Thrum's track.
    void likeFollowsThrumsOwnTrack() {
        Thrum t;
        t.world.write("duration-" + kTrackA, "30000");
        t.world.write("liked", "");
        t.player->playTracks({{"spotify://track/" + kTrackA, "Tone A", "Fake", 30000, 0}}, 0);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("playing"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(t.c->nowPlaying().value("thrum").toBool(), 3000);
        QVariantMap now = t.c->nowPlaying();
        QCOMPARE(now.value("device").toString(), QString("Thrum"));
        QCOMPARE(now.value("uri").toString(), "spotify:track:" + kTrackA);
        QVERIFY(now.value("playing").toBool());
        // Whether it is liked is asked of the session, once.
        QTRY_VERIFY_WITH_TIMEOUT(t.c->nowPlaying().value("likedKnown").toBool(), 3000);
        QVERIFY(!t.c->nowPlaying().value("liked").toBool());
        QVERIFY(t.fake.calls().contains("session liked " + kTrackA));

        t.c->setPlayingLiked(true);
        QTRY_VERIFY_WITH_TIMEOUT(readFile(t.world.dir.filePath("liked")).contains(kTrackA), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(t.c->nowPlaying().value("liked").toBool(), 3000);
        t.c->setPlayingLiked(false);
        QTRY_VERIFY_WITH_TIMEOUT(!readFile(t.world.dir.filePath("liked")).contains(kTrackA), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!t.c->nowPlaying().value("liked").toBool(), 3000);
        const QStringList cliLikes = t.fake.calls().filter(QRegularExpression("^like"));
        QVERIFY2(cliLikes.isEmpty(), qPrintable(cliLikes.join(" | ")));

        // Paused, it says so; stopped, Thrum's track is no longer shown.
        t.player->pause();
        QTRY_VERIFY_WITH_TIMEOUT(!t.c->nowPlaying().value("playing").toBool(), 3000);
        QVERIFY(t.c->nowPlaying().value("thrum").toBool());
        t.player->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!t.c->nowPlaying().value("thrum").toBool(), 3000);
    }

    // The catalogue, Liked Songs and the sound come from one sidecar, so one
    // Spotify session.
    void oneSessionForEverything() {
        Thrum t;
        QTRY_VERIFY_WITH_TIMEOUT(t.c->running() && !t.c->checking(), 8000);
        t.c->setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(t.c->items()->count(), 5, 8000);
        t.c->openRow(3);
        QTRY_COMPARE_WITH_TIMEOUT(t.c->items()->count(), 3, 5000);
        QVERIFY(t.fake.calls().contains("session playlist " + t.fake.id("playlist:Synthwave Essentials")));
        t.player->playTracks({{"spotify://track/" + kTrackA, "Tone A", "Fake", 3000, 0}}, 0);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("playing"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(t.c->nowPlaying().value("likedKnown").toBool(), 3000);
        QCOMPARE(t.world.pids().size(), 1);
        QCOMPARE(t.audio->starts(), 1);
        QCOMPARE(t.c->sessionReader()->audio(), t.audio.get());
    }

    // A whole playlist goes into Now Playing as its tracks, which Thrum
    // plays; so does a playlist entry already in the queue when its turn
    // comes. spotify_player plays none of it.
    void wholePlaylistsPlayAsTheirTracks() {
        Thrum t;
        const QString synth = t.fake.id("playlist:Synthwave Essentials");
        QVERIFY(t.c->expandsContexts());
        QTRY_VERIFY_WITH_TIMEOUT(t.c->running() && !t.c->checking(), 8000);
        t.c->setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(t.c->items()->count(), 5, 8000);
        t.c->openRow(3);
        QTRY_COMPARE_WITH_TIMEOUT(t.c->items()->count(), 3, 5000);
        t.fake.clearCalls();
        t.c->playView(false);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->playlist()->count(), 3, 5000);
        for (int r = 0; r < 3; ++r)
            QVERIFY(t.player->playlist()->at(r).path.startsWith("spotify://track/"));
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("playing"), 5000);
        QVERIFY(!t.player->playsElsewhere());

        // An entry for the whole playlist, as an older Thrum saved it.
        t.player->stop();
        t.player->playTracks({{"/music/first.flac", "First", "Me", 1000, 0},
                              {"spotify://playlist/" + synth, "Synthwave Essentials", "Spotify", 0, 0}},
                             1);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->playlist()->count(), 4, 5000);
        QCOMPARE(t.player->playlist()->at(0).path, QString("/music/first.flac"));
        QCOMPARE(t.player->currentIndex(), 1);
        QVERIFY(t.currentPath().startsWith("spotify://track/"));
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("playing"), 5000);
        t.c->waitForIdle();
        QVERIFY2(t.fake.calls().filter("playback start").isEmpty(), qPrintable(t.fake.calls().join(" | ")));
    }

    // Thrum as a Spotify Connect device: a phone playing on it is heard
    // through Thrum's engine as one live entry that shows the phone's
    // track; Thrum's pause, play and next reach the phone; the phone's pause
    // reaches Thrum; and moving on in Thrum takes the entry away.
    void aPhonePlaysOnThrum() {
        Thrum t;
        QCOMPARE(t.c->connectState(), QString("off"));
        const QString wav = writeWav(t.dir.filePath("local.wav"), 30000);
        t.player->appendTracks({{wav, "Local", "Me", 30000, 0}});
        t.c->setConnectDevice(true);
        QCOMPARE(t.c->connectName(), QString("Thrum"));
        // It starts at once, as a device called Thrum.
        QTRY_COMPARE_WITH_TIMEOUT(t.c->connectState(), QString("ready"), 5000);
        QVERIFY2(readFile(t.world.dir.filePath("args")).contains("--connect Thrum serve"),
                 qPrintable(readFile(t.world.dir.filePath("args"))));

        const QJsonObject track{{"id", kTrackB},          {"title", "Phone Song"}, {"artists", QJsonArray{"Phone Artist"}},
                                {"album", "Pocket"},      {"duration_ms", 200000}, {"art", ""}};
        t.phone({QJsonObject{{"client", "Sam's phone"}, {"track", track}},
                 QJsonObject{{"state", "playing"}, {"position_ms", 0}}});
        QTRY_COMPARE_WITH_TIMEOUT(t.currentPath(), SpotifyAudio::connectPath(), 5000);
        QCOMPARE(t.player->currentIndex(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("playing"), 5000);
        QVERIFY(t.player->live());
        QTRY_COMPARE_WITH_TIMEOUT(t.player->title(), QString("Phone Song"), 3000);
        QCOMPARE(t.player->artist(), QString("Phone Artist"));
        QTRY_VERIFY_WITH_TIMEOUT(loudest(t.player->levels()) > 0.3, 3000);
        QCOMPARE(t.c->connectState(), QString("playing"));
        QVariantMap now = t.c->nowPlaying();
        QCOMPARE(now.value("title").toString(), QString("Phone Song"));
        QCOMPARE(now.value("device").toString(), QString("Thrum, from Sam's phone"));
        QCOMPARE(now.value("uri").toString(), "spotify:track:" + kTrackB);

        // Thrum's buttons, as the phone's.
        t.player->pause();
        QTRY_VERIFY2_WITH_TIMEOUT(!t.connectCalls("pause").isEmpty(), qPrintable(t.world.calls().join(" | ")), 3000);
        t.player->play();
        QTRY_VERIFY_WITH_TIMEOUT(!t.connectCalls("play").isEmpty(), 3000);
        t.player->next();
        QTRY_VERIFY_WITH_TIMEOUT(!t.connectCalls("next").isEmpty(), 3000);
        QCOMPARE(t.currentPath(), SpotifyAudio::connectPath());
        // Like saves the phone's track.
        t.c->setPlayingLiked(true);
        QTRY_VERIFY_WITH_TIMEOUT(readFile(t.world.dir.filePath("liked")).contains(kTrackB), 3000);

        // The phone pauses: so does Thrum, without telling the phone back.
        const int pausesBefore = t.connectCalls("pause").size();
        t.phone({QJsonObject{{"state", "paused"}, {"position_ms", 61000}}});
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("paused"), 3000);
        QTest::qWait(200);
        QCOMPARE(t.connectCalls("pause").size(), pausesBefore);

        // Thrum moves on to its own music: the entry goes.
        t.player->playIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->playlist()->count(), 1, 3000);
        QCOMPARE(t.currentPath(), wav);

        t.c->setConnectDevice(false);
        QCOMPARE(t.c->connectState(), QString("off"));
        QVERIFY(!Spotify::Settings::load(t.dir.filePath("spotify.conf")).connectDevice);
    }

    // A Connect device that can't start (no Premium, say) is reported, and
    // the session starts again without it, so Spotify tracks still play.
    void aConnectDeviceThatFailsLeavesPlayingAlone() {
        Thrum t;
        t.world.write("refuse-connect", "Spotify Connect needs Premium");
        t.c->setConnectDevice(true);
        QTRY_COMPARE_WITH_TIMEOUT(t.c->connectState(), QString("failed"), 5000);
        QVERIFY2(t.c->connectError().contains("Premium"), qPrintable(t.c->connectError()));
        t.player->playTracks({{"spotify://track/" + kTrackA, "Tone A", "Fake", 3000, 0}}, 0);
        QTRY_COMPARE_WITH_TIMEOUT(t.player->state(), QString("playing"), 5000);
        const QStringList starts = readFile(t.world.dir.filePath("args")).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(starts.size(), 2);
        QVERIFY(starts.first().contains("--connect"));
        QVERIFY(!starts.last().contains("--connect"));
    }
};

int runSpotifyAudioTests(int argc, char **argv) {
    SpotifyAudioTests t;
    return QTest::qExec(&t, argc, argv);
}

#include "spotifyaudio_tests.moc"
