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
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimeZone>
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
#include "spotifympris.h"
#include "spotifyrequests.h"

#include "spotify_fakes.h"

namespace {

QJsonValue readCli(const QString &file) {
    QFile f(file);
    f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson("[" + Spotify::repair(f.readAll()) + "]").array().at(0);
}

} // namespace

// A D-Bus of the tests' own, so a fake MPRIS player never appears on the
// desktop's session bus (where the Omarchy bar and media keys would see it)
// and Omamp never sees the real spotify_player there.
struct PrivateBus {
    QTemporaryDir dir;
    QProcess daemon;
    QString address;

    PrivateBus() {
        const QString config = dir.filePath("bus.conf");
        QFile f(config);
        f.open(QIODevice::WriteOnly);
        f.write("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
                "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                // An abstract socket: the scratch path would be too long
                // for a socket's name.
                "<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/>"
                "<allow receive_sender=\"*\"/><allow send_type=\"method_call\"/>"
                "<allow own=\"*\"/></policy></busconfig>\n");
        f.close();
        daemon.start("dbus-daemon", {"--config-file=" + config, "--nofork", "--print-address=1"});
        if (!daemon.waitForStarted(5000))
            return;
        for (int i = 0; i < 50 && address.isEmpty() && daemon.state() == QProcess::Running; ++i) {
            daemon.waitForReadyRead(100);
            address = QString::fromUtf8(daemon.readLine()).trimmed();
        }
    }
    ~PrivateBus() {
        daemon.kill();
        daemon.waitForFinished(2000);
    }
    bool ok() const { return !address.isEmpty(); }
};

// spotify_player's MPRIS service as it looks on the bus: PlaybackStatus,
// Metadata without an id ("mpris:trackid" is "/") and Position, with
// PropertiesChanged for the first two.
class FakeMpris : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)

public:
    FakeMpris(const QString &address, const QString &name) : m_bus(QDBusConnection::connectToBus(address, "fake-mpris")) {
        m_bus.registerObject("/org/mpris/MediaPlayer2", this, QDBusConnection::ExportAllProperties);
        m_registered = m_bus.registerService(name);
        m_name = name;
    }
    ~FakeMpris() override {
        m_bus.unregisterService(m_name);
        m_bus.unregisterObject("/org/mpris/MediaPlayer2");
        QDBusConnection::disconnectFromBus("fake-mpris");
    }
    bool registered() const { return m_registered; }
    QString why() const { return m_bus.lastError().message() + " (" + QString(m_bus.isConnected() ? "connected" : "not connected") + ")"; }
    QString playbackStatus() const { return m_status; }
    QVariantMap metadata() const { return m_metadata; }
    qlonglong position() const { return m_positionUs; }
    double volume() const { return m_volume; }
    void setVolume(double v) { m_volume = v; }

    // What spotify_player would show for a track.
    void show(const QString &title, const QString &artists, const QString &album, qint64 lengthMs,
              const QString &status, qint64 positionMs) {
        m_metadata = {{"xesam:title", title},
                      {"xesam:artist", QStringList{artists}},
                      {"xesam:album", album},
                      {"mpris:trackid", QVariant::fromValue(QDBusObjectPath("/"))},
                      {"mpris:length", qlonglong(lengthMs * 1000)},
                      {"mpris:artUrl", QString()}};
        m_status = status;
        m_positionUs = positionMs * 1000;
        announce({{"PlaybackStatus", m_status}, {"Metadata", m_metadata}});
    }
    void setStatus(const QString &status, qint64 positionMs) {
        m_status = status;
        m_positionUs = positionMs * 1000;
        announce({{"PlaybackStatus", m_status}});
    }

private:
    void announce(const QVariantMap &changed) {
        QDBusMessage m = QDBusMessage::createSignal("/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                                    "PropertiesChanged");
        m << QString("org.mpris.MediaPlayer2.Player") << changed << QStringList();
        m_bus.send(m);
    }

    QDBusConnection m_bus;
    QString m_name;
    bool m_registered = false;
    QString m_status = "Stopped";
    QVariantMap m_metadata;
    qint64 m_positionUs = 0;
    double m_volume = 1.0;
};

// Counts the playback-state reads in the fake's log: each one is a Spotify
// Web API request.
int stateReads(const QStringList &calls) { return calls.filter("get key playback").size(); }

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
        fake.clearCalls();
        QCOMPARE(b.tracksOf(artist, &error).size(), 4);
        // From search, never from `get item … artist`, which Spotify refuses.
        QVERIFY2(fake.calls().filter(QRegularExpression(" artist$")).isEmpty(), qPrintable(fake.calls().join(" | ")));

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
        // The list loads when the view is shown, from the playlists the
        // status check already read.
        QCOMPARE(c.items()->count(), 0);
        c.setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000); // the playlists
        QCOMPARE(fake.calls().filter("user-playlists").size(), 1);
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
    // --- Rate limits, the queue and MPRIS ----------------------------------------

    void queueSharesSerialisesAndSpaces() {
        FakeSpotify fake;
        Spotify::Requests q;
        q.setBackend(std::make_shared<Spotify::SpotifastBackend>());
        q.setGapMs(200);
        QAtomicInt runs = 0, running = 0, most = 0;
        auto work = [&](Spotify::Backend &, Spotify::Result &r) {
            ++runs;
            const int now = ++running;
            if (now > most.loadRelaxed())
                most.storeRelaxed(now);
            QThread::msleep(150);
            --running;
            r.extra.insert("t", QDateTime::currentMSecsSinceEpoch());
        };
        // Three asks for the same list: one command, three answers.
        int answers = 0;
        for (int i = 0; i < 3; ++i)
            q.submit(Spotify::Requests::Data, "library:playlists", work, this, [&](const Spotify::Result &) { ++answers; });
        // Two other lists: one at a time, 200 ms apart.
        QList<qint64> starts;
        for (const QString &key : {"a", "b"})
            q.submit(Spotify::Requests::Data, key, work, this,
                     [&](const Spotify::Result &r) { starts << r.extra.value("t").toLongLong(); });
        q.waitForIdle();
        QCOMPARE(answers, 3);
        QCOMPARE(runs.loadRelaxed(), 3);
        QCOMPARE(most.loadRelaxed(), 1);
        QCOMPARE(starts.size(), 2);
        QVERIFY2(starts[1] - starts[0] >= 190, qPrintable(QString::number(starts[1] - starts[0])));

        // The control lane doesn't wait behind a long list.
        runs = 0;
        most = 0;
        q.submit(Spotify::Requests::Data, "long", [](Spotify::Backend &, Spotify::Result &) { QThread::msleep(800); },
                 this, {});
        QElapsedTimer t;
        t.start();
        qint64 control = -1;
        q.submit(Spotify::Requests::Control, {}, [](Spotify::Backend &, Spotify::Result &) {}, this,
                 [&](const Spotify::Result &) { control = t.elapsed(); });
        QTRY_VERIFY_WITH_TIMEOUT(control >= 0, 3000);
        QVERIFY2(control < 600, qPrintable(QString::number(control)));
        q.waitForIdle();

        // Held back: nothing starts until the hold is over.
        q.setGapMs(0);
        q.holdData(600);
        t.restart();
        qint64 held = -1;
        q.submit(Spotify::Requests::Data, "held", [](Spotify::Backend &, Spotify::Result &) {}, this,
                 [&](const Spotify::Result &) { held = t.elapsed(); });
        QTRY_VERIFY_WITH_TIMEOUT(held >= 0, 3000);
        QVERIFY2(held >= 550, qPrintable(QString::number(held)));

        // Cancelled while waiting and while running.
        bool waitingCancelled = false, runningCancelled = false;
        q.submit(Spotify::Requests::Data, "run", [](Spotify::Backend &, Spotify::Result &) {
            for (int i = 0; i < 100 && !Spotify::CancelScope::cancelled(); ++i)
                QThread::msleep(20);
        }, this, [&](const Spotify::Result &r) { runningCancelled = r.cancelled; });
        q.submit(Spotify::Requests::Data, "wait", [](Spotify::Backend &, Spotify::Result &) {}, this,
                 [&](const Spotify::Result &r) { waitingCancelled = r.cancelled; });
        QTest::qWait(100);
        q.cancel("wait");
        q.cancel("run");
        QVERIFY(waitingCancelled);
        QTRY_VERIFY_WITH_TIMEOUT(runningCancelled, 2000);
    }

    void aCancelledCommandStopsItsProcess() {
        FakeSpotify fake;
        Spotify::SpotifyPlayerBackend b;
        QString error;
        QVERIFY(b.start(&error));
        fake.touch(fake.state() + "/slow-get", "5");
        auto flag = Spotify::makeCancelFlag();
        QElapsedTimer t;
        t.start();
        QThread *worker = QThread::create([&] {
            Spotify::CancelScope scope(flag);
            b.library("playlists", &error);
        });
        worker->start();
        QTest::qWait(300);
        flag->store(true);
        QVERIFY(worker->wait(3000));
        delete worker;
        QVERIFY2(t.elapsed() < 2500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(error, QString("cancelled"));
    }

    void readsRateLimitsFromTheLog() {
        QTemporaryDir dir;
        const QDateTime now = QDateTime::fromString("2026-10-20T02:12:30", Qt::ISODate);
        QDateTime utc = now;
        utc.setTimeZone(QTimeZone::UTC);
        QFile log(dir.filePath("spotify-player-26-10-20-12-07.log"));
        QVERIFY(log.open(QIODevice::WriteOnly));
        // Lines as spotify_player 0.25.1 writes them (from Matt's log, with
        // the request names kept and nothing personal).
        log.write("2026-10-20T02:07:27.488290Z  INFO spotify_player: Configurations: ...\n"
                  "2026-10-20T02:07:37.753933Z  WARN spotify_player::client::middleware: Spotify Web API rate limit "
                  "encountered method=GET url=https://api.spotify.com/v1/me/player retry_after_secs=2\n"
                  "2026-10-20T02:12:20.672865Z  WARN socket_request{request=Get(Item(Playlist, Id(\"x\")))}: "
                  "spotify_player::client::middleware: Spotify Web API rate limit encountered method=GET "
                  "url=https://api.spotify.com/v1/playlists/x retry_after_secs=19\n"
                  "2026-10-20T02:12:21.000000Z  INFO spotify_player::client: unrelated\n");
        log.close();
        QString file;
        qint64 offset = 0;
        Spotify::LogNews news = Spotify::readLogNews(dir.path(), &file, &offset, 120, utc);
        // The 02:07 line is older than two minutes; the 02:12 one counts.
        QCOMPARE(news.rateLimits, 1);
        QCOMPARE(news.retryUntil.toString(Qt::ISODate), QString("2026-10-20T02:12:39Z"));
        // Read again: nothing new.
        news = Spotify::readLogNews(dir.path(), &file, &offset, 120, utc);
        QCOMPARE(news.rateLimits, 0);
        QVERIFY(log.open(QIODevice::Append));
        log.write("2026-10-20T02:12:25.000000Z  WARN spotify_player::client::spotify: Custom Spotify Web API request "
                  "failed; retrying with ncspot client method=\"GET\" url=\"me/albums\" status=429 Too Many Requests\n");
        log.close();
        news = Spotify::readLogNews(dir.path(), &file, &offset, 120, utc);
        QCOMPARE(news.rateLimits, 1);
        QVERIFY(Spotify::isRateLimitText("search failed: 429 Too Many Requests"));
        QVERIFY(!Spotify::isRateLimitText("Premium required"));
    }

    void aSlowRateLimitedListSaysSoAndArrives() {
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setSlowMs(300);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        // Spotify rate-limits: spotify_player logs it and waits before it
        // answers.
        fake.touch(fake.state() + "/ratelimit", "8");
        fake.touch(fake.state() + "/slow-get", "2.5");
        c.openRow(0);
        QVERIFY(c.loading());
        QCOMPARE(c.viewTitle(), QString("Late Night Drive")); // shown at once
        QTRY_VERIFY_WITH_TIMEOUT(c.loadingSlow(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(c.rateLimited(), 3000);
        QVERIFY(c.retryIn() > 0);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 8000);
        QVERIFY(!c.loading());
        QVERIFY(c.viewError().isEmpty());
        // Opened again: from the cache, nothing asked.
        QFile::remove(fake.state() + "/ratelimit");
        QFile::remove(fake.state() + "/slow-get");
        c.back();
        fake.clearCalls();
        c.openRow(0);
        QCOMPARE(c.items()->count(), 5);
        QVERIFY(!c.loading());
        c.waitForIdle();
        QVERIFY(fake.calls().filter("get item").isEmpty());
        // The refresh button reads it again.
        c.reload();
        QTRY_VERIFY_WITH_TIMEOUT(!fake.calls().filter("get item").isEmpty(), 5000);
    }

    void aRateLimitedFailureRetriesThenOffersRetry() {
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        // spotify_player gave up after its own retries; its log says to
        // wait three seconds.
        fake.touch(fake.state() + "/ratelimit", "3");
        fake.touch(fake.state() + "/fail-get", "Bad request: 429 Too Many Requests\n");
        fake.clearCalls();
        c.openSection("liked");
        QTRY_VERIFY_WITH_TIMEOUT(c.rateLimited(), 5000);
        QVERIFY(c.loading()); // still waiting, to try again
        QVERIFY(c.viewError().isEmpty());
        // Spotify lets go before the retry: it arrives on its own.
        QFile::remove(fake.state() + "/fail-get");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 8, 10000);
        QCOMPARE(fake.calls().filter("user-liked-tracks").size(), 2);
        QVERIFY(c.viewError().isEmpty());

        // Failing every time: two retries, then an error and Retry.
        fake.touch(fake.state() + "/fail-get", "Bad request: 429 Too Many Requests\n");
        fake.clearCalls();
        c.openSection("albums");
        QTRY_VERIFY_WITH_TIMEOUT(!c.loading(), 20000);
        QVERIFY2(c.viewError().contains("rate-limiting"), qPrintable(c.viewError()));
        QCOMPARE(fake.calls().filter("user-saved-albums").size(), 3);
    }

    void cancelStopsALoad() {
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        fake.touch(fake.state() + "/slow-get", "6");
        c.openSection("artists");
        QTRY_VERIFY_WITH_TIMEOUT(c.loading(), 2000);
        QTest::qWait(300);
        QElapsedTimer t;
        t.start();
        c.cancelLoad();
        QTRY_VERIFY_WITH_TIMEOUT(!c.loading(), 3000);
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(c.viewError().contains("Stopped loading"));
        QFile::remove(fake.state() + "/slow-get");
        c.reload();
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);
    }

    void noBurstAtStartUp() {
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setWarmupMs(1500);
        QElapsedTimer t;
        t.start();
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 10000);
        // Omamp started spotify_player, then left it alone while it read
        // the library itself, then asked twice: devices and playlists.
        QVERIFY2(t.elapsed() >= 1400, qPrintable(QString::number(t.elapsed())));
        const QStringList calls = fake.calls();
        QCOMPARE(calls, (QStringList{"--daemon", "get key devices", "get key user-playlists"}));
        // Liked Songs, albums and artists wait until they are looked at.
        // Showing the view lists the playlists from what was read, and
        // reads what plays now once (no MPRIS here).
        c.setViewVisible(true);
        c.waitForIdle();
        QCOMPARE(c.items()->count(), 5);
        QCOMPARE(fake.calls().mid(3), QStringList{"get key playback"});
        c.setViewVisible(false);
        c.setViewVisible(true);
        c.waitForIdle();
        QCOMPARE(fake.calls().size(), 4);
    }

    void ownClientIdIsWrittenCarefully() {
        QTemporaryDir dir;
        const QString folder = dir.path();
        using B = Spotify::SpotifyPlayerBackend;
        // Shaped like a real app.toml: a multi-line string holding a line
        // that looks like a table, and tables after.
        const QByteArray original = "theme = \"dracula\"\n"
                                    "client_id = \"65b708073fc0480ea92a077233ca87bd\"\n"
                                    "client_port = 8080\n"
                                    "playback_format = \"\"\"\n"
                                    "[not a table] {track}\n"
                                    "client_id = \"inside a string\"\n"
                                    "{album}\"\"\"\n"
                                    "enable_media_control = true # MPRIS\n"
                                    "\n"
                                    "[device]\n"
                                    "name = \"spotify-player\"\n"
                                    "client_id = \"not the top level\"\n";
        QFile f(folder + "/app.toml");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(original);
        f.close();
        QCOMPARE(B::configuredClientId(folder), B::spotifyDesktopClientId());
        QString backup, error;
        QVERIFY(!B::writeClientId(folder, "not-an-id", &backup, &error));
        QVERIFY(error.contains("isn't a Spotify client ID"));
        const QString id = "0123456789ABCDEF0123456789abcdef";
        QVERIFY2(B::writeClientId(folder, id, &backup, &error), qPrintable(error));
        QVERIFY(backup.startsWith(folder + "/app.toml.omamp-backup-"));
        QFile b(backup);
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), original);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray expected = original;
        expected.replace("client_id = \"65b708073fc0480ea92a077233ca87bd\"",
                         "client_id = \"0123456789abcdef0123456789abcdef\"\nncspot_only_get_endpoints = []");
        QCOMPARE(f.readAll(), expected);
        f.close();
        QCOMPARE(B::configuredClientId(folder), id.toLower());

        // Without a client_id: added above the first table.
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("theme = \"dracula\"\n[device]\nname = \"x\"\n");
        f.close();
        QVERIFY(B::writeClientId(folder, id, &backup, &error));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("theme = \"dracula\"\nclient_id = \"0123456789abcdef0123456789abcdef\"\n"
                                         "ncspot_only_get_endpoints = []\n[device]\nname = \"x\"\n"));
        // An existing endpoint list is replaced, not doubled.
        f.close();
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("ncspot_only_get_endpoints = [\"me/playlists\"]\nclient_id = \"x\"\n");
        f.close();
        QVERIFY(B::writeClientId(folder, id, &backup, &error));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("client_id = \"0123456789abcdef0123456789abcdef\"\n"
                                         "ncspot_only_get_endpoints = []\n"));
        f.close();
        f.close();
        // No file: one is made.
        QTemporaryDir empty;
        QVERIFY(B::writeClientId(empty.filePath("sp"), id, &backup, &error));
        QCOMPARE(B::configuredClientId(empty.filePath("sp")), id.toLower());
        // client_id_command wins over client_id, so it is refused.
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("client_id_command = { command = \"pass\", args = [\"spotify\"] }\n");
        f.close();
        QVERIFY(!B::writeClientId(folder, id, &backup, &error));
        QVERIFY(error.contains("client_id_command"));
    }

    void ownClientIdStopsTheDaemonAndAsksForSignIn() {
        FakeSpotify fake;
        QTemporaryDir dir;
        fake.touch(fake.config() + "/app.toml", "theme = \"dracula\"\n[device]\nname = \"spotify-player\"\n");
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        QCOMPARE(c.clientIdKind(), QString("default"));
        QVERIFY(!c.useOwnClientId("nope"));
        QVERIFY(c.clientIdMessage().contains("isn't a Spotify client ID"));
        const QString id = "abcdefabcdefabcdefabcdefabcdef12";
        QVERIFY(c.useOwnClientId(id));
        QTRY_VERIFY_WITH_TIMEOUT(!c.clientIdBusy(), 10000);
        QVERIFY2(c.clientIdMessage().contains("spotify_player authenticate"), qPrintable(c.clientIdMessage()));
        QCOMPARE(c.clientIdKind(), QString("own"));
        QCOMPARE(c.clientId(), id);
        QCOMPARE(Spotify::SpotifyPlayerBackend::configuredClientId(), id);
        QVERIFY(!QDir(fake.config()).entryList({"app.toml.omamp-backup-*"}, QDir::Files).isEmpty());
        // Stopped, and not started again: the new ID needs a sign-in first.
        QTRY_VERIFY_WITH_TIMEOUT(!c.checking(), 8000);
        QVERIFY(!Spotify::SpotifyPlayerBackend().running());
        QVERIFY(!c.signedIn());
        QVERIFY(!c.running());
        QVERIFY(c.statusText().contains("Not signed in"));
        // Signing in (the token for the new ID appears) starts it again.
        fake.touch(fake.cache() + "/" + id + "_token.json");
        c.refreshStatus();
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && c.signedIn() && !c.checking(), 8000);
    }

    void followsMprisWithoutAskingSpotify() {
        PrivateBus bus;
        if (!bus.ok())
            QSKIP("dbus-daemon isn't available");
        FakeSpotify fake;
        fake.set("OMAMP_SPOTIFY_MPRIS_BUS", bus.address);
        fake.set("OMAMP_SPOTIFY_MPRIS_NAME", "org.mpris.MediaPlayer2.spotify_player");
        FakeMpris mpris(bus.address, "org.mpris.MediaPlayer2.spotify_player");
        QVERIFY2(mpris.registered(), qPrintable(mpris.why() + " at " + bus.address));
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.playback()->setTimings(100, 4000); // would poll ten times a second
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(c.followMode(), QString("mpris"), 3000);
        c.openSection("liked");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 8, 5000);
        fake.clearCalls();

        const QString one = fake.id("track:Tail Lights");
        const Spotify::Item tail = c.knownTrack(one);
        QVERIFY(!tail.name.isEmpty());
        player.playTracks({{"spotify://track/" + one, "Tail Lights", "Lunar Drive", tail.durationMs, 0},
                           {dir.filePath("b.flac"), "Local B", "Me", 1000, 0}},
                          0);
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start track --id " + one), 5000);
        QVERIFY(player.buffering());
        // spotify_player shows it: no id, so title and length match it.
        mpris.show("Tail Lights", "Lunar Drive", tail.album, tail.durationMs, "Playing", 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 3000);
        QCOMPARE(player.state(), QString("playing"));
        QCOMPARE(c.nowPlaying().value("title").toString(), QString("Tail Lights"));

        // Omamp's volume slider reaches Spotify's player, on the slider's
        // own scale (librespot applies its own curve).
        player.setVolume(50);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(mpris.volume() - 0.5) < 0.02, 3000);
        player.setVolume(20);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(mpris.volume() - 0.2) < 0.02, 3000);

        // Paused on the phone.
        QTest::qWait(300);
        mpris.setStatus("Paused", 90000);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("paused"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(player.position() - 90000) < 1500, 3000);
        mpris.setStatus("Playing", 90000);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("playing"), 3000);

        // The end: the next local file plays.
        mpris.setStatus("Paused", tail.durationMs);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 3000);
        QCOMPARE(engine->source(), dir.filePath("b.flac"));
        // All of that without one read of Spotify's playback state.
        QCOMPARE(stateReads(fake.calls()), 0);
        player.stop();
    }

    void withoutMprisItPollsSlowly() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        // The defaults: 5 s between reads, sooner only while starting.
        QCOMPARE(c.playback()->pollMs(), 5000);
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        QCOMPARE(c.followMode(), QString("polling"));
        const QString one = fake.id("track:Tail Lights");
        player.playTracks({{"spotify://track/" + one, "Tail Lights", "Lunar Drive", 221000, 0}}, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 5000);
        fake.clearCalls();
        QTest::qWait(6000);
        QVERIFY2(stateReads(fake.calls()) <= 2, qPrintable(fake.calls().join(" | ")));
        player.stop();
        // Nothing playing in Omamp: no reads at all.
        c.waitForIdle();
        fake.clearCalls();
        QTest::qWait(1500);
        QCOMPARE(stateReads(fake.calls()), 0);
    }

    void searchesShareOneRequest() {
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        Palette palette;
        c.setPalette(&palette);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        fake.clearCalls();
        // The same words in the library search and the palette.
        c.blendQuery("neon");
        palette.setText("neon");
        QTRY_COMPARE_WITH_TIMEOUT(c.blendResults()->count(), 11, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(palette.hasExternalRows(), 5000);
        c.search("neon");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 11, 5000);
        c.waitForIdle();
        QCOMPARE(fake.calls().filter("search").size(), 1);
    }

    // --- Spotify's limits on apps made since November 2024 ---------------------------
    //
    // With your own client ID, Spotify refuses an artist's top tracks,
    // related artists and album list, and the details and tracks of the
    // playlists it makes itself. spotify_player then asks again with
    // ncspot's shared ID, which is nearly always rate-limited, so the
    // answer takes up to a minute. The fake does the same (refusals in its
    // log, then a late answer).

    void refusalsAreKeptPerClientIdForTheDay() {
        QCOMPARE(Spotify::refusalKey("artists/3JlzEFwarS8ZcCGNFy11y4/top-tracks"), QString("artist-top-tracks"));
        QCOMPARE(Spotify::refusalKey("artists/3JlzEFwarS8ZcCGNFy11y4/related-artists"),
                 QString("artist-related-artists"));
        QCOMPARE(Spotify::refusalKey("artists/3JlzEFwarS8ZcCGNFy11y4/albums?include_groups=album,single"),
                 QString("artist-albums"));
        QCOMPARE(Spotify::refusalKey("playlists/37i9dQZF1DZ06evO2aJbod"), QString("playlist:37i9dQZF1DZ06evO2aJbod"));
        QCOMPARE(Spotify::refusalKey("playlists/37i9dQZF1DZ06evO2aJbod/items"),
                 QString("playlist:37i9dQZF1DZ06evO2aJbod"));
        QCOMPARE(Spotify::refusalKey("https://api.spotify.com/v1/recommendations?seed_artists=x"),
                 QString("recommendations"));
        QCOMPARE(Spotify::refusalKey("audio-features/3JlzEFwarS8ZcCGNFy11y4"), QString("audio-features/*"));

        // Lines as spotify_player 0.25.1 wrote them on Matt's machine.
        QTemporaryDir dir;
        const QDateTime now = QDateTime::fromString("2026-10-20T04:07:00Z", Qt::ISODate);
        QFile log(dir.filePath("spotify-player-26-10-20-13-38.log"));
        QVERIFY(log.open(QIODevice::WriteOnly));
        log.write("2026-10-19T23:00:00.000000Z  WARN x: spotify_player::client::spotify: Custom Spotify Web API "
                  "request failed; retrying with ncspot client method=\"GET\" url=\"playlists/yesterday\" status=404 Not Found\n"
                  "2026-10-20T03:39:03.811343Z  WARN socket_request{request=Get(Item(Playlist, Id(\"37i9dQZF1DZ06evO2aJbod\"))) "
                  "dest_addr=127.0.0.1:39329}: spotify_player::client::spotify: Custom Spotify Web API request failed; "
                  "retrying with ncspot client method=\"GET\" url=\"playlists/37i9dQZF1DZ06evO2aJbod\" status=404 Not Found\n"
                  "2026-10-20T04:05:19.269861Z  WARN socket_request{request=Get(Item(Artist, Id(\"3JlzEFwarS8ZcCGNFy11y4\"))) "
                  "dest_addr=127.0.0.1:35448}: spotify_player::client::spotify: Custom Spotify Web API request failed; "
                  "retrying with ncspot client method=\"GET\" url=\"artists/3JlzEFwarS8ZcCGNFy11y4/top-tracks\" status=403 Forbidden\n"
                  "2026-10-20T04:06:20.470298Z  WARN socket_request{request=Get(Item(Artist, Id(\"3JlzEFwarS8ZcCGNFy11y4\"))) "
                  "dest_addr=127.0.0.1:35448}: spotify_player::client::spotify: Custom Spotify Web API request failed; "
                  "retrying with ncspot client method=\"GET\" url=\"artists/3JlzEFwarS8ZcCGNFy11y4/albums?include_groups=album,single\" status=400 Bad Request\n"
                  "2026-10-20T04:06:30.000000Z  WARN x: spotify_player::client::spotify: Custom Spotify Web API request "
                  "failed; retrying with ncspot client method=\"GET\" url=\"me/albums\" status=429 Too Many Requests\n");
        log.close();
        QString file;
        qint64 offset = 0;
        const Spotify::LogNews news = Spotify::readLogNews(dir.path(), &file, &offset, 120, now);
        // Today's three refusals, though older than the rate-limit window;
        // yesterday's isn't, and a 429 is a rate limit, not a refusal.
        QCOMPARE(news.refusals.size(), 3);
        QCOMPARE(news.refusals.at(0).status, 404);
        QCOMPARE(news.refusals.at(1).url, QString("artists/3JlzEFwarS8ZcCGNFy11y4/top-tracks"));
        QCOMPARE(news.rateLimits, 1);

        // Kept per client ID, in a file, for the day.
        const QString store = dir.filePath("refused.json");
        const QDate today(2026, 10, 20);
        {
            Spotify::Refusals r(store);
            r.setClientId("c789e6070c2845b0bf498faa31d397a8");
            QVERIFY(r.learn("artist-top-tracks", 403, today));
            QVERIFY(!r.learn("artist-top-tracks", 403, today));
            QVERIFY(r.learn("playlist:37i9dQZF1DZ06evO2aJbod", 404, today));
        }
        Spotify::Refusals again(store);
        again.setClientId("C789E6070C2845B0BF498FAA31D397A8");
        QVERIFY(again.refused("artist-top-tracks", today));
        QCOMPARE(again.keys(today).size(), 2);
        // Tomorrow Spotify is asked again.
        QVERIFY(!again.refused("artist-top-tracks", today.addDays(1)));
        // Another client ID starts afresh.
        again.setClientId("d420a117a32841c2b3474932e49fb54b");
        QVERIFY(!again.refused("artist-top-tracks", today));

        // Who made a playlist.
        Spotify::Item p;
        p.kind = "playlist";
        p.id = "37i9dQZF1DZ06evO2aJbod";
        QVERIFY(p.spotifyMade());
        p.id = "4ytkzjU3Lf8GoSTI1oE9uR";
        QVERIFY(!p.spotifyMade());
        p.ownerId = "spotify";
        QVERIFY(p.spotifyMade());
        QCOMPARE(Spotify::contextQueuePath(p, true), QString("spotify://playlist/4ytkzjU3Lf8GoSTI1oE9uR?shuffle=1"));
        QString kind, id;
        QVERIFY(Spotify::parseReference("spotify://playlist/4ytkzjU3Lf8GoSTI1oE9uR?shuffle=1", &kind, &id));
        QCOMPARE(id, QString("4ytkzjU3Lf8GoSTI1oE9uR"));
        QVERIFY(Spotify::queuePathShuffles("spotify://playlist/4ytkzjU3Lf8GoSTI1oE9uR?shuffle=1"));
    }

    void anArtistPageComesFromSearchAlone() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.openSection("artists");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);
        int row = -1;
        for (int r = 0; r < c.items()->count(); ++r)
            if (c.items()->at(r).name == "Neon Coast")
                row = r;
        QVERIFY(row >= 0);
        fake.clearCalls();
        QElapsedTimer t;
        t.start();
        c.openRow(row);
        QCOMPARE(c.viewKind(), QString("artist"));
        QCOMPARE(c.viewTitle(), QString("Neon Coast"));
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        // Fast: the fake answers `get item … artist` only after its
        // refusals and a wait, as Spotify does.
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(c.items()->get(0).value("section").toString(), QString("Popular tracks"));
        QCOMPARE(c.items()->get(4).value("section").toString(), QString("Albums"));
        QCOMPARE(c.items()->at(4).name, QString("Afterglow Avenue"));
        QVERIFY(!c.viewNote().isEmpty());
        QCOMPARE(fake.calls(), (QStringList{"search -- Neon Coast", "search -- artist:\"Neon Coast\""}));

        // Play plays the artist on Spotify as one entry: no tracks read.
        fake.clearCalls();
        c.playView(false);
        QCOMPARE(player.playlist()->count(), 1);
        const QString id = fake.id("artist:Neon Coast");
        QCOMPARE(player.playlist()->at(0).path, "spotify://artist/" + id);
        QCOMPARE(player.playlist()->at(0).title, QString("Neon Coast"));
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start context --id " + id + " artist"), 5000);
        player.stop();

        // An artist row's Play does the same, from the list of artists.
        c.back();
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);
        fake.clearCalls();
        c.enqueueRow(row);
        QCOMPARE(player.playlist()->count(), 2);
        c.waitForIdle();
        // Not once was an artist read with `get item`.
        QVERIFY2(fake.calls().filter(QRegularExpression(" artist$")).filter("get item").isEmpty(),
                 qPrintable(fake.calls().join(" | ")));

        // A link to an artist Omamp knows by name opens the same page.
        c.activateUri("https://open.spotify.com/artist/" + fake.id("artist:Lunar Drive"));
        QTRY_VERIFY_WITH_TIMEOUT(!c.loading() && c.viewTitle() == "Lunar Drive", 5000);
        QVERIFY(c.items()->count() > 0);
    }

    void spotifysOwnPlaylistPlaysWithoutReadingIt() {
        FakeSpotify fake;
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.playback()->setTimings(100, 4000);
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        const QString synth = fake.id("playlist:Synthwave Essentials");
        fake.touch(fake.state() + "/restricted", synth.toUtf8() + "\n");
        fake.touch(fake.state() + "/slow-refused", "1");
        const QString tail = fake.id("track:Tail Lights"), harbor = fake.id("track:Harbor Lights");
        fake.touch(fake.state() + "/context-track", tail.toUtf8());
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        QCOMPARE(c.items()->at(3).name, QString("Synthwave Essentials")); // owned by "spotify"

        // Opened: the header at once, no list, nothing asked of Spotify.
        fake.clearCalls();
        c.openRow(3);
        QVERIFY(c.viewRestricted());
        QVERIFY(!c.loading());
        QCOMPARE(c.items()->count(), 0);
        QVERIFY(c.viewNote().contains("Spotify made this playlist"));
        c.waitForIdle();
        QVERIFY(fake.calls().filter("get item").isEmpty());

        // Play: one Now Playing entry, played by context.
        QList<Playlist::Track> after{{dir.filePath("b.flac"), "Local B", "Me", 1000, 0}};
        c.playView(false);
        player.appendTracks(after);
        QCOMPARE(player.playlist()->count(), 2);
        QCOMPARE(player.playlist()->at(0).path, "spotify://playlist/" + synth);
        QCOMPARE(player.playlist()->at(0).title, QString("Synthwave Essentials"));
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start context --id " + synth + " playlist"), 5000);
        QCOMPARE(engine->state(), AudioEngine::Stopped);
        // Confirmed by the context in the state; the deck shows its track.
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 5000);
        QCOMPARE(player.state(), QString("playing"));
        QCOMPARE(player.title(), QString("Tail Lights"));
        QCOMPARE(player.artist(), QString("Lunar Drive"));
        // Spotify moves on to the playlist's next track: the deck follows.
        const QString uri = "spotify:playlist:" + synth;
        fake.setPlayback(harbor, 1000, true, uri);
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Harbor Lights"), 5000);
        QCOMPARE(player.duration(), qint64(255000));
        QCOMPARE(player.currentIndex(), 0);
        // And stops at the end of the playlist: the next entry plays.
        fake.setPlayback(harbor, 255000, false, uri);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 1, 5000);
        QCOMPARE(engine->source(), dir.filePath("b.flac"));
        player.stop();
        QVERIFY(fake.calls().filter("get item").isEmpty());

        // Shuffle asks Spotify to shuffle; Play next queues it.
        fake.clearCalls();
        c.playView(true);
        QCOMPARE(player.playlist()->at(0).path, "spotify://playlist/" + synth + "?shuffle=1");
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start context --id " + synth + " playlist --shuffle"),
                                 5000);
        player.stop();
        c.enqueueView(false);
        QCOMPARE(player.playlist()->count(), 2);

        // The tracks only when asked, slowly, with the usual waiting.
        fake.clearCalls();
        c.showRestrictedTracks();
        QVERIFY(c.loading());
        QVERIFY(!c.viewRestricted());
        QVERIFY(c.viewNote().contains("shared connection"));
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 3, 8000);
        QCOMPARE(fake.calls().filter("get item").size(), 1);
    }

    void aRefusedPlaylistStopsWaitingAndIsRemembered() {
        FakeSpotify fake;
        QTemporaryDir dir;
        // Someone else's playlist that Spotify refuses your ID today.
        const QString retro = fake.id("playlist:Retro Futures");
        fake.touch(fake.state() + "/restricted", retro.toUtf8() + "\n");
        fake.touch(fake.state() + "/slow-refused", "5");
        {
            SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
            c.setEnabled(true);
            QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
            c.setViewVisible(true);
            QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
            QCOMPARE(c.items()->at(4).name, QString("Retro Futures"));
            QVERIFY(!c.isRestricted(c.items()->at(4)));
            QElapsedTimer t;
            t.start();
            c.openRow(4);
            QVERIFY(c.loading());
            // spotify_player's log says your ID was refused: Omamp stops
            // waiting on the shared ID and says why, long before the answer.
            QTRY_VERIFY_WITH_TIMEOUT(c.viewRestricted(), 3500);
            QVERIFY2(t.elapsed() < 3500, qPrintable(QString::number(t.elapsed())));
            QVERIFY(!c.loading());
            QVERIFY(c.viewNote().contains("wouldn't let your app"));
            QVERIFY(c.refused().contains("playlist:" + retro));
            c.waitForIdle();
            // Opened again: not asked again.
            c.back();
            fake.clearCalls();
            c.openRow(4);
            QVERIFY(c.viewRestricted());
            c.waitForIdle();
            QVERIFY(fake.calls().filter("get item").isEmpty());
        }
        // Remembered across runs, for this client ID and today.
        QVERIFY(QFile::exists(dir.filePath("art/refused.json")));
        SpotifyController again(dir.filePath("spotify.conf"), dir.filePath("art"));
        Spotify::Item item;
        item.kind = "playlist";
        item.id = retro;
        QVERIFY(again.isRestricted(item));
    }

    void aContextIsFollowedThroughMpris() {
        PrivateBus bus;
        if (!bus.ok())
            QSKIP("dbus-daemon isn't available");
        FakeSpotify fake;
        fake.set("OMAMP_SPOTIFY_MPRIS_BUS", bus.address);
        fake.set("OMAMP_SPOTIFY_MPRIS_NAME", "org.mpris.MediaPlayer2.spotify_player");
        FakeMpris mpris(bus.address, "org.mpris.MediaPlayer2.spotify_player");
        QVERIFY2(mpris.registered(), qPrintable(mpris.why()));
        QTemporaryDir dir;
        auto *engine = new FakeEngine;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.playback()->setTimings(100, 8000);
        Player player(new HandoffEngine(engine, c.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        c.setPlayer(&player);
        // Something else was playing before.
        mpris.show("Old Song", "Someone", "Old Album", 200000, "Playing", 5000);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(c.followMode(), QString("mpris"), 3000);
        c.setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        c.openRow(3);
        QVERIFY(c.viewRestricted());
        fake.clearCalls();
        c.playView(false);
        const QString synth = fake.id("playlist:Synthwave Essentials");
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start context --id " + synth + " playlist"), 5000);
        QTest::qWait(300);
        // The old song still shows: not ours yet.
        QVERIFY(player.buffering());
        mpris.show("Tail Lights", "Lunar Drive", "Night Ride Home", 221000, "Playing", 0);
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 3000);
        QCOMPARE(player.title(), QString("Tail Lights"));
        mpris.show("Harbor Lights", "Chrome Harbor", "Harbor Lights", 255000, "Playing", 0);
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Harbor Lights"), 3000);
        QCOMPARE(player.duration(), qint64(255000));
        QCOMPARE(stateReads(fake.calls()), 0);
        player.stop();
    }

    void anotherCatalogueReplacesTheWebApi() {
        // A source that may read Spotify's own playlists (as Spotify's own
        // apps do) replaces the Web API for artist pages and playlist
        // tracks; the views don't change.
        struct Shelf : Spotify::Catalogue {
            QString id() const override { return "test-shelf"; }
            bool listsQuickly(const Spotify::Item &) const override { return true; }
            QList<Spotify::Item> artistPage(const Spotify::Item &artist, QStringList *sections, QString *) override {
                Spotify::Item t;
                t.kind = "track";
                t.id = "shelfTrack0000000000001";
                t.name = "From the shelf";
                t.artists = {artist.name};
                if (sections)
                    *sections = {"Popular tracks"};
                return {t};
            }
            QList<Spotify::Item> playlistTracks(const Spotify::Item &, QString *) override {
                Spotify::Item t;
                t.kind = "track";
                t.id = "shelfTrack0000000000002";
                t.name = "Shelved";
                return {t};
            }
        };
        FakeSpotify fake;
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.backend()->setCatalogue(std::make_unique<Shelf>());
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        fake.clearCalls();
        c.openRow(3); // Spotify's own playlist lists now
        QVERIFY(!c.viewRestricted());
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 1, 5000);
        QCOMPARE(c.items()->at(0).name, QString("Shelved"));
        c.openSection("artists");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);
        c.openRow(0);
        QTRY_VERIFY_WITH_TIMEOUT(c.items()->count() == 1 && c.items()->at(0).name == "From the shelf", 5000);
        c.waitForIdle();
        QVERIFY2(fake.calls().filter("get item").isEmpty() && fake.calls().filter("search").isEmpty(),
                 qPrintable(fake.calls().join(" | ")));
    }

    void theSessionReaderReadsWhatTheWebApiCant() {
        FakeSpotify fake;
        fake.set("OMAMP_SPOTIFY_SESSION", QString(OMAMP_TESTS_DIR) + "/fake-spotify/omamp-spotify-session");
        QTemporaryDir dir;
        const QString synth = fake.id("playlist:Synthwave Essentials");
        // The Web API would refuse it and answer late.
        fake.touch(fake.state() + "/restricted", synth.toUtf8() + "\n");
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        QVERIFY(c.sessionReads());
        QVERIFY(c.sessionReaderInstalled());
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.setViewVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);

        // Spotify's own playlist lists at once, through the reader.
        fake.clearCalls();
        c.openRow(3);
        QVERIFY(!c.viewRestricted());
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 3, 5000);
        QVERIFY(c.viewError().isEmpty());
        QVERIFY(fake.calls().contains("session playlist " + synth));
        QVERIFY(fake.calls().filter("get item").isEmpty());

        // An artist: top tracks from the reader, albums from search.
        c.openSection("artists");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);
        int row = -1;
        for (int r = 0; r < c.items()->count(); ++r)
            if (c.items()->at(r).name == "Neon Coast")
                row = r;
        fake.clearCalls();
        c.openRow(row);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        QCOMPARE(c.items()->get(0).value("section").toString(), QString("Popular tracks"));
        QCOMPARE(c.items()->get(4).value("section").toString(), QString("Albums"));
        QVERIFY(c.viewNote().isEmpty());
        QVERIFY(fake.calls().contains("session artist-top " + fake.id("artist:Neon Coast")));
        QVERIFY(fake.calls().filter(QRegularExpression(" artist$")).filter("get item").isEmpty());
        // One reader process for all of it.
        QCOMPARE(c.sessionReader()->starts(), 1);

        // The reader failing: your own playlist falls back to the Web API;
        // Spotify's own says so instead of waiting on the shared ID.
        fake.touch(fake.state() + "/session-fail", "no connection");
        c.openSection("playlists");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        fake.clearCalls();
        c.openRow(0);
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        QVERIFY(!fake.calls().filter("get item --id " + fake.id("playlist:Late Night Drive")).isEmpty());
        c.back();
        c.openRow(3);
        c.reload();
        QTRY_VERIFY_WITH_TIMEOUT(!c.loading() && !c.viewError().isEmpty(), 5000);
        QVERIFY2(c.viewError().contains("no connection"), qPrintable(c.viewError()));
        // It still plays as a whole, and Retry reads it again.
        QVERIFY(c.viewRestricted());
        QFile::remove(fake.state() + "/session-fail");
        c.reload();
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 3, 5000);
        QVERIFY(c.viewError().isEmpty());

        // Switched off: back to playing it as a whole.
        c.setSessionReads(false);
        QVERIFY(c.sessionReader() == nullptr);
        c.back();
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 5, 5000);
        c.openRow(3);
        QVERIFY(c.viewRestricted());
        QVERIFY(Spotify::Settings::load(dir.filePath("spotify.conf")).sessionReads == false);

        // The command line reads through it too.
        Spotify::SpotifyPlayerBackend b;
        QString o, e;
        QTextStream out(&o), err(&e);
        fake.clearCalls();
        QCOMPARE(Cli::run({"spotify", "tracks", "spotify:playlist:" + synth, "--json"}, out, err), 0);
        QCOMPARE(QJsonDocument::fromJson(o.toUtf8()).object().value("items").toArray().size(), 3);
        QVERIFY(fake.calls().contains("session playlist " + synth));
    }

    void aReaderThatCantSignInFallsBack() {
        FakeSpotify fake;
        fake.set("OMAMP_SPOTIFY_SESSION", QString(OMAMP_TESTS_DIR) + "/fake-spotify/omamp-spotify-session");
        fake.touch(fake.state() + "/session-signin", "the sign-in has expired");
        QTemporaryDir dir;
        SpotifyController c(dir.filePath("spotify.conf"), dir.filePath("art"));
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(c.running() && !c.checking(), 8000);
        c.openSection("artists");
        QTRY_COMPARE_WITH_TIMEOUT(c.items()->count(), 6, 5000);
        c.openRow(0);
        // Search alone still gives the page.
        QTRY_VERIFY_WITH_TIMEOUT(!c.loading() && c.items()->count() > 0, 5000);
        QVERIFY(c.viewError().isEmpty());
    }
};

int runSpotifyTests(int argc, char **argv) {
    SpotifyTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "spotify_tests.moc"
