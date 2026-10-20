// Spotify without Spotify: a fake spotify_player (tests/fake-spotify) on
// PATH answers from recorded JSON shaped exactly as spotify_player 0.25.1
// prints it (tests/fixtures/spotify, made by make-fixtures.py from the
// structs in its source). Nothing here contacts Spotify, starts the real
// spotify_player, Spotifast or Spotify, or plays a sound: Omamp's own
// engine is a fake that only keeps state.
//
// It covers the JSON shapes, the backends' guards and commands, the
// settings, `omamp spotify`, the palette and search blending, the handover
// local → Spotify → local, following the backend's state, a Free account,
// and the pictures.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "cli.h"
#include "handoffengine.h"
#include "librarytree.h"
#include "mockhttp.h"
#include "nowplaying.h"
#include "palette.h"
#include "player.h"
#include "spotify.h"
#include "spotifycontroller.h"

namespace {

// An engine that plays nothing: it keeps what it was given and moves on
// when the test says the file ended.
class FakeEngine : public AudioEngine {
public:
    QString name() const override { return "native"; }
    bool gapless() const override { return true; }
    bool skipsUnplayable() const override { return true; }
    void load(const QString &path, const QUrl & = {}) override {
        m_source = path;
        m_queued.clear();
        setState(Stopped);
    }
    void queueNext(const QString &path, const QUrl & = {}) override { m_queued = path; }
    QString source() const override { return m_source; }
    void play() override {
        if (!m_source.isEmpty())
            setState(Playing);
    }
    void pause() override { setState(Paused); }
    void stop() override { setState(Stopped); }
    void seek(qint64) override {}
    qint64 position() const override { return 0; }
    qint64 duration() const override { return 0; }
    State state() const override { return m_state; }
    void setVolume(qreal) override {}
    void setMuted(bool) override {}
    QVariantMap metaData() const override { return {}; }

    // The current file played to its end.
    void end() {
        if (!m_queued.isEmpty()) {
            m_source = m_queued;
            m_queued.clear();
            emit advanced();
        } else {
            setState(Stopped);
            emit endOfQueue();
        }
    }
    QString queued() const { return m_queued; }

private:
    void setState(State s) {
        if (s != m_state) {
            m_state = s;
            emit stateChanged();
        }
    }
    QString m_source, m_queued;
    State m_state = Stopped;
};

QByteArray pngBytes(QColor color) {
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "png");
    return bytes;
}

// The fake's world: a copy of the fixtures it answers from and changes,
// spotify_player's cache (signed in or not) and config, and the
// environment that points Omamp at them. Restored on destruction.
struct FakeSpotify {
    QTemporaryDir root;
    QHash<QByteArray, QByteArray> saved;
    QHash<QString, QString> ids;

    explicit FakeSpotify(bool signedIn = true, const QString &artBase = "http://127.0.0.1:9") {
        const QString fixtures = QString(OMAMP_TESTS_DIR) + "/fixtures/spotify";
        QDirIterator it(fixtures, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString from = it.next();
            const QString to = state() + from.mid(fixtures.size());
            QDir().mkpath(QFileInfo(to).absolutePath());
            QFile in(from);
            in.open(QIODevice::ReadOnly);
            QByteArray data = in.readAll();
            data.replace("http://art.invalid", artBase.toUtf8());
            QFile out(to);
            out.open(QIODevice::WriteOnly);
            out.write(data);
        }
        QFile list(state() + "/ids.txt");
        list.open(QIODevice::ReadOnly);
        for (const QByteArray &line : list.readAll().split('\n')) {
            const QList<QByteArray> f = line.split('\t');
            if (f.size() == 3)
                ids.insert(QString::fromUtf8(f[0] + ':' + f[1]), QString::fromUtf8(f[2]));
        }
        QDir().mkpath(cache());
        QDir().mkpath(config());
        if (signedIn)
            signIn();
        set("PATH", QString(OMAMP_TESTS_DIR) + "/fake-spotify:" + qEnvironmentVariable("PATH"));
        set("FAKE_SPOTIFY_DIR", state());
        set("OMAMP_SPOTIFY_PLAYER_CACHE", cache());
        set("OMAMP_SPOTIFY_PLAYER_CONFIG", config());
        set("OMAMP_SPOTIFY_PLAYER", "");
        set("OMAMP_SPOTIFAST", "");
        set("OMAMP_SPOTIFY_OEMBED", "off");
        set("OMAMP_SPOTIFY_TIMEOUT_MS", "5000");
        set("XDG_CONFIG_HOME", root.filePath("xdg-config"));
        set("XDG_CACHE_HOME", root.filePath("xdg-cache"));
    }
    ~FakeSpotify() {
        stopDaemon();
        for (auto it = saved.cbegin(); it != saved.cend(); ++it)
            if (it.value().isNull())
                qunsetenv(it.key());
            else
                qputenv(it.key(), it.value());
    }
    void set(const char *name, const QString &value) {
        if (!saved.contains(name))
            saved.insert(name, qEnvironmentVariableIsSet(name) ? qgetenv(name) : QByteArray());
        qputenv(name, value.toUtf8());
    }
    QString state() const { return root.filePath("state"); }
    QString cache() const { return root.filePath("sp-cache"); }
    QString config() const { return root.filePath("sp-config"); }
    QString id(const QString &kindAndName) const { return ids.value(kindAndName); }
    void touch(const QString &file, const QByteArray &data = {}) {
        QFile f(file);
        f.open(QIODevice::WriteOnly);
        f.write(data);
    }
    void signIn() {
        touch(cache() + "/credentials.json");
        touch(cache() + "/0123456789abcdef_token.json");
    }
    void stopDaemon() {
        QFile::remove(state() + "/daemon.alive");
        Spotify::SpotifyPlayerBackend b;
        QElapsedTimer t;
        t.start();
        while (b.running() && t.elapsed() < 5000)
            QTest::qWait(50);
        if (b.running())
            qWarning("the fake daemon is still running after 5 s");
    }
    QStringList calls() const {
        QFile f(state() + "/calls.log");
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    }
    void clearCalls() { QFile::remove(state() + "/calls.log"); }
    // What spotify_player would report: the track at a position.
    void setPlayback(const QString &trackId, qint64 progressMs, bool playing) {
        QFile item(state() + "/tracks/full-" + trackId + ".json");
        item.open(QIODevice::ReadOnly);
        const QByteArray json =
            "{\"device\":{\"id\":\"d1\",\"is_active\":true,\"is_private_session\":false,\"is_restricted\":false,"
            "\"name\":\"spotify-player\",\"type\":\"Speaker\",\"volume_percent\":70},\"repeat_state\":\"off\","
            "\"shuffle_state\":false,\"context\":null,\"timestamp\":1760000000000,\"progress_ms\":" +
            QByteArray::number(progressMs) + ",\"is_playing\":" + (playing ? "true" : "false") +
            ",\"item\":" + item.readAll() +
            ",\"currently_playing_type\":\"track\",\"actions\":{\"disallows\":{}}}\n";
        touch(state() + "/playback.json.new", json);
        QFile::remove(state() + "/playback.json");
        QFile::rename(state() + "/playback.json.new", state() + "/playback.json");
    }
};

QJsonValue readCli(const QString &file) {
    QFile f(file);
    f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson("[" + Spotify::repair(f.readAll()) + "]").array().at(0);
}

} // namespace

class SpotifyTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }

    // --- The CLI's JSON -------------------------------------------------------

    void parsesSpotifyPlayerShapes() {
        FakeSpotify fake;
        // spotify_player turns "\n" escapes into real line breaks; the
        // recorded output has them, and repair() makes it JSON again.
        QFile raw(fake.state() + "/playlists.json");
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QByteArray text = raw.readAll().trimmed();
        QVERIFY(text.contains("weekly") && text.indexOf('\n') > 0);
        QVERIFY(!Spotify::repair(text).contains('\n'));

        const QList<Spotify::Item> playlists = Spotify::parseItems(readCli(fake.state() + "/playlists.json").toArray(), "playlist");
        QCOMPARE(playlists.size(), 5);
        QCOMPARE(playlists[0].name, QString("Late Night Drive"));
        QCOMPARE(playlists[0].owner, QString("Matt Example"));
        QCOMPARE(playlists[3].owner, QString("Spotify"));
        QCOMPARE(playlists[0].id, fake.id("playlist:Late Night Drive"));

        const QList<Spotify::Item> liked = Spotify::parseItems(readCli(fake.state() + "/liked.json").toArray(), "track");
        QCOMPARE(liked.size(), 8);
        QCOMPARE(liked[0].name, QString("Coastline at 2 AM"));
        QCOMPARE(liked[0].artists, QStringList{"Neon Coast"});
        QCOMPARE(liked[0].album, QString("Afterglow Avenue"));
        QCOMPARE(liked[0].durationMs, qint64(232000)); // {"secs": 232, "nanos": 0}
        QCOMPARE(liked[0].year, QString("2019"));
        QCOMPARE(liked[0].uri(), "spotify:track:" + fake.id("track:Coastline at 2 AM"));
        QCOMPARE(liked[0].queuePath(), "spotify://track/" + fake.id("track:Coastline at 2 AM"));

        const QList<Spotify::Item> albums = Spotify::parseItems(readCli(fake.state() + "/albums.json").toArray(), "album");
        QCOMPARE(albums.size(), 6);
        QCOMPARE(albums[1].name, QString("Pixel Rain"));
        QCOMPARE(albums[1].artistText(), QString("Midnight Arcade"));

        const Spotify::SearchResults search = Spotify::parseSearch(readCli(fake.state() + "/search.json").toObject());
        QCOMPARE(search.tracks.size(), 5);
        QCOMPARE(search.albums.size(), 2);
        QCOMPARE(search.artists.size(), 2);
        QCOMPARE(search.playlists.size(), 2);

        const QList<Spotify::Device> devices = Spotify::parseDevices(readCli(fake.state() + "/devices.json").toArray());
        QCOMPARE(devices.size(), 2);
        QVERIFY(devices[0].active);
        QCOMPARE(devices[0].name, QString("spotify-player"));

        // An album's context leaves the album out of its tracks.
        const QList<Spotify::Item> albumTracks = Spotify::parseContextTracks(
            readCli(fake.state() + "/items/album-" + fake.id("album:Pixel Rain") + ".json").toObject());
        QCOMPARE(albumTracks.size(), 3);
        QCOMPARE(albumTracks[0].album, QString("Pixel Rain"));
        QCOMPARE(albumTracks[0].albumId, fake.id("album:Pixel Rain"));

        // The playback state: rspotify's CurrentPlaybackContext with a FullTrack.
        fake.setPlayback(fake.id("track:Harbor Lights"), 61000, true);
        const Spotify::Playback p = Spotify::parsePlayback(readCli(fake.state() + "/playback.json"));
        QVERIFY(p.valid && p.playing);
        QCOMPARE(p.progressMs, qint64(61000));
        QCOMPARE(p.track.name, QString("Harbor Lights"));
        QCOMPARE(p.track.durationMs, qint64(255000));
        QCOMPARE(p.deviceName, QString("spotify-player"));
        QVERIFY(p.track.imageUrl.endsWith("-640.jpg"));
        QVERIFY(!Spotify::parsePlayback(QJsonValue()).valid);
    }

    void parsesReferences() {
        QString kind, id;
        QVERIFY(Spotify::parseReference("spotify:track:7OqU7GsRSBR5QIa6RHgbtA", &kind, &id));
        QCOMPARE(kind, QString("track"));
        QCOMPARE(id, QString("7OqU7GsRSBR5QIa6RHgbtA"));
        QVERIFY(Spotify::parseReference("https://open.spotify.com/intl-de/album/CKrO3fWiBUNv7gSwfwoxCg?si=abc", &kind, &id));
        QCOMPARE(kind, QString("album"));
        QCOMPARE(id, QString("CKrO3fWiBUNv7gSwfwoxCg"));
        QVERIFY(Spotify::parseReference("spotify://playlist/SQJW9AvMtoCHGVBgzFxF4j", &kind, &id));
        QCOMPARE(kind, QString("playlist"));
        QVERIFY(Spotify::parseReference("spotify:user:someone:playlist:SQJW9AvMtoCHGVBgzFxF4j", &kind, &id));
        QCOMPARE(kind, QString("playlist"));
        QVERIFY(Spotify::parseReference("7OqU7GsRSBR5QIa6RHgbtA", &kind, &id, "track"));
        QVERIFY(!Spotify::parseReference("7OqU7GsRSBR5QIa6RHgbtA", &kind, &id));
        QVERIFY(!Spotify::parseReference("https://example.com/track/7OqU7GsRSBR5QIa6RHgbtA", &kind, &id));
        QVERIFY(!Spotify::parseReference("spotify:show:7OqU7GsRSBR5QIa6RHgbtA", &kind, &id));
        QVERIFY(Spotify::isQueuePath("spotify://track/x"));
        QVERIFY(Playlist::isReference("spotify://track/7OqU7GsRSBR5QIa6RHgbtA"));
    }

    void tellsInstancesFromCommands() {
        const QString program = "/opt/fake/spotify_player";
        // The binary itself, as the daemon or the app.
        QVERIFY(Spotify::SpotifyPlayerBackend::isInstance(program, {"spotify_player", "--daemon"}, program));
        QVERIFY(Spotify::SpotifyPlayerBackend::isInstance(program, {"spotify_player"}, program));
        // A script run by its interpreter.
        QVERIFY(Spotify::SpotifyPlayerBackend::isInstance("/usr/bin/bash", {"/bin/sh", program, "-d"}, program));
        // CLI commands are not instances.
        QVERIFY(!Spotify::SpotifyPlayerBackend::isInstance(program, {"spotify_player", "get", "key", "playback"}, program));
        QVERIFY(!Spotify::SpotifyPlayerBackend::isInstance(program, {"spotify_player", "authenticate"}, program));
        // Another program, or another copy elsewhere.
        QVERIFY(!Spotify::SpotifyPlayerBackend::isInstance("/usr/bin/spotify_player", {"spotify_player", "-d"}, program));
        QVERIFY(!Spotify::SpotifyPlayerBackend::isInstance("/usr/bin/bash", {"bash"}, program));
    }

    // --- The backends ------------------------------------------------------------

    void refusesBeforeSignInAndWhileStopped() {
        FakeSpotify fake(false);
        Spotify::SpotifyPlayerBackend b;
        QVERIFY(b.installed());
        QVERIFY(b.program().startsWith(OMAMP_TESTS_DIR));
        QVERIFY(!b.signedIn());
        QString error;
        QVERIFY(b.library("playlists", &error).isEmpty());
        QVERIFY2(error.contains("signed in"), qPrintable(error));
        // Never started without a sign-in: the CLI would start a client
        // that asks for one.
        QVERIFY(!b.start(&error));
        QVERIFY(fake.calls().isEmpty());

        fake.signIn();
        QVERIFY(b.signedIn());
        error.clear();
        QVERIFY(!b.playback(&error).valid);
        QVERIFY2(error.contains("isn't running"), qPrintable(error));
        QVERIFY(fake.calls().isEmpty());
        QCOMPARE(b.signInCommand(), (QStringList{b.program(), "authenticate"}));
    }

    void runsCommandsAgainstTheDaemon() {
        FakeSpotify fake;
        Spotify::SpotifyPlayerBackend b;
        QString error;
        QVERIFY2(b.start(&error), qPrintable(error));
        QVERIFY(b.running());
        QCOMPARE(fake.calls().value(0), QString("--daemon"));

        QCOMPARE(b.library("playlists", &error).size(), 5);
        QCOMPARE(b.library("liked", &error).size(), 8);
        QCOMPARE(b.library("albums", &error).size(), 6);
        QCOMPARE(b.library("artists", &error).size(), 6);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        Spotify::Item playlist;
        playlist.kind = "playlist";
        playlist.id = fake.id("playlist:Late Night Drive");
        QCOMPARE(b.tracksOf(playlist, &error).size(), 5);
        Spotify::Item artist;
        artist.kind = "artist";
        artist.id = fake.id("artist:Neon Coast");
        QCOMPARE(b.tracksOf(artist, &error).size(), 4);

        const Spotify::SearchResults r = b.search("neon coast", &error);
        QCOMPARE(r.tracks.size(), 5);
        QCOMPARE(QString::fromUtf8([&] { QFile f(fake.state() + "/last-search"); f.open(QIODevice::ReadOnly); return f.readAll().trimmed(); }()),
                 QString("neon coast"));

        const QString track = fake.id("track:Pixel Rain");
        fake.clearCalls();
        QVERIFY(b.playTrack(track, &error));
        QVERIFY(b.seekTo(30000, 10000, &error));
        QVERIFY(b.seekTo(5000, 10000, &error));
        QVERIFY(b.like(true, &error));
        QVERIFY(b.like(false, &error));
        Spotify::Item t;
        t.kind = "track";
        t.id = track;
        QVERIFY(b.addToPlaylist(playlist.id, t, &error));
        QVERIFY(b.connectDevice("d1f00dfeedbeef0000000000000000000000000b", &error));
        QCOMPARE(fake.calls(), (QStringList{"playback start track --id " + track, "playback seek -- 20000",
                                            "playback seek -- -5000", "like", "like --unlike",
                                            "playlist edit --track-id " + track + " add " + playlist.id,
                                            "connect --id d1f00dfeedbeef0000000000000000000000000b"}));
        const Spotify::Playback p = b.playback(&error);
        QVERIFY(p.valid && p.playing);
        QCOMPARE(p.track.id, track);
        QCOMPARE(b.track(track, &error).name, QString("Pixel Rain"));
        QCOMPARE(b.account(&error), QString("Matt Example"));

        // The backend's own error comes through, first line, without
        // spotify_player's "Bad request: ".
        fake.touch(fake.state() + "/fail-search", "Bad request: search failed: 429 Too Many Requests\nmore\n");
        QVERIFY(b.search("x", &error).isEmpty());
        QCOMPARE(error, QString("search failed: 429 Too Many Requests"));
        fake.touch(fake.state() + "/fail-playback", "Bad request: Player command failed: Premium required (PREMIUM_REQUIRED)\n");
        QVERIFY(!b.pause(&error));
        QVERIFY2(error.contains("Premium"), qPrintable(error));
    }

    void readsTheDeviceNameFromItsConfig() {
        FakeSpotify fake;
        Spotify::SpotifyPlayerBackend b;
        QCOMPARE(b.deviceName(), QString("spotify-player"));
        fake.touch(fake.config() + "/app.toml", "client_port = 8080\n[device]\nvolume = 70\nname = \"tetelestai\"\n[layout]\nname = \"no\"\n");
        QCOMPARE(b.deviceName(), QString("tetelestai"));
    }

    void spotifastIsControlOnly() {
        FakeSpotify fake;
        Spotify::SpotifastBackend b;
        QVERIFY(b.installed());
        QVERIFY(!b.caps().library && !b.caps().search);
        QString error;
        QVERIFY(b.library("playlists", &error).isEmpty());
        QVERIFY(error.contains("spotify_player"));

        fake.touch(fake.state() + "/spotifast-now.txt",
                   "playing\tHarbor Lights\tChrome Harbor\tHarbor Lights\t61000\t255000\t70\toff\toff\thttps://i.example/x.jpg\tyes\tSpotifast\n");
        bool saved = false;
        error.clear();
        const Spotify::Playback p = b.playback(&error);
        QVERIFY2(p.valid, qPrintable(error));
        QVERIFY(p.playing);
        QCOMPARE(p.track.name, QString("Harbor Lights"));
        QCOMPARE(p.progressMs, qint64(61000));
        QCOMPARE(p.track.durationMs, qint64(255000));
        QCOMPARE(p.track.imageUrl, QString("https://i.example/x.jpg"));
        Spotify::SpotifastBackend::parseNowPlaying("paused\tA\tB\tC\t1\t2\t\t\t\t\tno\tX", &saved);
        QVERIFY(!saved);

        fake.clearCalls();
        QVERIFY(b.playTrack("7OqU7GsRSBR5QIa6RHgbtA", &error));
        QVERIFY(b.seekTo(90500, 0, &error));
        QVERIFY(b.like(true, &error)); // already saved: nothing to toggle
        QCOMPARE(fake.calls(), (QStringList{"spotifast play-uri spotify:track:7OqU7GsRSBR5QIa6RHgbtA",
                                            "spotifast seek-to 90.5", "spotifast now-playing --raw"}));
        fake.touch(fake.state() + "/spotifast-down");
        QVERIFY(!b.pause(&error));
        QVERIFY(error.contains("isn't running"));
    }

    void settingsAreOffByDefault() {
        QTemporaryDir dir;
        const QString file = dir.filePath("spotify.conf");
        Spotify::Settings s = Spotify::Settings::load(file);
        QVERIFY(!s.enabled);
        QCOMPARE(s.backend, QString("spotify_player"));
        s.enabled = true;
        s.backend = "spotifast";
        s.blendPalette = false;
        QVERIFY(s.save(file));
        const Spotify::Settings back = Spotify::Settings::load(file);
        QVERIFY(back.enabled);
        QCOMPARE(back.backend, QString("spotifast"));
        QVERIFY(!back.blendPalette);
        QVERIFY(back.blendSearch);
        // Nothing secret is written.
        QFile f(file);
        f.open(QIODevice::ReadOnly);
        const QByteArray text = f.readAll();
        QVERIFY(!text.contains("token") && !text.contains("password"));
    }

    void commandLine() {
        FakeSpotify fake;
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            QCOMPARE(Cli::run({"spotify", "status", "--json"}, out, err), 0);
            const QJsonObject s = QJsonDocument::fromJson(o.toUtf8()).object();
            QCOMPARE(s.value("running").toBool(), false);
            QVERIFY(s.value("next_step").toString().contains("spotify_player --daemon"));
        }
        Spotify::SpotifyPlayerBackend b;
        QString error;
        QVERIFY(b.start(&error));
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            QCOMPARE(Cli::run({"spotify", "status", "--json"}, out, err), 0);
            const QJsonObject s = QJsonDocument::fromJson(o.toUtf8()).object();
            QVERIFY(s.value("signed_in").toBool() && s.value("running").toBool());
            QCOMPARE(s.value("premium").toString(), QString("yes"));
            QCOMPARE(s.value("account").toString(), QString("Matt Example"));
            QCOMPARE(s.value("devices").toArray().size(), 2);
        }
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            QCOMPARE(Cli::run({"spotify", "search", "neon", "--json"}, out, err), 0);
            const QJsonObject r = QJsonDocument::fromJson(o.toUtf8()).object();
            QCOMPARE(r.value("tracks").toArray().size(), 5);
            QCOMPARE(r.value("tracks").toArray().at(0).toObject().value("name").toString(), QString("Coastline at 2 AM"));
        }
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            QCOMPARE(Cli::run({"spotify", "playlists"}, out, err), 0);
            QVERIFY(o.contains("spotify:playlist:" + fake.id("playlist:Late Night Drive") + "\tLate Night Drive by Matt Example"));
        }
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            fake.clearCalls();
            const QString id = fake.id("track:Tail Lights");
            QCOMPARE(Cli::run({"spotify", "play", "https://open.spotify.com/track/" + id + "?si=abc"}, out, err), 0);
            QCOMPARE(fake.calls().last(), "playback start track --id " + id);
            fake.clearCalls();
            const QString album = fake.id("album:Static Bloom");
            QCOMPARE(Cli::run({"spotify", "play", "spotify:album:" + album}, out, err), 0);
            QCOMPARE(fake.calls().last(), "playback start context --id " + album + " album");
        }
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            QCOMPARE(Cli::run({"spotify", "now", "--json"}, out, err), 0);
            QCOMPARE(QJsonDocument::fromJson(o.toUtf8()).object().value("track").toObject().value("name").toString(),
                     QString("Tail Lights"));
        }
        {
            QString o, e;
            QTextStream out(&o), err(&e);
            QCOMPARE(Cli::run({"spotify", "play", "https://example.com/x", "--json"}, out, err), 1);
            QVERIFY(e.contains("Spotify URI"));
            QCOMPARE(QJsonDocument::fromJson(o.toUtf8()).object().value("ok").toBool(true), false);
        }
    }

    // --- The window's side ------------------------------------------------------

    void enableAndDisable() {
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        QVERIFY(!c.enabled());
        // Off: nothing runs, and a Spotify track in the queue says why.
        c.refreshStatus();
        c.waitForIdle();
        QVERIFY(fake.calls().isEmpty());
        QVERIFY(c.statusText().contains("off"));

        LibraryTree tree;
        auto show = [&] { tree.setExtraNode("spotify", "Spotify", c.enabled()); };
        connect(&c, &SpotifyController::settingsChanged, &tree, show);
        show();
        QCOMPARE(tree.rowOfKey("spotify"), -1);

        c.setEnabled(true);
        QVERIFY(Spotify::Settings::load(dir.filePath("spotify.conf")).enabled);
        QVERIFY(tree.rowOfKey("spotify") >= 0);
        QCOMPARE(tree.kind(tree.rowOfKey("spotify")), QString("spotify"));
        // Switching on starts the daemon (start_daemon is on) and reads
        // the account and the device.
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        QCOMPARE(c.account(), QString("Matt Example"));
        QCOMPARE(c.premium(), QString("yes"));
        QVERIFY(c.statusText().contains("Connected as Matt Example"));
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000); // the playlists
        QCOMPARE(c.ownPlaylists().size(), 3); // Matt's own, not Spotify's or Kai's

        c.openRow(0);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        QCOMPARE(c.viewTitle(), QString("Late Night Drive"));
        QVERIFY(c.canGoBack());
        c.back();
        QTRY_COMPARE_WITH_TIMEOUT(c.section(), QString("playlists"), 5000);
        c.openSection("liked");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 8, 5000);
        c.search("neon");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 11, 5000);
        QCOMPARE(c.items()->get(0).value("section").toString(), QString("Tracks"));
        QCOMPARE(c.items()->get(5).value("section").toString(), QString("Albums"));

        c.setEnabled(false);
        QCOMPARE(tree.rowOfKey("spotify"), -1);
        QCOMPARE(c.items()->count(), 0);
        QVERIFY(!Spotify::Settings::load(dir.filePath("spotify.conf")).enabled);
    }

    void signInRunsTheBackendsOwnCommand() {
        FakeSpotify fake(false);
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(!c.checking(), 5000);
        QVERIFY(!c.signedIn());
        QVERIFY(c.statusText().contains("Not signed in"));
        QStringList launched;
        c.setTerminalLauncher([&](const QStringList &command) {
            launched = command;
            return true;
        });
        QVERIFY(c.signIn());
        QCOMPARE(launched.value(1), QString("authenticate"));
        QVERIFY(launched.value(0).endsWith("/spotify_player"));
        QVERIFY(c.waitingForSignIn());
        // The fake is never asked to authenticate; the files appearing
        // stand for the person finishing in the browser.
        QVERIFY(!fake.calls().contains("authenticate"));
    }

    void blendsIntoThePaletteAndTheSearch() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        Palette palette;
        palette.addAction("neon-mode", "Neon mode");
        c.setPalette(&palette);

        // Off: the palette lists only what is local.
        palette.setText("neo");
        QTest::qWait(700);
        c.waitForIdle();
        QVERIFY(!palette.hasExternalRows());
        QVERIFY(fake.calls().isEmpty());

        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        palette.setText("neon");
        QTRY_VERIFY_WITH_TIMEOUT(palette.hasExternalRows(), 5000);
        QCOMPARE(palette.get(0).value("section").toString(), QString("Local"));
        int first = -1;
        for (int r = 0; r < palette.count(); ++r)
            if (palette.get(r).value("kind").toString() == "external" && first < 0)
                first = r;
        QVERIFY(first > 0);
        const QVariantMap row = palette.get(first);
        QCOMPARE(row.value("section").toString(), QString("Spotify"));
        QCOMPARE(row.value("title").toString(), QString("Coastline at 2 AM"));
        QCOMPARE(row.value("kindLabel").toString(), QString("Track"));
        QCOMPARE(row.value("subtitle").toString(), QString("Neon Coast — Afterglow Avenue"));
        QCOMPARE(row.value("detail").toString(), QString("3:52"));
        // 4 tracks, 2 albums, 2 artists, 2 playlists.
        int external = 0;
        for (int r = 0; r < palette.count(); ++r)
            external += palette.get(r).value("kind").toString() == "external";
        QCOMPARE(external, 10);

        // Choosing the track plays it.
        fake.clearCalls();
        QVERIFY(palette.activate(first));
        QCOMPARE(player.playlist()->count(), 1);
        QCOMPARE(player.playlist()->at(0).path, "spotify://track/" + fake.id("track:Coastline at 2 AM"));
        QCOMPARE(player.playlist()->at(0).title, QString("Coastline at 2 AM"));
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start track --id " + fake.id("track:Coastline at 2 AM")), 5000);

        // Choosing an album opens it in the Spotify view.
        QSignalSpy shown(&c, &SpotifyController::showRequested);
        int albumRow = -1;
        for (int r = 0; r < palette.count(); ++r)
            if (palette.get(r).value("kindLabel").toString() == "Album" && albumRow < 0)
                albumRow = r;
        QVERIFY(palette.activate(albumRow));
        QCOMPARE(shown.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 4, 5000); // Afterglow Avenue's tracks

        // Query-language text and actions stay local.
        palette.setText("artist:neon");
        QTest::qWait(600);
        QVERIFY(!palette.hasExternalRows());

        // The library search box.
        c.blendQuery("neon");
        QTRY_COMPARE_WITH_TIMEOUT(c.blendResults()->count(), 11, 5000);
        QCOMPARE(c.blendResults()->get(0).value("section").toString(), QString("Tracks"));
        c.setBlendSearch(false);
        QCOMPARE(c.blendResults()->count(), 0);
        player.stop();
    }

    void handsOverLocalSpotifyLocal() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.playback()->setTimings(100, 4000);
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.openSection("liked");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 8, 5000);
        const QString id = fake.id("track:Pixel Rain");
        Spotify::Item pixel = c.knownTrack(id);
        QVERIFY(pixel.isValid());

        QList<Playlist::Track> queue;
        queue << Playlist::Track{dir.filePath("a.flac"), "Local A", "Me", 1000, 0};
        queue << Playlist::Track{pixel.queuePath(), pixel.name, pixel.artistText(), pixel.durationMs, 0};
        queue << Playlist::Track{dir.filePath("b.flac"), "Local B", "Me", 1000, 0};
        player.playTracks(queue, 0);
        QCOMPARE(engine->source(), dir.filePath("a.flac"));
        QCOMPARE(player.state(), QString("playing"));
        // The real engine gets nothing to queue: Spotify comes next.
        QCOMPARE(engine->queued(), QString());

        fake.clearCalls();
        engine->end();
        QCOMPARE(player.currentIndex(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start track --id " + id), 5000);
        QCOMPARE(player.state(), QString("playing"));
        QCOMPARE(player.title(), QString("Pixel Rain"));
        QCOMPARE(player.artist(), QString("Midnight Arcade"));
        QCOMPARE(player.duration(), qint64(219000));
        QCOMPARE(engine->state(), AudioEngine::Stopped);
        // Confirmed by the playback state.
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 5000);
        QCOMPARE(c.premium(), QString("yes"));

        // The position follows the backend.
        fake.setPlayback(id, 120000, true);
        QTRY_VERIFY_WITH_TIMEOUT(player.position() >= 120000 && player.position() < 125000, 5000);

        // Paused in Omamp → paused in Spotify.
        fake.clearCalls();
        player.pause();
        QCOMPARE(player.state(), QString("paused"));
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback pause"), 5000);
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback play"), 5000);
        QCOMPARE(player.state(), QString("playing"));
        // Seeking sends the difference.
        fake.setPlayback(id, 100000, true);
        QTest::qWait(400);
        fake.clearCalls();
        player.seek(150000);
        QTRY_VERIFY_WITH_TIMEOUT(!fake.calls().filter("playback seek -- ").isEmpty(), 5000);

        // Spotify reaches the end: the next local file plays in Omamp.
        fake.setPlayback(id, 219000, false);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 2, 5000);
        QCOMPARE(engine->source(), dir.filePath("b.flac"));
        QCOMPARE(engine->state(), AudioEngine::Playing);
        QCOMPARE(player.state(), QString("playing"));
        QCOMPARE(player.title(), QString("Local B"));
        player.stop();
    }

    void followsPausesFromElsewhereAndSpotifyToSpotify() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        // Longer than the 250 ms position tick, which once kept restarting
        // the poll timer so it never fired.
        c.playback()->setTimings(500, 4000);
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        const QString one = fake.id("track:Tail Lights"), two = fake.id("track:Harbor Lights");
        player.playTracks({{"spotify://track/" + one, "Tail Lights", "Lunar Drive", 221000, 0},
                           {"spotify://track/" + two, "Harbor Lights", "Chrome Harbor", 255000, 0}},
                          0);
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering() && player.state() == "playing", 5000);
        QCOMPARE(engine->state(), AudioEngine::Stopped);

        // Paused on the phone: Omamp shows paused and stays on the track.
        fake.setPlayback(one, 90000, false);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("paused"), 5000);
        QCOMPARE(player.position(), qint64(90000));
        QCOMPARE(player.currentIndex(), 0);
        // Resumed there too.
        fake.setPlayback(one, 95000, true);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("playing"), 5000);

        // The end of one Spotify track starts the next.
        fake.clearCalls();
        fake.setPlayback(one, 221000, false);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start track --id " + two), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 5000);
        QCOMPARE(player.title(), QString("Harbor Lights"));

        // Someone starts something else in Spotify: Omamp pauses its queue.
        fake.setPlayback(one, 3000, true);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("paused"), 5000);
        QCOMPARE(player.currentIndex(), 1);
        QVERIFY(c.message().contains("something else"));
        player.stop();
    }

    void freeAccountSaysSo() {
        FakeSpotify fake;
        QTemporaryDir dir;
        // A Free account: spotify_player makes no Connect device, and the
        // start fails in its log while the CLI returns at once.
        fake.touch(fake.state() + "/devices.json",
                   "[{\"id\":\"k1\",\"is_active\":false,\"is_private_session\":false,\"is_restricted\":false,"
                   "\"name\":\"Kitchen speaker\",\"type\":\"Speaker\",\"volume_percent\":40}]\n");
        fake.touch(fake.state() + "/free");
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.playback()->setTimings(100, 800);
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        QCOMPARE(c.premium(), QString("unknown"));
        QVERIFY2(c.statusText().contains("Premium") && c.statusText().contains("Browsing and search work"),
                 qPrintable(c.statusText()));
        // Browsing still works.
        c.openSection("albums");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);

        const QString id = fake.id("track:Insert Coin");
        player.playTracks({{"spotify://track/" + id, "Insert Coin", "Midnight Arcade", 187000, 0}}, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!player.error().isEmpty(), 5000);
        QVERIFY2(player.error().contains("Midnight Arcade - Insert Coin") && player.error().contains("Premium"),
                 qPrintable(player.error()));
        QVERIFY(player.state() != "playing");
    }

    void spotifyOffFailsTheTrackPolitely() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        player.playTracks({{"spotify://track/7OqU7GsRSBR5QIa6RHgbtA", "Coastline at 2 AM", "Neon Coast", 232000, 0},
                           {dir.filePath("c.flac"), "Local C", "Me", 1000, 0}},
                          0);
        // Skipped with a message, on to the local file.
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 3000);
        QVERIFY(player.error().contains("Spotify is off"));
        QCOMPARE(engine->source(), dir.filePath("c.flac"));
        QVERIFY(fake.calls().isEmpty());
    }

    void picturesAreCachedLocally() {
        MockHttp http;
        QList<QByteArray> asked;
        QMutex mutex;
        http.setHandler([&](const MockHttp::Request &r) {
            {
                QMutexLocker lock(&mutex);
                asked << r.path + (r.query.isEmpty() ? QByteArray() : "?" + r.query);
            }
            MockHttp::Reply reply;
            if (r.path == "/oembed") {
                reply.body = QJsonDocument(QJsonObject{{"thumbnail_url", http.url() + "/thumb.png"}}).toJson();
            } else {
                reply.contentType = "image/png";
                reply.body = pngBytes(r.path.contains("thumb") ? Qt::blue : Qt::red);
            }
            return reply;
        });
        FakeSpotify fake(true, http.url());
        fake.set("OMAMP_SPOTIFY_OEMBED", http.url() + "/oembed");
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.playback()->setTimings(100, 4000);
        NowPlaying::setReferenceArt([&c](const QString &path) { return c.art()->fileForPath(path); });
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);

        // The playing track's picture comes from the playback state.
        const QString id = fake.id("track:Static Bloom"), album = fake.id("album:Static Bloom");
        QSignalSpy art(&c, &SpotifyController::playingArtChanged);
        player.playTracks({{"spotify://track/" + id, "Static Bloom", "Velvet Static", 263000, 0}}, 0);
        const QString file = dir.filePath("art/album-" + album + ".jpg");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(file), 5000);
        QCOMPARE(QImage(file).pixelColor(10, 10).red() > 200, true);
        QCOMPARE(c.art()->fileForPath("spotify://track/" + id), file);
        QTRY_VERIFY_WITH_TIMEOUT(art.count() > 0, 3000);
        QVERIFY(asked.contains("/album-" + album.toUtf8() + "-640.jpg")); // the largest
        // NowPlaying stores it with the other covers, for the deck and MPRIS.
        const NowPlaying::Art found = NowPlaying::resolve("spotify://track/" + id, 0, dir.filePath("none.db"), dir.filePath("np"));
        QCOMPARE(found.source, QString("external"));
        QVERIFY(QFileInfo::exists(found.large));
        player.stop();

        // A list row's picture comes from oEmbed, once.
        c.openSection("playlists");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        QCOMPARE(c.items()->get(0).value("kind").toString(), QString("playlist"));
        c.items()->wantArt(0);
        c.items()->wantArt(0);
        const QString playlistFile = dir.filePath("art/playlist-" + fake.id("playlist:Late Night Drive") + ".jpg");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(playlistFile), 5000);
        QVERIFY(QImage(playlistFile).pixelColor(10, 10).blue() > 200);
        QTRY_VERIFY_WITH_TIMEOUT(!c.items()->data(c.items()->index(0), SpotifyItemsModel::ArtRole).toString().isEmpty(), 3000);
        int oembeds = 0;
        for (const QByteArray &a : asked)
            if (a.startsWith("/oembed")) {
                ++oembeds;
                QVERIFY(QUrl::fromPercentEncoding(a).contains("url=https://open.spotify.com/playlist/" +
                                                              fake.id("playlist:Late Night Drive")));
            }
        QCOMPARE(oembeds, 1);
        NowPlaying::setReferenceArt({});
    }
};

int runSpotifyTests(int argc, char **argv) {
    SpotifyTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "spotify_tests.moc"
