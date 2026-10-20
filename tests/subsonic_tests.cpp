// Phase 5: OpenSubsonic and Subsonic servers as library sources. A mock
// server (QTcpServer on 127.0.0.1, serving fixture JSON) stands in for
// Navidrome; no real server is ever contacted. Secrets go to a file under a
// temporary XDG_CONFIG_HOME, never to the desktop keyring.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include <sys/stat.h>

#include "artwork.h"
#include "cli.h"
#include "library.h"
#include "query.h"
#include "remote.h"
#include "subsonic.h"
#include "volumes.h"
#include "mocksubsonic.h"

namespace {

QVariant value(Library &library, const QString &sql) {
    QSqlQuery q(library.db());
    if (!q.exec(sql) || !q.next())
        return {};
    return q.value(0);
}

int cli(const QStringList &args, QString *outText, QString *errText = nullptr) {
    QString o, e;
    QTextStream out(&o), err(&e);
    const int code = Cli::run(args, out, err);
    out.flush();
    err.flush();
    if (outText)
        *outText = o;
    if (errText)
        *errText = e;
    return code;
}

} // namespace

class SubsonicTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_xdg;
    QByteArray m_oldCache, m_oldConfig, m_oldStore;

    qint64 track(Library &library, const QString &songId) {
        return value(library, QString("SELECT id FROM tracks WHERE rel_path = '%1'").arg(songId)).toLongLong();
    }

private slots:
    void initTestCase() {
        m_oldCache = qgetenv("XDG_CACHE_HOME");
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        m_oldStore = qgetenv("OMAMP_SECRET_STORE");
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_xdg.filePath("cache")));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_xdg.filePath("config")));
        qputenv("OMAMP_SECRET_STORE", "file");
        // The mock server is on 127.0.0.1; a proxy from the environment must not get in the way.
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }

    void cleanupTestCase() {
        for (const auto &[name, old] : {std::pair{"XDG_CACHE_HOME", m_oldCache}, std::pair{"XDG_CONFIG_HOME", m_oldConfig},
                                        std::pair{"OMAMP_SECRET_STORE", m_oldStore}}) {
            if (old.isEmpty())
                qunsetenv(name);
            else
                qputenv(name, old);
        }
        RemoteSources::clearSecretCache();
    }

    void tokenFollowsTheSpecification() {
        // The example from the Subsonic API documentation.
        QCOMPARE(SubsonicClient::token("sesame", "c19b2d"), QString("26719a1196d2a940705a59634eb18eab"));
    }

    void pingSignsEveryRequest() {
        MockSubsonic server;
        SubsonicClient client(server.url(), "listener", "sesame", SubsonicClient::Auth::Token);
        SubsonicServer info;
        QString error;
        QVERIFY2(client.ping(&info, &error), qPrintable(error));
        QCOMPARE(info.type, QString("navidrome"));
        QCOMPARE(info.serverVersion, QString("0.53.3"));
        QCOMPARE(info.apiVersion, QString("1.16.1"));
        QVERIFY(info.openSubsonic);
        QVERIFY(client.ping(nullptr, &error));
        // A fresh salt each time, and the password never on the wire.
        QCOMPARE(server.queries.size(), 2);
        QVERIFY(server.queries[0].queryItemValue("s") != server.queries[1].queryItemValue("s"));
        for (const QUrlQuery &q : std::as_const(server.queries)) {
            QVERIFY(!q.hasQueryItem("p"));
            QVERIFY(!q.toString().contains("sesame"));
            QCOMPARE(q.queryItemValue("c"), QString("omamp"));
        }

        SubsonicClient wrong(server.url(), "listener", "nope", SubsonicClient::Auth::Token);
        QVERIFY(!wrong.ping(nullptr, &error));
        QCOMPARE(error, QString("Wrong username or password"));
        const SubsonicClient::Reply r = wrong.call("ping");
        QCOMPARE(r.code, 40);

        SubsonicClient keyed(server.url(), QString(), "key-123", SubsonicClient::Auth::ApiKey);
        QVERIFY2(keyed.ping(nullptr, &error), qPrintable(error));
        QVERIFY(!server.queries.last().hasQueryItem("u"));
        SubsonicClient badKey(server.url(), QString(), "key-999", SubsonicClient::Auth::ApiKey);
        QVERIFY(!badKey.ping(nullptr, &error));
        QCOMPARE(error, QString("Invalid API key"));

        // A web server that isn't Subsonic, and nothing listening at all.
        server.refuse = true;
        QVERIFY(!client.ping(nullptr, &error));
        QVERIFY2(error.contains("503"), qPrintable(error));
        QVERIFY(!error.contains("t=")); // never the signed URL
        SubsonicClient nowhere(QUrl("http://127.0.0.1:1"), "listener", "sesame", SubsonicClient::Auth::Token);
        QVERIFY(!nowhere.ping(nullptr, &error));
        QVERIFY(!error.isEmpty());
    }

    void browsingCalls() {
        MockSubsonic server;
        SubsonicClient client(QUrl(server.url().toString() + "/"), "listener", "sesame", SubsonicClient::Auth::Token);
        QString error;
        QCOMPARE(client.musicFolders(&error).size(), 1);
        const QJsonArray artists = client.artists(&error);
        QCOMPARE(artists.size(), 2);
        QCOMPARE(artists[1].toObject()["name"].toString(), QString("Neon Coast"));
        // Paging.
        QCOMPARE(client.albumList2("alphabeticalByName", 2, 0, &error).size(), 2);
        const QJsonArray rest = client.albumList2("alphabeticalByName", 2, 2, &error);
        QCOMPARE(rest.size(), 1);
        QCOMPARE(rest[0].toObject()["name"].toString(), QString("Quiet Hours"));
        QCOMPARE(server.queries.last().queryItemValue("offset"), QString("2"));
        const QJsonObject album = client.album("al-1", &error);
        QCOMPARE(album["song"].toArray().size(), 2);
        QVERIFY(client.album("al-9", &error).isEmpty());
        QCOMPARE(error, QString("Album not found"));
        error.clear();
        const QJsonObject found = client.search3("tunnel", 0, 0, 20, &error);
        QCOMPARE(found["song"].toArray().size(), 1);
        QCOMPARE(found["song"].toArray()[0].toObject()["id"].toString(), QString("s-2"));
        // The path stays /rest/<method>: the trailing slash of the address is dropped.
        QVERIFY(client.signedUrl("ping").path() == "/rest/ping");
    }

    void coverStreamScrobbleAndRating() {
        MockSubsonic server;
        SubsonicClient client(server.url(), "listener", "sesame", SubsonicClient::Auth::Token);
        QString error, type;
        const QByteArray cover = client.coverArt("al-1", 300, &type, &error);
        QVERIFY2(cover.startsWith("\x89PNG"), qPrintable(error));
        QCOMPARE(type, QString("image/png"));
        QCOMPARE(server.queries.last().queryItemValue("size"), QString("300"));

        const QUrl stream = client.streamUrl("s-1", 320, "opus");
        const QUrlQuery q(stream);
        QCOMPARE(stream.path(), QString("/rest/stream"));
        QCOMPARE(q.queryItemValue("id"), QString("s-1"));
        QCOMPARE(q.queryItemValue("maxBitRate"), QString("320"));
        QCOMPARE(q.queryItemValue("format"), QString("opus"));
        QCOMPARE(q.queryItemValue("t"), SubsonicClient::token("sesame", q.queryItemValue("s")));
        const QUrlQuery plain(client.streamUrl("s-1"));
        QVERIFY(!plain.hasQueryItem("maxBitRate"));
        QVERIFY(!plain.hasQueryItem("format"));

        QVERIFY(client.scrobble("s-1", true, 1760000000000, &error));
        QCOMPARE(server.methods.last(), QString("scrobble"));
        QCOMPARE(server.queries.last().queryItemValue("submission"), QString("true"));
        QCOMPARE(server.queries.last().queryItemValue("time"), QString("1760000000000"));
        QVERIFY(client.setRating("s-1", 9, &error));
        QCOMPARE(server.queries.last().queryItemValue("rating"), QString("5"));
    }

    void sourceMirrorsTheServerIntoTheLibrary() {
        MockSubsonic server;
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QString error;
        RemoteSource added;
        const qint64 id = RemoteSources::add(library, server.url(), "listener", "sesame", "token", "Home NAS", &error, &added);
        QVERIFY2(id, qPrintable(error));
        QCOMPARE(added.serverType, QString("navidrome"));
        QCOMPARE(added.secretStore, QString("file"));
        // The secret is in a private file, not in the database.
        const QString secrets = Secrets::filePath();
        QVERIFY(secrets.startsWith(m_xdg.path()));
        struct stat st {};
        QCOMPARE(stat(QFile::encodeName(secrets).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QFile secretFile(secrets);
        QVERIFY(secretFile.open(QIODevice::ReadOnly));
        QVERIFY(secretFile.readAll().contains("sesame"));
        QSqlQuery all(library.db());
        QVERIFY(all.exec("SELECT * FROM remote_sources"));
        while (all.next())
            for (int i = 0; i < all.record().count(); ++i)
                QVERIFY(!all.value(i).toString().contains("sesame"));
        // The same server and user twice is refused.
        QVERIFY(!RemoteSources::add(library, server.url(), "listener", "sesame", "token", "Again", &error));
        QVERIFY(error.contains("already"));
        QVERIFY(!RemoteSources::add(library, server.url(), "listener", "wrong", "token", "Bad", &error));
        QVERIFY(error.contains("Wrong username or password"));

        // First sync: everything.
        RemoteSyncResult r = RemoteSources::sync(library, id);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.albums, 3);
        QCOMPARE(r.albumsFetched, 3);
        QCOMPARE(r.tracksAdded, 4);
        QCOMPARE(value(library, "SELECT kind FROM volumes").toString(), QString("subsonic"));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 4);
        const qint64 highway = track(library, "s-1");
        QVERIFY(highway);
        QCOMPARE(value(library, QString("SELECT file_name || '.' || format FROM tracks WHERE id = %1").arg(highway)).toString(),
                 QString("01 Highway.flac"));
        QCOMPARE(value(library, QString("SELECT duration_ms FROM tracks WHERE id = %1").arg(highway)).toInt(), 200000);
        // Search and browse work on it like on files.
        QCOMPARE(Query::run(library, "highway").tracks.size(), 2);
        QCOMPARE(Query::run(library, "artist:\"neon coast\"").tracks.size(), 3);
        QCOMPARE(Query::run(library, "genre:ambient").tracks.size(), 1);
        const qint64 quiet = track(library, "s-3");
        QCOMPARE(value(library, QString("SELECT count(*) FROM track_artists WHERE track_id = %1 AND role = 'artist'").arg(quiet)).toInt(), 2);
        QCOMPARE(value(library, QString("SELECT rg_track_gain FROM tracks WHERE id = %1").arg(quiet)).toDouble(), -6.5);
        // The server's rating and play count fill empty library values.
        const qint64 live = track(library, "s-4");
        QCOMPARE(value(library, QString("SELECT rating FROM tracks WHERE id = %1").arg(live)).toInt(), 80);
        QCOMPARE(value(library, QString("SELECT play_count FROM tracks WHERE id = %1").arg(live)).toInt(), 3);
        QCOMPARE(value(library, "SELECT count(*) FROM albums").toInt(), 3);

        // Playing: a stream URL, never a path.
        QVERIFY(library.absolutePath(highway).isEmpty());
        QUrl url = library.playableUrl(highway, &error);
        QVERIFY2(url.isValid(), qPrintable(error));
        QCOMPARE(url.host(), QString("127.0.0.1"));
        QCOMPARE(url.path(), QString("/rest/stream"));
        QCOMPARE(QUrlQuery(url).queryItemValue("id"), QString("s-1"));
        QVERIFY(RemoteSources::setStreaming(library, id, 192, "mp3", &error));
        url = library.playableUrl(highway);
        QCOMPARE(QUrlQuery(url).queryItemValue("maxBitRate"), QString("192"));
        QCOMPARE(QUrlQuery(url).queryItemValue("format"), QString("mp3"));
        // Refreshing local volumes leaves the remote one alone.
        library.refreshVolumes(VolumeResolver());
        QCOMPARE(value(library, "SELECT online FROM volumes WHERE kind = 'subsonic'").toInt(), 1);

        // Second sync: nothing changed, so only the ping and one page of albums.
        server.methods.clear();
        r = RemoteSources::sync(library, id);
        QVERIFY(r.ok);
        QCOMPARE(r.albumsFetched, 0);
        QCOMPARE(r.albumsUnchanged, 3);
        QCOMPARE(server.methods, QStringList({"ping", "getAlbumList2"}));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 4);

        // A history entry, to see it survive the song going away and coming back.
        QSqlQuery q(library.db());
        QVERIFY(q.exec(QString("INSERT INTO play_history (track_id, played_at) VALUES (%1, 1)").arg(quiet)));
        QVERIFY(q.exec(QString("UPDATE tracks SET rating = 100 WHERE id = %1").arg(quiet)));

        // One album renamed, one removed.
        QJsonObject renamed = server.albums.last(); // Quiet Hours sorts last
        QJsonArray quietSongs = server.songs.value("al-2");
        server.removeAlbum("al-2");
        QJsonObject liveAlbum;
        for (const QJsonObject &a : std::as_const(server.albums))
            if (a["id"].toString() == "al-3")
                liveAlbum = a;
        liveAlbum["name"] = "Live at the Pier";
        QJsonArray liveSongs = server.songs.value("al-3");
        QJsonObject liveSong = liveSongs[0].toObject();
        liveSong["album"] = "Live at the Pier";
        liveSongs[0] = liveSong;
        server.addAlbum(liveAlbum, liveSongs);
        server.methods.clear();
        r = RemoteSources::sync(library, id);
        QVERIFY(r.ok);
        QCOMPARE(r.albumsFetched, 1);
        QCOMPARE(r.albumsGone, 1);
        QCOMPARE(r.tracksMissing, 1);
        QCOMPARE(server.count("getAlbum"), 1);
        QCOMPARE(value(library, QString("SELECT album FROM tracks WHERE id = %1").arg(live)).toString(),
                 QString("Live at the Pier"));
        QCOMPARE(value(library, QString("SELECT availability FROM tracks WHERE id = %1").arg(quiet)).toString(),
                 QString("missing"));
        QVERIFY(library.playableUrl(quiet).isEmpty());

        // It comes back: same row, rating and history kept.
        server.addAlbum(renamed, quietSongs);
        r = RemoteSources::sync(library, id);
        QCOMPARE(r.tracksBack, 1);
        QCOMPARE(track(library, "s-3"), quiet);
        QCOMPARE(value(library, QString("SELECT availability FROM tracks WHERE id = %1").arg(quiet)).toString(),
                 QString("online"));
        QCOMPARE(value(library, QString("SELECT rating FROM tracks WHERE id = %1").arg(quiet)).toInt(), 100);
        QCOMPARE(value(library, QString("SELECT count(*) FROM play_history WHERE track_id = %1").arg(quiet)).toInt(), 1);

        // The server goes away: offline, never missing.
        server.refuse = true;
        r = RemoteSources::sync(library, id);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("offline"));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'offline'").toInt(), 4);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);
        QVERIFY(!RemoteSources::find(library, "Home NAS", &error).lastError.isEmpty());
        server.refuse = false;
        r = RemoteSources::sync(library, id);
        QVERIFY(r.ok);
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 4);
        QVERIFY(RemoteSources::find(library, "Home NAS", &error).lastError.isEmpty());

        // A full sync fetches every album again.
        r = RemoteSources::sync(library, id, true);
        QCOMPARE(r.albumsFetched, 3);
        QCOMPARE(r.tracksAdded, 0);

        // Covers are fetched once, then read from the cache.
        server.methods.clear();
        const QString cover = RemoteSources::coverArt(library, highway, 256, &error);
        QVERIFY2(!cover.isEmpty(), qPrintable(error));
        QVERIFY(cover.startsWith(m_xdg.path()));
        QVERIFY(cover.endsWith(".png"));
        QCOMPARE(RemoteSources::coverArt(library, highway, 256, &error), cover);
        QCOMPARE(server.count("getCoverArt"), 1);

        // The album's cover (album grid, info panel) comes from the server
        // too: a server album has no folder to look in.
        server.realPictures = true;
        ArtworkCache art(library, m_xdg.path() + "/art");
        const qint64 album = value(library, QString("SELECT album_id FROM tracks WHERE id = %1").arg(highway)).toLongLong();
        QVERIFY(album > 0);
        const QString albumCover = art.artworkFor(ArtworkCache::Kind::Album, album, 256);
        QVERIFY2(!albumCover.isEmpty(), qPrintable(art.lastError()));
        QVERIFY(QFileInfo::exists(albumCover));
        QCOMPARE(value(library, QString("SELECT artwork_id IS NOT NULL FROM albums WHERE id = %1").arg(album)).toInt(), 1);

        // Plays and ratings go back to the server.
        QVERIFY(RemoteSources::scrobble(library, highway, true, 1760000000000, &error));
        QCOMPARE(server.queries.last().queryItemValue("id"), QString("s-1"));
        QVERIFY(RemoteSources::setRating(library, highway, 60, &error));
        QCOMPARE(server.queries.last().queryItemValue("rating"), QString("3"));
        QVERIFY(!RemoteSources::setRating(library, 99999, 60, &error));

        // Removing the source removes its tracks and its secret.
        QVERIFY(RemoteSources::remove(library, id, false, &error));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks").toInt(), 0);
        QCOMPARE(value(library, "SELECT count(*) FROM volumes").toInt(), 0);
        QCOMPARE(value(library, "SELECT count(*) FROM albums").toInt(), 0);
        QVERIFY(secretFile.seek(0));
        secretFile.close();
        QVERIFY(secretFile.open(QIODevice::ReadOnly));
        QVERIFY(!secretFile.readAll().contains("sesame"));
    }

    void apiKeySourceAndKeepingTracks() {
        MockSubsonic server;
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QString error;
        const qint64 id = RemoteSources::add(library, server.url(), QString(), "key-123", "apikey", QString(), &error);
        QVERIFY2(id, qPrintable(error));
        QCOMPARE(RemoteSources::find(library, QString::number(id), &error).name, QString("127.0.0.1"));
        QVERIFY(RemoteSources::sync(library, id).ok);
        const QUrlQuery q(library.playableUrl(track(library, "s-2")));
        QCOMPARE(q.queryItemValue("apiKey"), QString("key-123"));
        QVERIFY(!q.hasQueryItem("u"));
        QVERIFY(RemoteSources::remove(library, id, true, &error));
        QCOMPARE(value(library, "SELECT count(*) FROM tracks WHERE availability = 'offline'").toInt(), 4);
        QVERIFY(library.playableUrl(track(library, "s-2"), &error).isEmpty());
    }

    void commandLine() {
        MockSubsonic server;
        QTemporaryDir dir;
        const QString db = dir.filePath("library.db");
        QString out, err;
        QCOMPARE(cli({"remote", "add", "subsonic", server.url().toString(), "--user", "listener", "--password", "x", "--db", db},
                     &out, &err),
                 1);
        QVERIFY(err.contains("never taken from the command line"));
        QCOMPARE(cli({"remote", "add", "subsonic", server.url().toString(), "--user", "listener", "--db", db}, &out, &err), 1);
        Cli::setStdinForTesting("sesame\n");
        QCOMPARE(cli({"remote", "add", "subsonic", server.url().toString(), "--user", "listener", "--password-stdin",
                      "--name", "nas", "--db", db, "--json"},
                     &out, &err),
                 0);
        QJsonObject j = QJsonDocument::fromJson(out.toUtf8()).object();
        QVERIFY(j["ok"].toBool());
        QCOMPARE(j["server_type"].toString(), QString("navidrome"));
        QVERIFY(!out.contains("sesame"));

        QCOMPARE(cli({"remote", "list", "--db", db, "--json"}, &out, &err), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object()["sources"].toArray().size(), 1);
        QCOMPARE(cli({"remote", "sync", "nas", "--db", db, "--json"}, &out, &err), 0);
        j = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(j["results"].toArray()[0].toObject()["tracks_added"].toInt(), 4);
        QCOMPARE(cli({"remote", "sync", "--db", db}, &out, &err), 0);
        QVERIFY2(out.contains("3 unchanged"), qPrintable(out));
        QCOMPARE(cli({"remote", "set", "nas", "--max-bitrate", "128", "--format", "opus", "--db", db}, &out, &err), 0);
        QVERIFY(out.contains("128 kbit/s"));
        QCOMPARE(cli({"volume", "list", "--db", db, "--json"}, &out, &err), 0);
        const QJsonObject volume = QJsonDocument::fromJson(out.toUtf8()).object()["volumes"].toArray()[0].toObject();
        QCOMPARE(volume["kind"].toString(), QString("subsonic"));
        QCOMPARE(volume["tracks"].toInt(), 4);
        QCOMPARE(cli({"query", "tunnel", "--db", db, "--json"}, &out, &err), 0);
        QVERIFY(out.contains("Tunnel"));
        QCOMPARE(cli({"remote", "remove", "nas", "--db", db}, &out, &err), 0);
        QCOMPARE(cli({"remote", "list", "--db", db}, &out, &err), 0);
        QVERIFY(out.contains("No remote sources"));
        QCOMPARE(cli({"remote", "sync", "9", "--db", db}, &out, &err), 1);
    }
};

int runSubsonicTests(int argc, char **argv) {
    SubsonicTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "subsonic_tests.moc"
