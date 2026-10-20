// Internet radio, without the internet: the built-in Nightride FM and Rekt
// Network channels, the Nightride feed, ICY titles in a stream, live
// playback (no length, no seeking), switching channels, favourites,
// fallback addresses, what is recorded and scrobbled, radio-browser.info
// search, station lists, the command line and the window's picker.
//
// radiohttp.h plays nightride.fm, its stream server and radio-browser.info
// on 127.0.0.1. Engines run into in-memory sinks (OMAMP_NO_AUDIO_DEVICE is
// set by main()), pulled faster than real time where a test needs minutes
// of radio.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "artprovider.h"
#include "cli.h"
#include "decoder.h"
#include "library.h"
#include "librarycontroller.h"
#include "mpris.h"
#include "nativeengine.h"
#include "nowplaying.h"
#include "nowplaying_context.h"
#include "pcmsink.h"
#include "player.h"
#include "playrecorder.h"
#include "radio.h"
#include "radiocontroller.h"
#include "radiohttp.h"
#include "streaming.h"
#include "theme.h"
#include "translator.h"

namespace {

QVariant sql(Library &library, const QString &text, const QVariantList &binds = {}) {
    QSqlQuery q(library.db());
    q.prepare(text);
    for (const QVariant &b : binds)
        q.addBindValue(b);
    if (!q.exec() || !q.next())
        return {};
    return q.value(0);
}

QByteArray feedRow(const QString &station, const QString &artist, const QString &title) {
    return QJsonDocument(QJsonArray{QJsonObject{{"station", station}, {"artist", artist}, {"title", title},
                                                {"track_id", "t-" + title}, {"at", 1792452430917.0}}})
        .toJson(QJsonDocument::Compact);
}

QObject *findItem(QQuickItem *item, const QString &name) {
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QObject *o = findItem(child, name))
            return o;
    return nullptr;
}

QStringList *qmlWarnings = nullptr;
QtMessageHandler previousHandler = nullptr;
void collect(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (qmlWarnings && type == QtWarningMsg && message.contains("qrc:/"))
        qmlWarnings->append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

// A player on the native engine, into a sink pulled at speed × real time.
struct Rig {
    BufferSink *sink;
    std::unique_ptr<Player> player;
    Rig(RadioController &radio, double speed) : sink(new BufferSink(48000, false)) {
        player.reset(new Player(new NativeEngine(sink), {},
                                [&radio](const QString &path, qint64, QString *error) { return radio.resolve(path, error); }));
        sink->setAutoPull(speed);
        radio.setPlayer(player.get());
    }
};

} // namespace

class RadioTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<RadioHttp> m_http;
    QByteArray m_oldCache, m_oldConfig;
    int m_dbs = 0;

    QString db() { return m_dir.filePath(QString("library-%1.db").arg(++m_dbs)); }
    QString lastDb() const { return m_dir.filePath(QString("library-%1.db").arg(m_dbs)); }

private slots:
    void initTestCase() {
        QVERIFY(qEnvironmentVariableIsSet("OMAMP_NO_AUDIO_DEVICE"));
        QVERIFY(m_dir.isValid());
        // Five seconds of two tones as a 128 kbps MP3: the station's audio.
        QProcess ff;
        ff.start("ffmpeg", {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                            "aevalsrc=0.4*sin(2*PI*440*t)|0.4*sin(2*PI*660*t):s=44100:d=5", "-c:a", "libmp3lame",
                            "-b:a", "128k", m_dir.filePath("tone.mp3")});
        QVERIFY(ff.waitForFinished(60000));
        QCOMPARE(ff.exitCode(), 0);
        QFile f(m_dir.filePath("tone.mp3"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray audio = f.readAll();

        m_http = std::make_unique<RadioHttp>();
        // Every Nightride channel streams the tone with ICY titles; the
        // feed knows what three of them play.
        for (const Radio::Station &s : Radio::builtinStations()) {
            RadioHttp::Live live;
            live.audio = audio;
            live.titles = {"Stream Artist - " + s.name};
            m_http->addLive(s.metaKey + ".mp3", live);
            m_http->addLive(s.metaKey + ".ogg", live);
        }
        RadioHttp::Live icy;
        icy.audio = audio;
        icy.titles = {"Alpha Artist - First Song", "Beta Artist - Second Song", "Gamma - Third"};
        icy.titleEvery = 40000; // a new title every 2.5 s of audio
        m_http->addLive("icy.mp3", icy);
        // Long songs, for scrobbling: 35 s of audio each.
        RadioHttp::Live slow = icy;
        slow.titles = {"Intro Artist - Already Playing", "Second Artist - Whole Song", "Third Artist - Next Song",
                       "Fourth Artist - Last Song"};
        slow.titleEvery = 16000 * 35;
        m_http->addLive("long.mp3", slow);
        RadioHttp::Live missing;
        missing.status = 404;
        m_http->addLive("missing.mp3", missing);
        m_http->setFeedSnapshot({feedRow("darksynth", "Grave Circuit", "Chrome Teeth"),
                                 feedRow("nightride", "Neon Harbour", "Coastline Drive"),
                                 feedRow("chillsynth", "Paper Satellites", "Slow Orbit")});

        qputenv("OMAMP_NIGHTRIDE_URL", m_http->base().toUtf8());
        qputenv("OMAMP_NIGHTRIDE_STREAM_URL", (m_http->base() + "/live").toUtf8());
        qputenv("OMAMP_RADIOBROWSER_URL", m_http->base().toUtf8());
        m_oldCache = qgetenv("XDG_CACHE_HOME");
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CACHE_HOME", m_dir.filePath("cache").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath("config").toUtf8());
        Streaming::Options o;
        o.rwTimeoutMs = 3000;
        o.reconnectDelayMaxS = 1;
        Streaming::setOptions(o);
    }

    void cleanupTestCase() {
        qunsetenv("OMAMP_NIGHTRIDE_URL");
        qunsetenv("OMAMP_NIGHTRIDE_STREAM_URL");
        qunsetenv("OMAMP_RADIOBROWSER_URL");
        qputenv("XDG_CACHE_HOME", m_oldCache);
        qputenv("XDG_CONFIG_HOME", m_oldConfig);
        if (m_oldCache.isEmpty())
            qunsetenv("XDG_CACHE_HOME");
        if (m_oldConfig.isEmpty())
            qunsetenv("XDG_CONFIG_HOME");
        Streaming::setOptions({});
        m_http.reset();
    }

    // ---- The stations ------------------------------------------------------

    void seedsEveryChannel() {
        Library library(db());
        QVERIFY(library.open());
        // The radio step (10) is in; later steps may follow it.
        QVERIFY(library.schemaVersion() >= 10);
        Radio::Stations stations(library);
        QVERIFY(stations.seed());
        QCOMPARE(stations.all().size(), 11);
        QStringList nightride, rekt;
        for (const Radio::Station &s : stations.group("nightride"))
            nightride << s.metaKey;
        for (const Radio::Station &s : stations.group("rekt"))
            rekt << s.metaKey;
        QCOMPARE(nightride, QStringList({"nightride", "chillsynth", "datawave", "spacesynth", "darksynth",
                                         "horrorsynth", "ebsm"}));
        QCOMPARE(rekt, QStringList({"rekt", "rektory", "rektify", "d-notive"}));
        const Radio::Station dark = stations.byKey("nightride/darksynth");
        QCOMPARE(dark.fullName(), QString("Nightride FM: Darksynth"));
        QCOMPARE(dark.streams, QStringList({m_http->base() + "/live/darksynth.mp3", m_http->base() + "/live/darksynth.ogg"}));
        QCOMPARE(dark.metaKind, QString("nightride"));
        QCOMPARE(stations.byKey("nightride/nightride").fullName(), QString("Nightride FM"));
        QCOMPARE(stations.byKey("nightride/rektory").fullName(), QString("Rekt Network: Rektory"));

        // Seeding again keeps favourites and listening time, and a changed
        // stream server is written over the old addresses.
        QVERIFY(stations.setFavourite("nightride/darksynth", true));
        stations.addListened("nightride/darksynth", 90000);
        qputenv("OMAMP_NIGHTRIDE_STREAM_URL", "http://127.0.0.1:1/elsewhere");
        QVERIFY(stations.seed());
        QCOMPARE(stations.byKey("nightride/darksynth").streams.first(), QString("http://127.0.0.1:1/elsewhere/darksynth.mp3"));
        qputenv("OMAMP_NIGHTRIDE_STREAM_URL", (m_http->base() + "/live").toUtf8());
        QVERIFY(stations.seed());
        QCOMPARE(stations.all().size(), 11);
        QVERIFY(stations.byKey("nightride/darksynth").favourite);
        QCOMPARE(stations.byKey("nightride/darksynth").listenedMs, qint64(90000));
        QCOMPARE(stations.group("favourites").size(), 1);

        // Finding stations as people name them.
        QCOMPARE(stations.find("darksynth").key, QString("nightride/darksynth"));
        QCOMPARE(stations.find("radio://nightride/ebsm").key, QString("nightride/ebsm"));
        QCOMPARE(stations.find("Nightride FM: Horrorsynth").key, QString("nightride/horrorsynth"));
        QCOMPARE(stations.find("rektify").key, QString("nightride/rektify"));
        QStringList which;
        QVERIFY(!stations.find("synth", &which).isValid());
        QVERIFY(which.size() > 2);
        QCOMPARE(stations.find("chill").key, QString("nightride/chillsynth"));
    }

    void parsesFeedPageAndIcy() {
        // The feed arrives in pieces, with CRLF and keepalives.
        Radio::SseParser sse;
        QList<QByteArray> events = sse.feed("data: [{\"station\":\"darksynth\",\"artist\":\"A\",\"ti");
        QVERIFY(events.isEmpty());
        events = sse.feed("tle\":\"B\"}]\r\n\r\ndata: keepalive\n\n: a comment\n\ndata: {\"station\":\"ebsm\",\"title\":\"Solo\"}\n\n");
        QCOMPARE(events.size(), 2);
        QList<Radio::FeedRow> rows = Radio::parseFeedData(events[0]);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].station, QString("darksynth"));
        QCOMPARE(rows[0].line(), QString("A – B"));
        rows = Radio::parseFeedData(events[1]);
        QCOMPARE(rows[0].line(), QString("Solo"));
        // A DJ on air is left out, as the site does.
        QVERIFY(Radio::parseFeedData("[{\"station\":\"nightride\",\"live\":true,\"title\":\"x\"}]").isEmpty());
        QVERIFY(Radio::parseFeedData("not json").isEmpty());

        // nightride.fm's station list, as the page has it.
        const QString html = "<nav>\n"
            "            <a class=\"station\" data-station=\"nightride\" data-type=\"stream\" href=\"?station=nightride\" title=\"x\">\n"
            "                <h3>NIGHTRIDE FM</h3>\n"
            "                <h4>Synthwave / Retrowave / Outrun</h4>\n"
            "            </a>\n"
            "            <a class=\"station\" data-station=\"darksynth\" data-type=\"stream\" href=\"?station=darksynth\">\n"
            "                <h3>DARKSYNTH</h3><h4>Darksynth / Cyberpunk / Synthmetal</h4></a>\n"
            "            <a class=\"station\" data-station=\"archives\" data-type=\"file\" href=\"/archive\"><h3>ARCHIVES</h3></a>\n"
            "        </nav>";
        const QList<Radio::CatalogEntry> site = Radio::parseNightrideHomepage(html);
        QCOMPARE(site.size(), 2);
        QCOMPARE(site[0].id, QString("nightride"));
        QCOMPARE(site[0].name, QString("Nightride FM"));
        QCOMPARE(site[1].name, QString("Darksynth"));
        QCOMPARE(site[1].description, QString("Darksynth / Cyberpunk / Synthmetal"));

        // ICY titles.
        QCOMPARE(Radio::parseStreamTitle("StreamTitle='Akyut - Heaven's Shadow';StreamUrl='';\0\0"),
                 QString("Akyut - Heaven's Shadow"));
        QCOMPARE(Radio::parseStreamTitle(QByteArray("StreamTitle='Caf\xe9 Del Mar';")), QString("Café Del Mar"));
        QCOMPARE(Radio::parseStreamTitle("StreamUrl='x';"), QString());
        QString artist, title;
        Radio::splitStreamTitle("Akyut - Heaven's Shadow", &artist, &title);
        QCOMPARE(artist, QString("Akyut"));
        QCOMPARE(title, QString("Heaven's Shadow"));
        Radio::splitStreamTitle("Station jingle", &artist, &title);
        QVERIFY(artist.isEmpty());
        QCOMPARE(title, QString("Station jingle"));

        // The ICY parser, fed a byte at a time.
        QByteArray stream;
        auto block = [](const QByteArray &meta) {
            QByteArray m = meta;
            const int n = int((m.size() + 15) / 16);
            m.resize(n * 16, '\0');
            return QByteArray(1, char(n)) + m;
        };
        stream += QByteArray(10, 'a') + block("StreamTitle='One - First';") + QByteArray(10, 'b') + block("") +
                  QByteArray(10, 'c') + block("StreamTitle='Two - Second';") + QByteArray(4, 'd');
        Radio::IcyParser icy(10);
        QStringList titles;
        for (char c : stream)
            titles += icy.feed(QByteArray(1, c));
        QCOMPARE(titles, QStringList({"One - First", "Two - Second"}));
    }

    void catalogueGrows() {
        Library library(db());
        QVERIFY(library.open());
        Radio::Stations stations(library);
        QVERIFY(stations.seed());
        // A channel new on the site goes with Nightride FM; one only the
        // feed names goes with Rekt Network; nothing known changes.
        const QList<Radio::CatalogEntry> site = {{"nightride", "Nightride FM", "Synthwave"},
                                                 {"testwave", "Testwave FM", "Test / Wave"},
                                                 {"archives", "Archives", ""}};
        QCOMPARE(stations.mergeNightrideCatalog(site, {"darksynth", "testwave", "new-rekt"}), 2);
        const Radio::Station testwave = stations.byKey("nightride/testwave");
        QCOMPARE(testwave.network, QString("nightride"));
        QCOMPARE(testwave.name, QString("Testwave FM"));
        QCOMPARE(testwave.description, QString("Test / Wave"));
        QCOMPARE(testwave.streams.first(), m_http->base() + "/live/testwave.mp3");
        QCOMPARE(stations.byKey("nightride/new-rekt").network, QString("rekt"));
        QCOMPARE(stations.byKey("nightride/new-rekt").name, QString("New Rekt"));
        QVERIFY(!stations.byKey("nightride/archives").isValid());
        QCOMPARE(stations.byKey("nightride/nightride").description, QString("Synthwave / Retrowave / Outrun"));
        QCOMPARE(stations.mergeNightrideCatalog(site, {"new-rekt"}), 0);
        // Seeding later doesn't lose them.
        QVERIFY(stations.seed());
        QCOMPARE(stations.all().size(), 13);
    }

    void addFindRemoveAndLists() {
        Library library(db());
        QVERIFY(library.open());
        Radio::Stations stations(library);
        QVERIFY(stations.seed());
        Radio::Station s;
        s.name = "Example FM";
        s.streams = {"http://radio.example/live.mp3"};
        QString error;
        const Radio::Station added = stations.add(s, &error);
        QVERIFY2(added.isValid(), qPrintable(error));
        QCOMPARE(added.key, QString("my/1"));
        QCOMPARE(added.network, QString("custom"));
        QCOMPARE(added.group(), QString("mine"));
        // The same address again is the same station; Nightride's own
        // address is Nightride's channel.
        bool existed = false;
        QCOMPARE(stations.add(s, &error, &existed).key, QString("my/1"));
        QVERIFY(existed);
        Radio::Station nr;
        nr.streams = {m_http->base() + "/live/darksynth.mp3"};
        QCOMPARE(stations.add(nr, &error, &existed).key, QString("nightride/darksynth"));
        QVERIFY(existed);
        Radio::Station bad;
        bad.streams = {"ftp://nope"};
        QVERIFY(!stations.add(bad, &error).isValid());
        QVERIFY(error.contains("http"));
        // Built-in channels stay.
        QVERIFY(!stations.remove("nightride/ebsm", &error));
        QVERIFY(stations.rename("my/1", "Example Radio"));
        QVERIFY(!stations.rename("nightride/ebsm", "Mine now"));
        QCOMPARE(stations.group("mine").size(), 1);

        // Lists out and in: PLS and M3U.
        const QString pls = m_dir.filePath("stations.pls"), m3u = m_dir.filePath("stations.m3u8");
        QVERIFY(stations.exportFile(pls, "all", &error));
        QVERIFY(stations.exportFile(m3u, "nightride", &error));
        QFile f(pls);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY(text.startsWith("[playlist]"));
        QVERIFY(text.contains("Title5=Nightride FM: Darksynth"));
        QVERIFY(text.contains("NumberOfEntries=12"));
        QVERIFY(stations.remove("my/1", &error));
        QCOMPARE(stations.group("mine").size(), 0);
        QCOMPARE(stations.importFile(pls, &error), 1); // Example Radio comes back; the channels are known
        QCOMPARE(stations.group("mine").first().name, QString("Example Radio"));
        QCOMPARE(stations.importFile(m3u, &error), 0);
        QFile other(m_dir.filePath("other.m3u"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write("#EXTM3U\n#EXTINF:-1,Somewhere FM\nhttps://somewhere.example/stream\n/home/me/song.mp3\n");
        other.close();
        QCOMPARE(stations.importFile(other.fileName(), &error), 1);
        QCOMPARE(stations.find("somewhere").streams.first(), QString("https://somewhere.example/stream"));
    }

    // ---- The engine ----------------------------------------------------------

    void decoderReadsIcyTitles() {
        Decoder d;
        QString error;
        QVERIFY2(d.open("radio://my/1", QUrl(m_http->liveUrl("icy.mp3")), &error), qPrintable(error));
        QVERIFY(d.isLive());
        QCOMPARE(d.durationMs(), qint64(0));
        QVERIFY(!d.seek(1000));
        QStringList seen;
        float buf[4096 * 2];
        QElapsedTimer clock;
        clock.start();
        while (seen.size() < 2 && clock.elapsed() < 20000) {
            QVERIFY(d.read(buf, 4096) > 0);
            if (d.takeStreamTitleChanged())
                seen << d.streamTitle();
        }
        QCOMPARE(seen, QStringList({"Alpha Artist - First Song", "Beta Artist - Second Song"}));
        // The server was asked for titles, by Omamp.
        bool asked = false;
        for (const RadioHttp::Request &r : m_http->requests())
            if (r.path == "/live/icy.mp3")
                asked = r.headers.value("icy-metadata") == "1" && r.headers.value("user-agent").startsWith("Omamp/");
        QVERIFY(asked);
        // A file never asks.
        Decoder file;
        QVERIFY(file.open(m_dir.filePath("tone.mp3")));
        QVERIFY(!file.isLive());
        QVERIFY(file.durationMs() > 4000);
    }

    // ---- Tuning in -----------------------------------------------------------

    void nightrideLive() {
        RadioController radio(db());
        radio.setCacheDir(m_dir.filePath("tiles"));
        Rig rig(radio, 4.0);
        Player &player = *rig.player;
        NowPlaying now(&player, lastDb(), m_dir.filePath("art"));
        QObject host;
        auto *mpris = new MprisPlayer(&host, &player, &now);

        // Something in the queue already: the station goes after it.
        Playlist::Track song = Playlist::trackFromFile(m_dir.filePath("tone.mp3"));
        player.appendTracks({song});
        QVERIFY(radio.tune("darksynth"));
        QCOMPARE(player.playlist()->count(), 2);
        QCOMPARE(player.currentIndex(), 1);
        QVERIFY(player.live());
        QCOMPARE(radio.currentKey(), QString("nightride/darksynth"));
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("playing"), 10000);
        // The feed's song shows as the title, the station as the album.
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Chrome Teeth"), 10000);
        QCOMPARE(player.artist(), QString("Grave Circuit"));
        QCOMPARE(player.album(), QString("Nightride FM: Darksynth"));
        QCOMPARE(radio.nowPlaying(), QString("Grave Circuit – Chrome Teeth"));
        QVERIFY(radio.feedConnected());
        QCOMPARE(radio.nowPlayingFor("nightride/chillsynth"), QString("Paper Satellites – Slow Orbit"));
        // The queue row keeps the station's name.
        QCOMPARE(player.playlist()->at(1).title, QString("Darksynth"));

        // LIVE: no length, no seeking, the clock counts listening time.
        QCOMPARE(player.duration(), qint64(0));
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 2000, 10000);
        QSignalSpy seeked(&player, &Player::seeked);
        const qint64 before = player.position();
        player.seek(before + 60000);
        player.seekBy(-1000);
        QCOMPARE(seeked.count(), 0);
        QVERIFY(player.position() >= before);
        QVERIFY(player.position() < before + 30000);

        // MPRIS: no length, no seeking, the queue reference as the address,
        // and the station's picture.
        QTRY_VERIFY_WITH_TIMEOUT(now.hasArt() && !now.loading(), 15000);
        QCOMPARE(now.artSource(), QString("radio"));
        QVariantMap meta = mpris->metadata();
        QVERIFY(!meta.contains("mpris:length"));
        QCOMPARE(meta.value("xesam:title").toString(), QString("Chrome Teeth"));
        QCOMPARE(meta.value("xesam:artist").toStringList(), QStringList({"Grave Circuit"}));
        QCOMPARE(meta.value("xesam:album").toString(), QString("Nightride FM: Darksynth"));
        QCOMPARE(meta.value("xesam:url").toString(), QString("radio://nightride/darksynth"));
        QVERIFY(meta.value("mpris:artUrl").toString().startsWith("file://"));
        QVERIFY(!mpris->canSeek());
        QVERIFY(mpris->canPlay());

        // A new song on the feed: everywhere at once, and remembered.
        QSignalSpy songs(&radio, &RadioController::songChanged);
        m_http->push(feedRow("darksynth", "Volkor X", "Masked Death"));
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Masked Death"), 10000);
        QCOMPARE(songs.count(), 1);
        QTRY_COMPARE(mpris->metadata().value("xesam:title").toString(), QString("Masked Death"));
        Library &library = *radio.library();
        QCOMPARE(sql(library, "SELECT count(*) FROM radio_heard").toInt(), 2);
        QCOMPARE(sql(library, "SELECT from_start FROM radio_heard WHERE title = 'Chrome Teeth'").toInt(), 0);
        QCOMPARE(sql(library, "SELECT from_start FROM radio_heard WHERE title = 'Masked Death'").toInt(), 1);
        // Another channel's song changes only its line.
        m_http->push(feedRow("nightride", "Someone", "Else"));
        QTRY_COMPARE(radio.nowPlayingFor("nightride/nightride"), QString("Someone – Else"));
        QCOMPARE(player.title(), QString("Masked Death"));

        // ] : the next channel takes the station's place in the queue.
        QVERIFY(radio.step(1));
        QCOMPARE(radio.currentKey(), QString("nightride/horrorsynth"));
        QCOMPARE(player.playlist()->count(), 2);
        QCOMPARE(player.playlist()->at(player.currentIndex()).path, QString("radio://nightride/horrorsynth"));
        QVERIFY(sql(library, "SELECT listened_ms FROM radio_stations WHERE key = 'nightride/darksynth'").toLongLong() > 2000);
        QVERIFY(sql(library, "SELECT last_played FROM radio_stations WHERE key = 'nightride/horrorsynth'").toLongLong() > 0);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), QString("playing"), 10000);
        // Horrorsynth isn't in the feed yet: its stream's own title stands in.
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Horrorsynth"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(player.artist(), QString("Stream Artist"), 10000);
        // ...until the feed speaks.
        m_http->push(feedRow("horrorsynth", "The Hollow Coven", "Lantern Hymn"));
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Lantern Hymn"), 10000);
        // Round the group: ebsm, then back to the first.
        QVERIFY(radio.step(1));
        QVERIFY(radio.step(1));
        QCOMPARE(radio.currentKey(), QString("nightride/nightride"));
        QVERIFY(radio.step(-1));
        QCOMPARE(radio.currentKey(), QString("nightride/ebsm"));
        QCOMPARE(player.playlist()->count(), 2);

        // Favourites.
        QVERIFY(radio.toggleFavourite("nightride/ebsm"));
        QVERIFY(radio.current().value("favourite").toBool());
        radio.setGroup("favourites");
        QCOMPARE(radio.stations()->count(), 1);
        QVERIFY(radio.toggleFavourite("nightride/ebsm"));
        QCOMPARE(radio.stations()->count(), 0);

        // Leaving the radio for a track: radio is off, the clock is a track's.
        player.playIndex(0);
        QVERIFY(!player.live());
        QVERIFY(!radio.active());
        QVERIFY(player.duration() > 0 || QTest::qWaitFor([&] { return player.duration() > 0; }, 5000));
        player.stop();
        // Radio is never a library play.
        QCOMPARE(sql(library, "SELECT count(*) FROM play_history").toInt(), 0);
    }

    void icyStationsAndFallbacks() {
        RadioController radio(db());
        radio.setCacheDir(m_dir.filePath("tiles"));
        Rig rig(radio, 4.0);
        Player &player = *rig.player;
        QSignalSpy toasts(&radio, &RadioController::toast);

        // An address of one's own, read for its ICY titles by the native engine.
        radio.addUrl(m_http->liveUrl("icy.mp3"), "Test ICY", true);
        QCOMPARE(radio.currentKey(), QString("my/1"));
        QCOMPARE(radio.current().value("fullName").toString(), QString("Test ICY"));
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("First Song"), 10000);
        QCOMPARE(player.artist(), QString("Alpha Artist"));
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Second Song"), 15000);
        QCOMPARE(player.album(), QString("Test ICY"));

        // The first address fails: the second plays, without a message and
        // without moving on in the queue.
        Library &library = *radio.library();
        Radio::Station two;
        two.name = "Two Addresses";
        two.streams = {m_http->liveUrl("missing.mp3"), m_http->liveUrl("icy.mp3") + "?second"};
        QVERIFY(Radio::Stations(library).add(two).isValid());
        radio.reload();
        QVERIFY(radio.tune("Two Addresses"));
        const int row = player.currentIndex();
        QTRY_VERIFY_WITH_TIMEOUT(player.state() == "playing" && player.position() > 1000, 15000);
        QCOMPARE(player.currentIndex(), row);
        QVERIFY(player.error().isEmpty());
        QVERIFY(m_http->count("/live/missing.mp3") >= 1);

        // Every address fails: a message, stopped, still on the station.
        Radio::Station none;
        none.name = "Gone FM";
        none.streams = {m_http->liveUrl("missing.mp3") + "?a", m_http->liveUrl("missing.mp3") + "?b"};
        QVERIFY(Radio::Stations(library).add(none).isValid());
        QVERIFY(radio.tune("Gone FM"));
        QTRY_VERIFY_WITH_TIMEOUT(player.error().startsWith("Can't play Gone FM"), 15000);
        QCOMPARE(player.state(), QString("stopped"));
        QCOMPARE(player.playlist()->at(player.currentIndex()).path, QString("radio://my/3"));
        // Only one radio row ever.
        QCOMPARE(player.playlist()->count(), 1);

        // Only stream addresses.
        radio.addUrl("ftp://not.radio/x", {}, false);
        QTRY_VERIFY(!toasts.isEmpty());
        QVERIFY(toasts.last().first().toString().contains("http"));
    }

    void scrobblingIsOptional() {
        RadioController radio(db());
        radio.setCacheDir(m_dir.filePath("tiles"));
        // Sixteen times real time: three 35 s songs in a few seconds.
        Rig rig(radio, 16.0);
        Player &player = *rig.player;
        PlayRecorder recorder(lastDb());
        recorder.setFlushing(false);
        recorder.attach(&player);
        Library &library = *radio.library();
        library.setSetting("listenbrainz.token", "test-token");
        QVERIFY(!radio.scrobble());
        QSignalSpy queued(&radio, &RadioController::scrobbleQueued);

        radio.addUrl(m_http->liveUrl("long.mp3"), "Long Songs", true);
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Already Playing"), 10000);
        // Heard from its start, but scrobbling is off.
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Whole Song"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Next Song"), 30000);
        QCOMPARE(sql(library, "SELECT count(*) FROM scrobble_queue").toInt(), 0);
        QCOMPARE(queued.count(), 0);
        // On: the next whole song goes to the queue, once.
        radio.setScrobble(true);
        QVERIFY(radio.scrobble());
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Last Song"), 30000);
        QTRY_COMPARE(queued.count(), 1);
        QCOMPARE(sql(library, "SELECT count(*) FROM scrobble_queue").toInt(), 1);
        const QString payload = sql(library, "SELECT payload FROM scrobble_queue").toString();
        QVERIFY2(payload.contains("Third Artist") && payload.contains("Next Song"), qPrintable(payload));
        // The song playing when tuned in was never whole.
        QCOMPARE(sql(library, "SELECT from_start FROM radio_heard WHERE title = 'Already Playing'").toInt(), 0);
        QVERIFY(sql(library, "SELECT heard_ms FROM radio_heard WHERE title = 'Whole Song'").toLongLong() >= 30000);
        player.stop();
        QCOMPARE(sql(library, "SELECT count(*) FROM play_history").toInt(), 0);
    }

    // ---- radio-browser.info --------------------------------------------------

    void findStations() {
        m_http->setSearchAnswer(QString("[\n"
            "            {\"stationuuid\":\"aaaa-1\",\"name\":\"Harbour Jazz\",\"url_resolved\":\"%1\",\"url\":\"%1\",\"homepage\":\"https://jazz.example\",\n"
            "             \"favicon\":\"%2/pic.png\",\"tags\":\"jazz,swing\",\"countrycode\":\"NZ\",\"codec\":\"MP3\",\"bitrate\":192},\n"
            "            {\"stationuuid\":\"aaaa-2\",\"name\":\"Nightride via the directory\",\"url_resolved\":\"%3\",\"url\":\"\",\"favicon\":\"\",\n"
            "             \"tags\":\"synthwave\",\"countrycode\":\"AU\",\"codec\":\"MP3\",\"bitrate\":320},\n"
            "            {\"stationuuid\":\"\",\"name\":\"No id\",\"url_resolved\":\"http://x\"}]")
                                    .arg(m_http->liveUrl("icy.mp3") + "?jazz", m_http->base(),
                                         m_http->base() + "/live/nightride.mp3")
                                    .toUtf8());
        RadioController radio(db());
        radio.setCacheDir(m_dir.filePath("tiles"));
        Rig rig(radio, 4.0);
        radio.search("jazz radio");
        QVERIFY(radio.searching());
        QTRY_VERIFY_WITH_TIMEOUT(!radio.searching(), 10000);
        QVERIFY2(radio.searchError().isEmpty(), qPrintable(radio.searchError()));
        QCOMPARE(radio.results().size(), 2);
        QVariantMap first = radio.results().first().toMap();
        QCOMPARE(first.value("name").toString(), QString("Harbour Jazz"));
        QCOMPARE(first.value("country").toString(), QString("NZ"));
        QVERIFY(!first.value("known").toBool());
        // The second is Nightride's own stream: already known.
        QVERIFY(radio.results().at(1).toMap().value("known").toBool());
        bool asked = false;
        for (const RadioHttp::Request &r : m_http->requests())
            if (r.path == "/json/stations/search")
                asked = QUrl::fromPercentEncoding(r.query).contains("name=jazz radio") && r.query.contains("hidebroken=true");
        QVERIFY(asked);

        // Add it: under My stations, with its picture fetched.
        const QString key = radio.addResult(0, false);
        QCOMPARE(key, QString("rb/aaaa-1"));
        QVERIFY(radio.results().first().toMap().value("known").toBool());
        Library &library = *radio.library();
        QTRY_VERIFY_WITH_TIMEOUT(!sql(library, "SELECT art_path FROM radio_stations WHERE key = 'rb/aaaa-1'").toString().isEmpty(), 10000);
        radio.setGroup("mine");
        QCOMPARE(radio.stations()->count(), 1);
        QCOMPARE(radio.stations()->data(radio.stations()->index(0), RadioStationsModel::NetworkNameRole).toString(),
                 QString("radio-browser.info"));
        // Playing it tells the directory, as it asks.
        const int clicks = m_http->count("/json/url/");
        QVERIFY(radio.tune(key));
        QTRY_COMPARE_WITH_TIMEOUT(m_http->count("/json/url/"), clicks + 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.player->title(), QString("First Song"), 15000);
        rig.player->stop();
    }

    void controllerRefreshesTheCatalogue() {
        m_http->setHomepage("<a class=\"station\" data-station=\"nightride\" data-type=\"stream\"><h3>NIGHTRIDE FM</h3><h4>x</h4></a>\n"
            "                               <a class=\"station\" data-station=\"moonwave\" data-type=\"stream\"><h3>MOONWAVE FM</h3><h4>Moon / Wave</h4></a>");
        RadioController radio(db());
        radio.setCacheDir(m_dir.filePath("tiles"));
        QSignalSpy toasts(&radio, &RadioController::toast);
        // The picker opens: the feed connects, names a channel Omamp doesn't
        // know, and the catalogue is read again.
        m_http->push(feedRow("brand-new", "Someone", "Something"));
        radio.setPickerVisible(true);
        QTRY_VERIFY_WITH_TIMEOUT(radio.feedConnected(), 10000);
        m_http->push(feedRow("brand-new", "Someone", "Something else"));
        QTRY_VERIFY_WITH_TIMEOUT(Radio::Stations(*radio.library()).byKey("nightride/moonwave").isValid(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(Radio::Stations(*radio.library()).byKey("nightride/brand-new").isValid(), 10000);
        QCOMPARE(Radio::Stations(*radio.library()).byKey("nightride/moonwave").network, QString("nightride"));
        QCOMPARE(Radio::Stations(*radio.library()).byKey("nightride/brand-new").network, QString("rekt"));
        QTRY_VERIFY(!toasts.isEmpty());
        // Picker closed, nothing playing: the feed lets go.
        radio.setPickerVisible(false);
        QVERIFY(!radio.feedConnected());
    }

    // ---- The command line ------------------------------------------------------

    void commandLine() {
        const QString file = db();
        auto run = [&](QStringList args, QString *output, QString *errors = nullptr) {
            QString o, e;
            QTextStream out(&o), err(&e);
            args << "--db" << file;
            const int code = Cli::run(args, out, err);
            out.flush();
            err.flush();
            *output = o;
            if (errors)
                *errors = e;
            return code;
        };
        QVERIFY(Cli::isCommand("radio"));
        QString out, err;
        QCOMPARE(run({"radio", "list", "--json"}, &out), 0);
        QJsonObject j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j.value("stations").toArray().size(), 11);
        QCOMPARE(run({"radio", "list", "--group", "rekt"}, &out), 0);
        QVERIFY(out.contains("Rekt Network: Rektory"));
        QVERIFY(!out.contains("Darksynth"));
        QCOMPARE(run({"radio", "list", "--group", "nope"}, &out, &err), 1);

        // play hands radio://<key> to the player (here, a fake).
        QStringList launched;
        Cli::setRadioLauncher([&](const QString &uri, QString *how) {
            launched << uri;
            *how = "running";
            return true;
        });
        QCOMPARE(run({"radio", "play", "darksynth", "--json"}, &out), 0);
        QCOMPARE(launched, QStringList({"radio://nightride/darksynth"}));
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j.value("station").toObject().value("full_name").toString(), QString("Nightride FM: Darksynth"));
        QCOMPARE(j.value("via").toString(), QString("running"));
        QCOMPARE(run({"radio", "play", "Nightride", "FM:", "EBSM"}, &out), 0);
        QVERIFY(out.contains("Tuning in to Nightride FM: EBSM"));
        QCOMPARE(run({"radio", "play", "synth"}, &out, &err), 1);
        QVERIFY(err.contains("could be"));
        QCOMPARE(run({"radio", "play", "no such station"}, &out, &err), 1);

        // search (the pretend directory), add, favourite, remove.
        m_http->setSearchAnswer("[{\"stationuuid\":\"cli-1\",\"name\":\"CLI Radio\",\"url_resolved\":\"http://cli.example/s.mp3\",\"codec\":\"AAC\",\"bitrate\":64,\"countrycode\":\"SE\"}]");
        QCOMPARE(run({"radio", "search", "cli", "--json"}, &out), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j.value("stations").toArray().first().toObject().value("name").toString(), QString("CLI Radio"));
        QCOMPARE(j.value("stations").toArray().first().toObject().value("known").toBool(), false);
        QCOMPARE(run({"radio", "search", "cli"}, &out), 0);
        QVERIFY(out.contains("AAC 64 kbps · SE"));
        QCOMPARE(run({"radio", "add", "http://cli.example/s.mp3", "--name", "CLI Radio", "--json"}, &out), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j.value("station").toObject().value("key").toString(), QString("my/1"));
        QCOMPARE(run({"radio", "add", "not a url"}, &out, &err), 1);
        QCOMPARE(run({"radio", "favourite", "CLI Radio"}, &out), 0);
        QCOMPARE(run({"radio", "list", "--group", "favourites", "--json"}, &out), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("stations").toArray().size(), 1);
        const QString list = m_dir.filePath("cli.m3u8");
        QCOMPARE(run({"radio", "export", list, "--group", "mine"}, &out), 0);
        QCOMPARE(run({"radio", "remove", "CLI Radio"}, &out), 0);
        QCOMPARE(run({"radio", "remove", "darksynth"}, &out, &err), 1);
        QCOMPARE(run({"radio", "import", list, "--json"}, &out), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("added").toInt(), 1);
        QCOMPARE(run({"radio", "frobnicate"}, &out, &err), 1);
    }

    // ---- The window ----------------------------------------------------------

    void pickerInTheWindow() {
        QTemporaryDir dir;
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", false);
        }
        QStringList warnings;
        qmlWarnings = &warnings;
        previousHandler = qInstallMessageHandler(collect);

        Translator translator(dir.filePath("language"));
        Theme theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl"));
        BufferSink *sink = new BufferSink(48000, false);
        RadioController *radioPtr = nullptr;
        Player player(new NativeEngine(sink), {},
                      [&radioPtr](const QString &path, qint64, QString *error) { return radioPtr->resolve(path, error); });
        sink->setAutoPull(2.0);
        LibraryController library(dir.filePath("library.db"));
        library.setPlayer(&player);
        library.setWatching(false);
        library.setArtCacheDir(dir.filePath("art"));
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
        auto *radio = engine.rootContext()->contextProperty("radio").value<RadioController *>();
        QVERIFY(radio);
        radioPtr = radio;
        engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider(dir.filePath("library.db"), dir.filePath("art")));
        engine.load(QUrl("qrc:/Main.qml"));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto find = [&](const QString &name) {
            QQuickItem *top = window->contentItem()->parentItem() ? window->contentItem()->parentItem() : window->contentItem();
            return findItem(top, name);
        };
        // Before the radio is used there is no library file.
        QVERIFY(!QFileInfo::exists(dir.filePath("library.db")));
        // Every channel is in the palette by name.
        QVERIFY(library.palette()->actionIds().contains("radio:nightride/darksynth"));
        QVERIFY(library.palette()->actionIds().contains("radio:nightride/d-notive"));

        // Shift+R: the picker, Nightride FM first, every channel's song.
        QTest::keyClick(window, Qt::Key_R, Qt::ShiftModifier);
        QObject *sheet = nullptr;
        // A sheet is a Popup, not an item: found among the window's objects.
        QTRY_VERIFY((sheet = window->findChild<QObject *>("radioSheet")) && sheet->property("opened").toBool());
        QCOMPARE(radio->group(), QString("nightride"));
        QTRY_COMPARE(radio->stations()->count(), 7);
        QTRY_VERIFY_WITH_TIMEOUT(radio->feedConnected(), 10000);
        QObject *line = nullptr;
        QTRY_VERIFY((line = find("nowPlaying_nightride/darksynth")) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(line->property("text").toString().contains("Grave Circuit"), 10000);
        // R alone still means repeat, not the radio.
        QVERIFY(!player.repeat());

        // Down, Right, Enter: tune in from the keyboard; the sheet closes.
        QObject *grid = find("radioGrid");
        QVERIFY(grid);
        QTRY_VERIFY(grid->property("activeFocus").toBool());
        QTRY_COMPARE(grid->property("currentIndex").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(grid->property("currentIndex").toInt(), 1);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(radio->currentKey(), QString("nightride/chillsynth"));
        QTRY_VERIFY(!sheet->property("opened").toBool());
        QVERIFY(player.live());
        QTRY_COMPARE_WITH_TIMEOUT(player.title(), QString("Slow Orbit"), 10000);
        // The deck says LIVE and has the dial instead of the position.
        QObject *badge = find("deckLiveBadge");
        QObject *dial = find("deckLiveBar");
        QVERIFY(badge && dial);
        QVERIFY(badge->property("visible").toBool());
        QVERIFY(dial->property("visible").toBool());

        // ] and [ change channel while one plays.
        QTest::keyClick(window, Qt::Key_BracketRight);
        QCOMPARE(radio->currentKey(), QString("nightride/datawave"));
        QTest::keyClick(window, Qt::Key_BracketLeft);
        QTest::keyClick(window, Qt::Key_BracketLeft);
        QCOMPARE(radio->currentKey(), QString("nightride/nightride"));
        QCOMPARE(player.playlist()->count(), 1);
        // The palette by name.
        QMetaObject::invokeMethod(window, "runAction", Q_ARG(QVariant, QString("radio:nightride/rektory")));
        QCOMPARE(radio->currentKey(), QString("nightride/rektory"));
        // F in the picker marks a favourite.
        QTest::keyClick(window, Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY(sheet->property("opened").toBool());
        QCOMPARE(radio->group(), QString("rekt"));
        QTRY_VERIFY(grid->property("activeFocus").toBool());
        QTRY_COMPARE(grid->property("currentIndex").toInt(), 1);
        QTest::keyClick(window, Qt::Key_F);
        QVERIFY(radio->isFavourite("nightride/rektory"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!sheet->property("opened").toBool());

        // Find stations from the palette, searching the pretend directory.
        m_http->setSearchAnswer("[{\"stationuuid\":\"win-1\",\"name\":\"Window Radio\",\"url_resolved\":\"http://w.example/s\",\"codec\":\"MP3\",\"bitrate\":128}]");
        QMetaObject::invokeMethod(window, "runAction", Q_ARG(QVariant, QString("radio-find")));
        QObject *field = nullptr;
        QTRY_VERIFY((field = find("findStationsField")) != nullptr);
        QTRY_VERIFY(field->property("activeFocus").toBool());
        for (const char c : QByteArray("window"))
            QTest::keyClick(window, c);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(radio->results().size(), 1, 10000);
        QTest::keyClick(window, Qt::Key_Escape);

        // A track again: no LIVE, the position bar is back.
        player.stop();
        QVERIFY(QFileInfo::exists(dir.filePath("library.db")));
        qInstallMessageHandler(previousHandler);
        qmlWarnings = nullptr;
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }
};

int runRadioTests(int argc, char **argv) {
    RadioTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "radio_tests.moc"
