// Phase 4: the web. Every test talks to MockHttp on 127.0.0.1, serving JSON
// written by hand from the services' documented shapes (tests/fixtures/web),
// so nothing here reaches the internet. liveSmoke() alone talks to the real
// MusicBrainz and LRCLIB, and only when OMAMP_LIVE_TESTS=1.
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include "acoustid.h"
#include "artwork.h"
#include "cli.h"
#include "coverart.h"
#include "fixture_library.h"
#include "http.h"
#include "identify.h"
#include "library.h"
#include "lyrics.h"
#include "mockhttp.h"
#include "musicbrainz.h"
#include "scanner.h"
#include "scrobble.h"
#include "tagwriter.h"
#include "textmatch.h"
#include "volumes.h"

#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/synchronizedlyricsframe.h>

namespace {

const QString releaseA = "a1b2c3d4-0001-4000-8000-000000000001";
const QString releaseB = "a1b2c3d4-0002-4000-8000-000000000002";
const QString groupId = "b0000000-0000-4000-8000-0000000000aa";

QByteArray readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray png(const QColor &colour, int size = 32) {
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

bool run(const QString &program, const QStringList &args) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        qWarning() << program << args << p.readAllStandardError();
        return false;
    }
    return true;
}

// A quiet tone of a given length with tags; nothing is played.
bool tone(const QString &file, double seconds, const QStringList &extra) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds),
                        "-af", "volume=0.05", "-map_metadata", "-1"};
    return run("ffmpeg", args + extra + QStringList{file});
}

QMap<QString, QByteArray> tree(const QString &root) {
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        out.insert(path.mid(root.size() + 1), QCryptographicHash::hash(readAll(path), QCryptographicHash::Sha1));
    }
    return out;
}

} // namespace

class WebTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<MockHttp> m_server;
    QString m_pristine; // the audio files, made once
    int m_counter = 0;
    QMap<QByteArray, QByteArray> m_savedEnv;

    QString fixturePath(const QString &name) const { return QString(OMAMP_TESTS_DIR) + "/fixtures/web/" + name; }
    QByteArray fixture(const QString &name) const {
        QByteArray b = readAll(fixturePath(name));
        b.replace("{{BASE}}", (m_server->url() + "/caa").toUtf8());
        return b;
    }

    void setEnv(const char *name, const QByteArray &value) {
        if (!m_savedEnv.contains(name))
            m_savedEnv.insert(name, qgetenv(name));
        qputenv(name, value);
    }

    // The services as documented, played by the mock server.
    MockHttp::Reply route(const MockHttp::Request &r) {
        const QString path = QString::fromUtf8(r.path);
        auto json = [](const QByteArray &body, int status = 200) { return MockHttp::Reply{status, body, "application/json", {}}; };
        if (path == "/ws/2/release/" + releaseA)
            return json(fixture("mb-release-a.json"));
        if (path == "/ws/2/release/" + releaseB)
            return json(fixture("mb-release-b.json"));
        if (path.startsWith("/ws/2/release/"))
            return json("{\"error\":\"Not Found\",\"help\":\"For usage, please see: https://musicbrainz.org/development/mmd\"}", 404);
        if (path == "/ws/2/release")
            return json(fixture("mb-release-search.json"));
        if (path == "/ws/2/recording")
            return json(fixture("mb-recording-search.json"));
        if (path == "/v2/lookup") {
            if (r.formItem("client") != "testkey")
                return json("{\"status\":\"error\",\"error\":{\"code\":4,\"message\":\"invalid API key\"}}", 400);
            if (r.formItem("fingerprint").contains("midnight"))
                return json(fixture("acoustid-lookup.json"));
            return json("{\"status\":\"ok\",\"results\":[]}");
        }
        if (path == "/caa/release/" + releaseA || path == "/caa/release-group/" + groupId)
            return json(fixture("caa-release.json"));
        if (path.startsWith("/caa/release"))
            return json("", 404);
        if (path.startsWith("/caa/img/")) {
            const QColor colour = path.contains("front") ? QColor(200, 30, 30) : QColor(30, 30, 200);
            return MockHttp::Reply{200, png(colour, path.contains("1200") ? 64 : 32), "image/png", {}};
        }
        if (path == "/api/get") {
            if (r.item("track_name") == "Coastline")
                return json(fixture("lrclib-get.json"));
            return json("{\"code\":404,\"name\":\"TrackNotFound\",\"message\":\"Failed to find specified track\"}", 404);
        }
        if (path == "/api/search")
            return json(r.item("track_name") == "Midnight Highway" ? fixture("lrclib-search.json") : QByteArray("[]"));
        if (path == "/1/validate-token") {
            if (r.headers.value("authorization") == "Token good-token")
                return json(fixture("listenbrainz-validate.json"));
            return json("{\"code\":200,\"message\":\"Token invalid.\",\"valid\":false}");
        }
        if (path == "/1/submit-listens") {
            if (r.headers.value("authorization") != "Token good-token")
                return json("{\"code\":401,\"error\":\"You need to provide an Authorization header.\"}", 401);
            if (r.body.contains("\"artist_name\":\"Bad\""))
                return json("{\"code\":400,\"error\":\"JSON document may only contain listen_type, payload\"}", 400);
            return json("{\"status\":\"ok\"}");
        }
        if (path == "/2.0/") {
            const QString method = r.method == "POST" ? r.formItem("method") : r.item("method");
            if (method == "auth.getToken")
                return json("{\"token\":\"tok123\"}");
            if (method == "auth.getSession")
                return r.item("token") == "tok123"
                           ? json("{\"session\":{\"name\":\"fmuser\",\"key\":\"sess\",\"subscriber\":0}}")
                           : json("{\"error\":14,\"message\":\"Unauthorized Token - This token has not been authorized\"}", 403);
            if (r.formItem("sk") != "sess")
                return json("{\"error\":9,\"message\":\"Invalid session key - Please re-authenticate\"}", 403);
            if (method == "track.scrobble")
                return json(fixture("lastfm-scrobble.json"));
            if (method == "track.updateNowPlaying")
                return json("{\"nowplaying\":{\"ignoredMessage\":{\"code\":\"0\",\"#text\":\"\"}}}");
        }
        return json("{\"error\":\"unexpected request\"}", 404);
    }

    struct Setup {
        QString root, music, db, cache, art;
        std::unique_ptr<Library> library;
        qint64 id(const QString &fileName) {
            QSqlQuery q(library->db());
            q.prepare("SELECT id FROM tracks WHERE rel_path LIKE ?");
            q.addBindValue("%/" + fileName);
            return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
        }
    };

    // A fresh library over a copy of the audio files.
    std::unique_ptr<Setup> library(bool withFiles = true) {
        auto s = std::make_unique<Setup>();
        s->root = m_tmp.path() + QString("/case%1").arg(++m_counter);
        s->music = s->root + "/music";
        s->db = s->root + "/library.db";
        s->cache = s->root + "/http";
        s->art = s->root + "/art";
        QDir().mkpath(s->music);
        if (withFiles) {
            QDirIterator it(m_pristine, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString from = it.next();
                const QString to = s->music + from.mid(m_pristine.size());
                QDir().mkpath(QFileInfo(to).absolutePath());
                QFile::copy(from, to);
            }
        }
        s->library = std::make_unique<Library>(s->db);
        s->library->open();
        if (withFiles)
            Scanner(*s->library, VolumeResolver()).scan({s->music});
        return s;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_server = std::make_unique<MockHttp>();
        m_server->setHandler([this](const MockHttp::Request &r) { return route(r); });
        // Nothing may touch the real home: caches, data and settings go to
        // the temporary folder for this class.
        setEnv("XDG_CACHE_HOME", (m_tmp.path() + "/xdg-cache").toUtf8());
        setEnv("XDG_DATA_HOME", (m_tmp.path() + "/xdg-data").toUtf8());
        setEnv("XDG_CONFIG_HOME", (m_tmp.path() + "/xdg-config").toUtf8());
        const QByteArray base = m_server->url().toUtf8();
        setEnv("OMAMP_MUSICBRAINZ_URL", base + "/ws/2");
        setEnv("OMAMP_ACOUSTID_URL", base + "/v2");
        setEnv("OMAMP_COVERART_URL", base + "/caa");
        setEnv("OMAMP_LRCLIB_URL", base + "/api");
        setEnv("OMAMP_LISTENBRAINZ_URL", base + "/1");
        setEnv("OMAMP_LASTFM_URL", base + "/2.0");
        setEnv("OMAMP_ACOUSTID_KEY", "");
        setEnv("OMAMP_LISTENBRAINZ_TOKEN", "");
        setEnv("OMAMP_LASTFM_API_KEY", "");
        setEnv("OMAMP_LASTFM_API_SECRET", "");
        setEnv("OMAMP_OFFLINE", "");

        m_pristine = m_tmp.path() + "/pristine";
        const QString album = m_pristine + "/Neon Coast/Night Drive";
        QVERIFY(tone(album + "/01 midnight highway.flac", 5,
                     {"-metadata", "title=midnight highway", "-metadata", "artist=Neon Coast", "-metadata",
                      "album=Night Drive", "-metadata", "track=1", "-metadata", "date=1997"}));
        QVERIFY(tone(album + "/02 Coastline.mp3", 7,
                     {"-c:a", "libmp3lame", "-b:a", "64k", "-id3v2_version", "4", "-metadata", "title=Coastline",
                      "-metadata", "artist=Neon Coast", "-metadata", "album=Night Drive", "-metadata", "track=2"}));
        QVERIFY(tone(album + "/03 Afterglow.m4a", 9,
                     {"-c:a", "aac", "-b:a", "64k", "-metadata", "title=Afterglow", "-metadata",
                      "artist=Neon Coast; Lia", "-metadata", "album=Night Drive", "-metadata", "track=3"}));
        QVERIFY(tone(m_pristine + "/Mixes/Summer Mix/01 Heatwave.flac", 3,
                     {"-metadata", "title=Heatwave", "-metadata", "artist=Sun Machine", "-metadata",
                      "album=Summer Mix", "-metadata", "track=1"}));
    }

    void cleanupTestCase() {
        Net::Config::resetForTests();
        for (auto it = m_savedEnv.constBegin(); it != m_savedEnv.constEnd(); ++it) {
            if (it.value().isEmpty())
                qunsetenv(it.key().constData());
            else
                qputenv(it.key().constData(), it.value());
        }
    }

    void init() {
        Net::Config::resetForTests();
        // Fast tests: no pacing (one test measures it on purpose), short backoff.
        for (Net::Service s : {Net::Service::MusicBrainz, Net::Service::AcoustId, Net::Service::CoverArt,
                               Net::Service::Lrclib, Net::Service::ListenBrainz, Net::Service::LastFm})
            Net::Config::setIntervalMs(s, 0);
        Net::Config::setBackoffMs(10);
        m_server->setHandler([this](const MockHttp::Request &r) { return route(r); });
        m_server->clear();
    }

    // --- the HTTP layer -----------------------------------------------------

    void userAgentAndEndpoints() {
        QCOMPARE(Net::userAgent(), QString("Omamp/%1 ( https://github.com/matt-shearing/omamp )").arg(Net::version()));
        QVERIFY(!Net::version().isEmpty());
        QCOMPARE(Net::defaultBaseUrl(Net::Service::MusicBrainz), QString("https://musicbrainz.org/ws/2"));
        QCOMPARE(Net::baseUrl(Net::Service::MusicBrainz), m_server->url() + "/ws/2");

        Net::HttpClient http(nullptr, m_tmp.path() + "/ua-cache");
        const Net::Response r = http.get(Net::Service::MusicBrainz, QUrl(m_server->url() + "/ws/2/release/" + releaseA));
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(m_server->requests().last().headers.value("user-agent"), Net::userAgent().toUtf8());

        // A library setting moves an endpoint when the environment doesn't.
        auto s = library(false);
        s->library->setSetting("net.url.lrclib", "http://example.invalid/api/");
        qunsetenv("OMAMP_LRCLIB_URL");
        QCOMPARE(Net::baseUrl(Net::Service::Lrclib, s->library.get()), QString("http://example.invalid/api"));
        qputenv("OMAMP_LRCLIB_URL", (m_server->url() + "/api").toUtf8());
        QCOMPARE(Net::baseUrl(Net::Service::Lrclib, s->library.get()), m_server->url() + "/api");

        // Values are percent-encoded once.
        const QUrl u = Net::makeUrl("http://h", "/p", {{"q", "a b&c"}, {"x", "ä"}});
        QCOMPARE(u.toString(QUrl::FullyEncoded), QString("http://h/p?q=a%20b%26c&x=%C3%A4"));
    }

    void rateLimitsAcrossThreads() {
        Net::Config::setIntervalMs(Net::Service::MusicBrainz, 150);
        const QString url = m_server->url() + "/ws/2/release/" + releaseA;
        QElapsedTimer timer;
        timer.start();
        // Two worker threads, each with its own client, share one pace.
        auto work = [url] {
            Net::HttpClient http;
            http.setCacheEnabled(false);
            for (int i = 0; i < 2; ++i)
                http.get(Net::Service::MusicBrainz, QUrl(url));
        };
        std::unique_ptr<QThread> a(QThread::create(work)), b(QThread::create(work));
        a->start();
        b->start();
        QVERIFY(a->wait(20000));
        QVERIFY(b->wait(20000));
        QCOMPARE(m_server->count(), 4);
        QVERIFY2(timer.elapsed() >= 3 * 150 - 20, qPrintable(QString::number(timer.elapsed())));

        // Another service has its own pace.
        Net::Config::setIntervalMs(Net::Service::Lrclib, 0);
        timer.restart();
        Net::HttpClient http;
        http.get(Net::Service::Lrclib, QUrl(m_server->url() + "/api/search?track_name=x"));
        QVERIFY(timer.elapsed() < 150);
    }

    void listenBrainzRateHeaders() {
        int calls = 0;
        m_server->setHandler([&calls](const MockHttp::Request &) {
            ++calls;
            return MockHttp::Reply{200, "{\"status\":\"ok\"}", "application/json",
                                   {{"X-RateLimit-Limit", "30"}, {"X-RateLimit-Remaining", "0"},
                                    {"X-RateLimit-Reset-In", "0.4"}}};
        });
        Net::HttpClient http;
        QElapsedTimer timer;
        timer.start();
        http.get(Net::Service::ListenBrainz, QUrl(m_server->url() + "/1/x"));
        const qint64 first = timer.elapsed();
        http.get(Net::Service::ListenBrainz, QUrl(m_server->url() + "/1/x"));
        QVERIFY2(timer.elapsed() - first >= 350, qPrintable(QString::number(timer.elapsed() - first)));
        QCOMPARE(calls, 2);
    }

    void retriesWithBackoff() {
        int calls = 0;
        m_server->setHandler([&calls](const MockHttp::Request &) {
            ++calls;
            if (calls == 1)
                return MockHttp::Reply{503, "{\"error\":\"busy\"}", "application/json", {}};
            if (calls == 2)
                return MockHttp::Reply{429, "{}", "application/json", {{"Retry-After", "0"}}};
            return MockHttp::Reply{200, "{\"ok\":1}", "application/json", {}};
        });
        Net::HttpClient http;
        Net::Response r = http.get(Net::Service::MusicBrainz, QUrl(m_server->url() + "/ws/2/x"));
        QVERIFY(r.ok());
        QCOMPARE(r.attempts, 3);
        QCOMPARE(calls, 3);

        // A client error is final.
        calls = 0;
        m_server->setHandler([&calls](const MockHttp::Request &) {
            ++calls;
            return MockHttp::Reply{400, "{\"error\":\"bad\"}", "application/json", {}};
        });
        r = http.get(Net::Service::MusicBrainz, QUrl(m_server->url() + "/ws/2/x"));
        QCOMPARE(r.status, 400);
        QCOMPARE(calls, 1);

        // Busy for good: gives up after the configured retries.
        calls = 0;
        Net::Config::setMaxRetries(2);
        m_server->setHandler([&calls](const MockHttp::Request &) {
            ++calls;
            return MockHttp::Reply{503, "{}", "application/json", {}};
        });
        r = http.get(Net::Service::MusicBrainz, QUrl(m_server->url() + "/ws/2/x"));
        QCOMPARE(r.status, 503);
        QCOMPARE(calls, 3);
        QVERIFY(!r.ok());
    }

    void cacheAndOffline() {
        auto s = library(false);
        Net::HttpClient http(s->library.get(), s->cache);
        const QUrl url(m_server->url() + "/ws/2/release/" + releaseA);
        Net::Response r = http.get(Net::Service::MusicBrainz, url, 3600);
        QVERIFY(r.ok() && !r.fromCache);
        r = http.get(Net::Service::MusicBrainz, url, 3600);
        QVERIFY(r.ok() && r.fromCache);
        QCOMPARE(m_server->count(), 1);
        QCOMPARE(r.json().object().value("title").toString(), QString("Night Drive"));

        // Asking afresh goes out again.
        Net::Request fresh;
        fresh.service = Net::Service::MusicBrainz;
        fresh.url = url;
        fresh.cacheSeconds = 3600;
        fresh.refresh = true;
        QVERIFY(!http.send(fresh).fromCache);
        QCOMPARE(m_server->count(), 2);

        // Uncached requests are never cached; a 404 is (briefly).
        http.get(Net::Service::MusicBrainz, url);
        http.get(Net::Service::MusicBrainz, url);
        QCOMPARE(m_server->count(), 4);
        const QUrl missing(m_server->url() + "/ws/2/release/00000000-0000-0000-0000-000000000000");
        QCOMPARE(http.get(Net::Service::MusicBrainz, missing, 3600).status, 404);
        QCOMPARE(http.get(Net::Service::MusicBrainz, missing, 3600).status, 404);
        QCOMPARE(m_server->count(), 5);

        // Turned off: the cache answers, even past its time; nothing else.
        s->library->setSetting("net.offline", "1");
        m_server->clear();
        fresh.refresh = false;
        r = http.send(fresh);
        QVERIFY(r.ok() && r.fromCache);
        r = http.get(Net::Service::MusicBrainz, QUrl(m_server->url() + "/ws/2/release/" + releaseB), 3600);
        QVERIFY(r.offline);
        QVERIFY(r.error.startsWith("Offline"));
        QCOMPARE(m_server->count(), 0);
        s->library->setSetting("net.offline", "0");
        QVERIFY(http.cache().clear() >= 2);

        // A host that refuses connections is offline, and not retried forever.
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost, 0);
        const quint16 port = closed.serverPort();
        closed.close();
        QElapsedTimer timer;
        timer.start();
        r = http.get(Net::Service::MusicBrainz, QUrl(QString("http://127.0.0.1:%1/x").arg(port)));
        QVERIFY(r.offline);
        QCOMPARE(r.attempts, 1);
        QVERIFY(timer.elapsed() < 5000);
    }

    // --- MusicBrainz, AcoustID, matching ------------------------------------

    void musicBrainzParsing() {
        Net::HttpClient http(nullptr, m_tmp.path() + "/mb-cache");
        http.setCacheEnabled(false);
        MusicBrainz::Client mb(http);
        QString error;
        const MusicBrainz::Release r = mb.release(releaseA, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(r.id, releaseA);
        QCOMPARE(r.title, QString("Night Drive"));
        QCOMPARE(MusicBrainz::creditText(r.credits), QString("Neon Coast"));
        QCOMPARE(r.date, QString("1997-05-12"));
        QCOMPARE(r.year(), 1997);
        QCOMPARE(r.country, QString("GB"));
        QCOMPARE(r.releaseGroupId, groupId);
        QCOMPARE(r.firstReleaseDate, QString("1996-11-01"));
        QCOMPARE(r.primaryType, QString("Album"));
        QCOMPARE(r.labels.size(), 1);
        QCOMPARE(r.labels.first().name, QString("Coastal Records"));
        QCOMPARE(r.labels.first().catalogNumber, QString("NC 001"));
        QCOMPARE(r.genres, (QStringList{"synthwave", "electronic"})); // by votes
        QCOMPARE(r.media.size(), 1);
        QCOMPARE(r.trackCount(), 3);
        QCOMPARE(r.formats(), QString("CD"));
        QVERIFY(r.complete);
        const MusicBrainz::Track &t3 = r.media[0].tracks[2];
        QCOMPARE(t3.title, QString("Afterglow"));
        QCOMPARE(t3.lengthMs, qint64(9000));
        QCOMPARE(t3.recordingId, QString("90000000-0000-4000-8000-000000000003"));
        QCOMPARE(MusicBrainz::creditText(t3.credits), QString("Neon Coast feat. Lia"));
        QCOMPARE(MusicBrainz::creditNames(t3.credits), (QStringList{"Neon Coast", "Lia"}));
        QCOMPARE(r.media[0].tracks[0].isrcs, QStringList{"GBNCA9700001"});

        // The lookup asks for everything identify writes.
        const MockHttp::Request lookup = m_server->requests().last();
        QCOMPARE(lookup.item("fmt"), QString("json"));
        for (const QString &inc : {"recordings", "artist-credits", "labels", "release-groups", "isrcs", "genres"})
            QVERIFY2(lookup.item("inc").contains(inc), qPrintable(lookup.item("inc")));

        const QList<MusicBrainz::Release> found = mb.searchReleases("Neon Coast", "Night Drive", 3, 10, &error);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].searchScore, 100);
        QCOMPARE(found[1].id, releaseA);
        QCOMPARE(found[1].trackCount(), 3);
        QVERIFY(!found[1].complete);
        const QString query = m_server->requests().last().item("query");
        QCOMPARE(query, QString("release:\"Night Drive\" AND artist:\"Neon Coast\" AND tracks:3"));
        QCOMPARE(MusicBrainz::quoted("AC/DC: \"Live\""), QString("\"AC\\/DC\\: \\\"Live\\\"\""));

        const QList<MusicBrainz::Recording> recs = mb.searchRecordings("Neon Coast", "Coastline", 5, &error);
        QCOMPARE(recs.size(), 1);
        QCOMPARE(recs[0].releases.size(), 1);
        QCOMPARE(recs[0].releases[0].id, releaseA);
        QCOMPARE(recs[0].isrcs, QStringList{"GBNCA9700002"});

        QVERIFY(mb.release("00000000-0000-0000-0000-000000000000", &error).id.isEmpty());
        QVERIFY(error.contains("nothing"));
    }

    void textMatching() {
        QCOMPARE(TextMatch::normalise("The Beatles!"), QString("beatles"));
        QCOMPARE(TextMatch::normalise("Björk & Friends"), QString("bjork and friends"));
        QCOMPARE(TextMatch::similarity("midnight highway", "Midnight Highway"), 1.0);
        QVERIFY(TextMatch::similarity("Coastline", "Coastline (Remastered)") > 0.85);
        QVERIFY(TextMatch::similarity("Coastline", "Afterglow") < 0.3);
        QCOMPARE(TextMatch::distance("kitten", "sitting"), 3);
        QCOMPARE(TextMatch::durationSimilarity(5000, 6500), 1.0);
        QCOMPARE(TextMatch::durationSimilarity(5000, 40000), 0.0);
        QCOMPARE(TextMatch::durationSimilarity(0, 40000), 0.5);
    }

    void acoustIdLookup() {
        Net::HttpClient http;
        http.setCacheEnabled(false);
        AcoustId::Result r = AcoustId::lookup(http, "", "FP", 5);
        QVERIFY(r.missingKey);
        QVERIFY(r.error.contains("no application key"));
        QCOMPARE(m_server->count(), 0); // nothing is sent without a key

        r = AcoustId::lookup(http, "wrongkey", "FP-midnight", 5);
        QVERIFY(!r.ok);
        QVERIFY(r.missingKey);
        QVERIFY(r.error.contains("invalid API key"));

        r = AcoustId::lookup(http, "testkey", "FP-midnight", 5);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.matches.size(), 2);
        QCOMPARE(r.matches[0].recordingId, QString("90000000-0000-4000-8000-000000000001"));
        QCOMPARE(r.matches[0].score, 0.97);
        QCOMPARE(r.matches[0].artist, QString("Neon Coast"));
        QCOMPARE(r.matches[0].releaseIds, QStringList{releaseA});
        QCOMPARE(r.matches[0].releaseGroupIds, QStringList{groupId});
        const MockHttp::Request sent = m_server->requests().last();
        QCOMPARE(sent.method, QByteArray("POST"));
        QCOMPARE(sent.formItem("client"), QString("testkey"));
        QCOMPARE(sent.formItem("duration"), QString("5"));
        QCOMPARE(sent.formItem("fingerprint"), QString("FP-midnight"));
        QCOMPARE(sent.formItem("meta"), QString("recordings releaseids releasegroupids"));

        // The key: environment over setting.
        auto s = library(false);
        QCOMPARE(AcoustId::applicationKey(s->library.get()), QString());
        s->library->setSetting("acoustid.key", "fromsetting");
        QCOMPARE(AcoustId::applicationKey(s->library.get()), QString("fromsetting"));
        qputenv("OMAMP_ACOUSTID_KEY", "fromenv");
        QCOMPARE(AcoustId::applicationKey(s->library.get()), QString("fromenv"));
        qputenv("OMAMP_ACOUSTID_KEY", "");
    }

    void clustering() {
        auto s = library();
        QSqlQuery q(s->library->db());
        QList<qint64> ids;
        q.exec("SELECT id FROM tracks ORDER BY rel_path DESC");
        while (q.next())
            ids << q.value(0).toLongLong();
        QCOMPARE(ids.size(), 4);
        const QList<Identify::Cluster> clusters = Identify::cluster(*s->library, ids);
        QCOMPARE(clusters.size(), 2);
        const Identify::Cluster &night = clusters[0].album == "Night Drive" ? clusters[0] : clusters[1];
        QCOMPARE(night.files.size(), 3);
        QCOMPARE(night.artist, QString("Neon Coast")); // most files agree
        QCOMPARE(night.year, 0);                       // only one file has a year
        QCOMPARE(night.files[0].title, QString("midnight highway")); // in track order
        QCOMPARE(night.files[2].durationMs / 1000, qint64(9));
        QVERIFY(night.folder.endsWith("/Night Drive"));
    }

    void identifyBySearch() {
        auto s = library();
        const QList<qint64> ids = {s->id("01 midnight highway.flac"), s->id("02 Coastline.mp3"),
                                   s->id("03 Afterglow.m4a")};
        Net::HttpClient http(s->library.get(), s->cache);
        Identify::Identifier identifier(*s->library, http);
        Identify::Options options;
        const Identify::Result result = identifier.identify(ids, options);
        QCOMPARE(result.clusters.size(), 1);
        QVERIFY(!result.acoustIdUsed);
        QVERIFY(result.notes.join(' ').contains("AcoustID was skipped"));
        const Identify::ClusterResult &c = result.clusters.first();
        QCOMPARE(c.candidates.size(), 2);
        const Identify::Candidate &best = c.candidates.first();
        // The search ranked the deluxe edition first; the tracks say otherwise.
        QCOMPARE(best.release.id, releaseA);
        QCOMPARE(best.confidence(), QString("high"));
        QVERIFY2(best.score > c.candidates[1].score + 0.1,
                 qPrintable(QString("%1 vs %2").arg(best.score).arg(c.candidates[1].score)));
        QCOMPARE(best.sources, QStringList{"search"});
        QCOMPARE(best.mapping.size(), 3);
        QVERIFY(best.unmatched.isEmpty());
        QCOMPARE(best.mapping[0].trackId, ids[0]);
        QCOMPARE(best.mapping[0].track.title, QString("Midnight Highway"));
        QCOMPARE(best.mapping[2].track.position, 3);
        QCOMPARE(best.mapping[0].via, QString("tags"));
        QCOMPARE(best.parts.value("track_count"), 1.0);
        QVERIFY(c.candidates[1].parts.value("track_count") < 1.0);
        // The search sent the album, artist and track count.
        bool searched = false;
        for (const MockHttp::Request &r : m_server->requests())
            searched = searched || (r.path == "/ws/2/release" && r.item("query").contains("tracks:3"));
        QVERIFY(searched);
        // The review dialog's data serialises.
        const QJsonObject j = result.toJson();
        QCOMPARE(j["clusters"].toArray().first().toObject()["candidates"].toArray().size(), 2);

        // Preferences move a candidate: prefer the US digital release.
        Identify::Options prefer;
        prefer.countries = {"US"};
        prefer.formats = {"Digital Media"};
        const Identify::Candidate a = Identify::Identifier::score(c.cluster, c.candidates[0].release, prefer);
        const Identify::Candidate b = Identify::Identifier::score(c.cluster, c.candidates[1].release, prefer);
        QCOMPARE(b.parts.value("country"), 1.0);
        QCOMPARE(a.parts.value("country"), 0.0);
        QVERIFY(a.score > b.score); // but not enough to beat the right track count
    }

    void identifyByMbidAndRelease() {
        auto s = library();
        const qint64 one = s->id("01 midnight highway.flac");
        // A release ID already in the tags is looked up first.
        QVERIFY(TagWriter(*s->library).apply({{one, "mbreleaseid", releaseA, {}, {}}}).ok);
        Net::HttpClient http(s->library.get(), s->cache);
        Identify::Identifier identifier(*s->library, http);
        Identify::Options options;
        options.useSearch = false;
        const QList<qint64> ids = {one, s->id("02 Coastline.mp3"), s->id("03 Afterglow.m4a")};
        Identify::Result r = identifier.identify(ids, options);
        // Only one of three files carries the ID: not a consensus.
        QVERIFY(r.clusters.first().candidates.isEmpty());
        QVERIFY(!r.clusters.first().error.isEmpty());
        // All three carry it now.
        QList<TagEdit> edits;
        for (qint64 id : ids)
            edits << TagEdit{id, "mbreleaseid", releaseA, {}, {}};
        QVERIFY(TagWriter(*s->library).apply(edits).ok);
        m_server->clear();
        r = identifier.identify(ids, options);
        QCOMPARE(r.clusters.first().candidates.size(), 1);
        QCOMPARE(r.clusters.first().best()->sources, QStringList{"mbid"});
        QCOMPARE(m_server->count(), 1); // one lookup, no search

        // The review's "use this release".
        options.forceRelease = releaseB;
        r = identifier.identify(ids, options);
        QCOMPARE(r.clusters.first().best()->release.id, releaseB);
        QCOMPARE(r.clusters.first().best()->sources, QStringList{"chosen"});
    }

    void identifyWithAcoustId() {
        auto s = library();
        // A stand-in fpcalc whose fingerprint names the file, so the mock
        // AcoustID can answer per file.
        const QString fake = s->root + "/fpcalc";
        QFile script(fake);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nname=$(basename \"$4\")\nprintf '{\"duration\": 5.0, \"fingerprint\": \"FP-%s\"}' \"$name\"\n");
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const AcoustId::Fingerprint fp = AcoustId::compute(s->music + "/Neon Coast/Night Drive/01 midnight highway.flac", fake);
        QVERIFY2(fp.ok, qPrintable(fp.error));
        QCOMPARE(fp.fingerprint, QString("FP-01 midnight highway.flac"));

        s->library->setSetting("acoustid.key", "testkey");
        Net::HttpClient http(s->library.get(), s->cache);
        Identify::Identifier identifier(*s->library, http);
        Identify::Options options;
        options.fpcalc = fake;
        options.useSearch = false;
        const QList<qint64> ids = {s->id("01 midnight highway.flac"), s->id("02 Coastline.mp3"),
                                   s->id("03 Afterglow.m4a")};
        const Identify::Result r = identifier.identify(ids, options);
        QVERIFY(r.acoustIdUsed);
        const Identify::Candidate *best = r.clusters.first().best();
        QVERIFY(best);
        QCOMPARE(best->release.id, releaseA);
        QCOMPARE(best->sources, QStringList{"acoustid"});
        QCOMPARE(best->mapping[0].via, QString("acoustid"));
        QCOMPARE(best->mapping[1].via, QString("tags"));
        int lookups = 0;
        for (const MockHttp::Request &req : m_server->requests())
            lookups += req.path == "/v2/lookup" ? 1 : 0;
        QCOMPARE(lookups, 3);

        // Without fpcalc the run says so and carries on.
        options.fpcalc = s->root + "/no-such-fpcalc";
        const Identify::Result missing = identifier.identify(ids, options);
        QVERIFY(missing.clusters.first().notes.join(' ').contains("fpcalc"));
    }

    void identifyApplyAndUndo() {
        auto s = library();
        const QList<qint64> ids = {s->id("01 midnight highway.flac"), s->id("02 Coastline.mp3"),
                                   s->id("03 Afterglow.m4a")};
        Net::HttpClient http(s->library.get(), s->cache);
        Identify::Identifier identifier(*s->library, http);
        const Identify::Result result = identifier.identify(ids, Identify::Options());
        QCOMPARE(result.clusters.size(), 1);
        QVERIFY(result.clusters.first().best());
        const Identify::Candidate best = *result.clusters.first().best();

        Identify::ApplyOptions options;
        options.sandbox = s->music;
        options.artCacheDir = s->art;
        options.coverFile = true;
        options.writeGenres = true;

        // The dry run changes nothing and fetches nothing.
        const QMap<QString, QByteArray> before = tree(s->music);
        m_server->clear();
        options.dryRun = true;
        const Identify::ApplyResult dry = Identify::apply(*s->library, http, best, options);
        QVERIFY(dry.tags.dryRun);
        QCOMPARE(dry.tags.changed, 3);
        QCOMPARE(tree(s->music), before);
        QCOMPARE(m_server->count(), 0);
        QCOMPARE(dry.cover, QString("skipped"));
        bool titleChange = false;
        for (const TagFieldChange &c : dry.tags.files.first().changes)
            titleChange = titleChange || (c.field == "title" && c.newValue == "Midnight Highway");
        QVERIFY(titleChange);

        options.dryRun = false;
        const Identify::ApplyResult r = Identify::apply(*s->library, http, best, options);
        QVERIFY2(r.tags.ok, qPrintable(QJsonDocument(r.toJson()).toJson()));
        QCOMPARE(r.cover, QString("embedded"));
        QVERIFY(!r.tags.batch.isEmpty());
        QCOMPARE(r.coverFile, s->music + "/Neon Coast/Night Drive/cover.png");
        QVERIFY(QFileInfo::exists(r.coverFile));
        // The front cover at 500, not the back.
        const QImage cover = QImage::fromData(readAll(r.coverFile));
        QCOMPARE(cover.pixelColor(0, 0), QColor(200, 30, 30));
        bool asked500 = false;
        for (const MockHttp::Request &req : m_server->requests())
            asked500 = asked500 || req.path == "/caa/img/front-500.png";
        QVERIFY(asked500);

        // Every format holds every field.
        for (const QString &name : {QString("01 midnight highway.flac"), QString("02 Coastline.mp3"),
                                    QString("03 Afterglow.m4a")}) {
            const QString path = s->library->absolutePath(s->id(name));
            const QMap<QString, QString> v = TagWriter::readFile(path);
            QCOMPARE(v.value("album"), QString("Night Drive"));
            QCOMPARE(v.value("albumartist"), QString("Neon Coast"));
            QCOMPARE(v.value("year"), QString("1997"));
            QCOMPARE(v.value("originalyear"), QString("1996"));
            QCOMPARE(v.value("tracktotal"), QString("3"));
            QCOMPARE(v.value("disc"), QString("1"));
            QCOMPARE(v.value("disctotal"), QString("1"));
            QCOMPARE(v.value("mbreleaseid"), releaseA);
            QCOMPARE(v.value("mbreleasegroupid"), groupId);
            QCOMPARE(v.value("mbalbumartistid"), QString("c0000000-0000-4000-8000-0000000000c1"));
            QCOMPARE(v.value("label"), QString("Coastal Records"));
            QCOMPARE(v.value("catalognumber"), QString("NC 001"));
            QCOMPARE(v.value("releasecountry"), QString("GB"));
            QCOMPARE(v.value("genre"), QString("Synthwave; Electronic"));
            QVERIFY2(v.value("mbrecordingid").startsWith("90000000-"), qPrintable(name + v.value("mbrecordingid")));
            QVERIFY(v.value("mbreleasetrackid").startsWith("f0000000-"));
            QVERIFY(!v.value("artwork").isEmpty());
        }
        const QMap<QString, QString> first = TagWriter::readFile(s->library->absolutePath(ids[0]));
        QCOMPARE(first.value("title"), QString("Midnight Highway"));
        QCOMPARE(first.value("isrc"), QString("GBNCA9700001"));
        const QMap<QString, QString> third = TagWriter::readFile(s->library->absolutePath(ids[2]));
        QCOMPARE(third.value("artist"), QString("Neon Coast; Lia"));
        QCOMPARE(third.value("mbartistid"),
                 QString("c0000000-0000-4000-8000-0000000000c1; c0000000-0000-4000-8000-0000000000c2"));

        // The library row follows, MB IDs included, and the album has its picture.
        QSqlQuery q(s->library->db());
        q.prepare("SELECT title, mb_track_id, mb_release_id, isrc, album_id FROM tracks WHERE id = ?");
        q.addBindValue(ids[0]);
        QVERIFY(q.exec() && q.next());
        QCOMPARE(q.value(0).toString(), QString("Midnight Highway"));
        QCOMPARE(q.value(1).toString(), QString("90000000-0000-4000-8000-000000000001"));
        QCOMPARE(q.value(2).toString(), releaseA);
        QCOMPARE(q.value(3).toString(), QString("GBNCA9700001"));
        const qint64 album = q.value(4).toLongLong();
        q.finish();
        QVERIFY(!ArtworkCache(*s->library, s->art).albumArtwork(album, 256).isEmpty());
        // The "not tagged from MusicBrainz" audit no longer lists them.
        q.exec("SELECT count(*) FROM tracks WHERE mb_track_id IS NULL OR mb_track_id = ''");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1); // the Summer Mix track

        // A second apply finds nothing to change.
        const Identify::ApplyResult again = Identify::apply(*s->library, http, best, options);
        QCOMPARE(again.tags.changed, 0);

        // Undo puts every tag back.
        const TagBatchResult undone = TagWriter(*s->library).undo(r.tags.batch);
        QVERIFY(undone.ok);
        const QMap<QString, QString> back = TagWriter::readFile(s->library->absolutePath(ids[0]));
        QCOMPARE(back.value("title"), QString("midnight highway"));
        QCOMPARE(back.value("mbreleaseid"), QString());
        QCOMPARE(back.value("artwork"), QString());
        // No temporary files were left.
        for (const QString &rel : tree(s->music).keys())
            QVERIFY2(!QFileInfo(rel).fileName().startsWith(".omamp-"), qPrintable(rel));
    }

    // --- cover art ------------------------------------------------------------

    void coverArtFront() {
        Net::HttpClient http;
        CoverArt::Fetched f = CoverArt::front(http, releaseA, 250);
        QVERIFY2(f.ok, qPrintable(f.error));
        QCOMPARE(f.releaseId, releaseA);
        QVERIFY(f.url.endsWith("/caa/img/front-250.png"));
        QCOMPARE(f.mime, QString("image/png"));
        f = CoverArt::front(http, releaseA, 0);
        QVERIFY(f.url.endsWith("/front-original.png"));
        f = CoverArt::front(http, releaseA, 900);
        QVERIFY(f.url.endsWith("/front-1200.png"));
        // No picture for the release: its group's.
        f = CoverArt::front(http, releaseB, 500, groupId);
        QVERIFY(f.ok);
        QCOMPARE(f.releaseGroupId, groupId);
        f = CoverArt::front(http, releaseB, 500);
        QVERIFY(!f.ok);
        QVERIFY(f.notFound);
        QCOMPARE(CoverArt::snapSize(300), 500);
    }

    void fetchArtForMissing() {
        auto s = library();
        Net::HttpClient http(s->library.get(), s->cache);
        const QList<qint64> missing = CoverArt::albumsMissingArt(*s->library);
        QCOMPARE(missing.size(), 2);
        CoverArt::AlbumOptions options;
        options.artCacheDir = s->art;
        options.sandbox = s->music;
        options.saveFile = true;
        options.dryRun = true;
        const QMap<QString, QByteArray> before = tree(s->music);
        QList<CoverArt::AlbumResult> results;
        for (qint64 album : missing)
            results << CoverArt::fetchForAlbum(*s->library, http, album, options);
        QCOMPARE(tree(s->music), before);
        QStringList statuses;
        for (const auto &r : results)
            statuses << r.status;
        statuses.sort();
        // Summer Mix: the search's releases don't match its title.
        QCOMPARE(statuses, (QStringList{"no-match", "would-fetch"}));

        options.dryRun = false;
        options.embed = true;
        CoverArt::AlbumResult night;
        for (qint64 album : missing) {
            const CoverArt::AlbumResult r = CoverArt::fetchForAlbum(*s->library, http, album, options);
            if (r.album == "Night Drive")
                night = r;
        }
        QCOMPARE(night.status, QString("embedded"));
        QCOMPARE(night.how, QString("search"));
        QCOMPARE(night.releaseId, releaseA); // the closest title, not the search's first
        QVERIFY(QFileInfo::exists(night.picture));
        QVERIFY(night.picture.startsWith(s->art + "/fetched/"));
        QVERIFY(QFileInfo::exists(night.coverFile));
        QVERIFY(!night.batch.isEmpty());
        QVERIFY(!ArtworkCache(*s->library, s->art).albumArtwork(night.albumId, 96).isEmpty());
        QCOMPARE(CoverArt::albumsMissingArt(*s->library).size(), 1);
        // Undo takes the embedded pictures out again.
        QVERIFY(TagWriter(*s->library).undo(night.batch).ok);
        QVERIFY(TagWriter::readFile(s->music + "/Neon Coast/Night Drive/02 Coastline.mp3").value("artwork").isEmpty());
    }

    // --- lyrics ---------------------------------------------------------------

    void lrcParser() {
        const Lyrics::Parsed p = Lyrics::parseLrc(
            "\xEF\xBB\xBF[ar:Neon Coast]\r\n[ti:Coastline]\r\n[offset:+500]\r\n"
            "[00:12.00][01:02.50]Chorus line\r\n"
            "[00:05.10]First <00:05.10>word <00:05.60>by <00:06.00>word\r\n"
            "[00:07:30]Colon fraction\r\n[00:08]No fraction\r\n[00:09.123]Milliseconds\r\n[00:10.00]\r\n");
        QVERIFY(p.synced);
        QCOMPARE(p.tags.value("ar"), QString("Neon Coast"));
        QCOMPARE(p.tags.value("ti"), QString("Coastline"));
        QCOMPARE(p.offsetMs, qint64(500));
        QCOMPARE(p.lines.size(), 7);
        QCOMPARE(p.lines[0].timeMs, qint64(4600)); // 5.1 s less the offset
        QCOMPARE(p.lines[0].text, QString("First word by word"));
        QCOMPARE(p.lines[1].timeMs, qint64(6800));
        QCOMPARE(p.lines[2].timeMs, qint64(7500));
        QCOMPARE(p.lines[3].timeMs, qint64(8623));
        QCOMPARE(p.lines[4].text, QString()); // a blank, timed line
        QCOMPARE(p.lines[5].text, QString("Chorus line"));
        QCOMPARE(p.lines[6].timeMs, qint64(62000));
        QCOMPARE(p.lineAt(0), -1);
        QCOMPARE(p.lineAt(4600), 0);
        QCOMPARE(p.lineAt(11000), 4);
        QCOMPARE(p.lineAt(999999), 6);
        QCOMPARE(Lyrics::formatStamp(62500), QString("[01:02.50]"));
        const Lyrics::Parsed again = Lyrics::parseLrc(Lyrics::toLrc(p));
        QCOMPARE(again.lines.size(), p.lines.size());
        QCOMPARE(again.lines[6].timeMs, qint64(62000));

        const Lyrics::Parsed plain = Lyrics::parseLrc("One\n\nTwo\n");
        QVERIFY(!plain.synced);
        QCOMPARE(plain.lines.size(), 3);
        QCOMPARE(plain.lineAt(5000), -1);
        QCOMPARE(plain.plainText(), QString("One\n\nTwo"));
        QVERIFY(Lyrics::parseLrc("").isEmpty());
    }

    void lyricsFromLrclib() {
        auto s = library(false);
        QVERIFY(Fixture::fill(*s->library));
        Net::HttpClient http(s->library.get(), s->cache);
        QSqlQuery q(s->library->db());
        q.exec("SELECT id FROM tracks WHERE title = 'Coastline'");
        QVERIFY(q.next());
        const qint64 coastline = q.value(0).toLongLong();
        q.exec("SELECT id FROM tracks WHERE title = 'Midnight Highway'");
        QVERIFY(q.next());
        const qint64 midnight = q.value(0).toLongLong();
        q.exec("SELECT id FROM tracks WHERE title = 'Heatwave'");
        QVERIFY(q.next());
        const qint64 heatwave = q.value(0).toLongLong();
        q.finish();

        // Nothing on disk or cached, and fetching is off: nothing is sent.
        Lyrics::Entry e = Lyrics::forTrack(*s->library, &http, coastline);
        QVERIFY(!e.found());
        QCOMPARE(m_server->count(), 0);

        Lyrics::Options fetch;
        fetch.fetch = true;
        e = Lyrics::forTrack(*s->library, &http, coastline, fetch);
        QVERIFY(e.found());
        QCOMPARE(e.source, QString("lrclib"));
        QCOMPARE(e.lrclibId, qint64(3396226));
        QVERIFY(e.synced.startsWith("[00:01.50]"));
        QCOMPARE(e.parsed().lines.size(), 4);
        const MockHttp::Request get = m_server->requests().last();
        QCOMPARE(get.path, QByteArray("/api/get"));
        QCOMPARE(get.item("artist_name"), QString("Neon Coast"));
        QCOMPARE(get.item("track_name"), QString("Coastline"));
        QCOMPARE(get.item("album_name"), QString("Night Drive"));
        QCOMPARE(get.item("duration"), QString("185"));

        // Cached in the table: no second request, even with a new client.
        Net::HttpClient other(s->library.get(), s->root + "/other-cache");
        m_server->clear();
        e = Lyrics::forTrack(*s->library, &other, coastline, fetch);
        QCOMPARE(e.source, QString("lrclib"));
        QCOMPARE(m_server->count(), 0);
        QCOMPARE(Lyrics::cached(*s->library, coastline).lrclibId, qint64(3396226));

        // Not found exactly: the search's closest length wins (246 s against 245).
        e = Lyrics::forTrack(*s->library, &http, midnight, fetch);
        QVERIFY(e.found());
        QCOMPARE(e.lrclibId, qint64(502));
        QCOMPARE(m_server->requests().last().path, QByteArray("/api/search"));

        // Nowhere: "none" is remembered, and asked again only after 30 days.
        e = Lyrics::forTrack(*s->library, &http, heatwave, fetch);
        QVERIFY(!e.found());
        QCOMPARE(Lyrics::cached(*s->library, heatwave).source, QString("none"));
        m_server->clear();
        e = Lyrics::forTrack(*s->library, &http, heatwave, fetch);
        QCOMPARE(m_server->count(), 0);
        QVERIFY(e.error.contains("no lyrics"));
        QSqlQuery old(s->library->db());
        old.exec("UPDATE lyrics SET fetched = fetched - 31 * 86400 WHERE source = 'none'");
        Lyrics::forTrack(*s->library, &other, heatwave, fetch);
        QVERIFY(m_server->count() > 0);
    }

    void lyricsOnDiskAndWriting() {
        auto s = library();
        const qint64 flac = s->id("01 midnight highway.flac");
        const qint64 mp3 = s->id("02 Coastline.mp3");
        const QString flacPath = s->library->absolutePath(flac);
        const QString mp3Path = s->library->absolutePath(mp3);
        QVERIFY(!Lyrics::local(flacPath).found());

        // An LRCLIB answer, written as a .lrc beside the track and undone.
        Lyrics::Entry e;
        e.trackId = flac;
        e.source = "lrclib";
        e.plain = "Engine hum\nRoad goes on";
        e.synced = "[00:01.00]Engine hum\n[00:02.50]Road goes on";
        Lyrics::WriteResult w = Lyrics::write(*s->library, {e}, "lrc", false, true, nullptr, s->music);
        QCOMPARE(w.tags.changed, 1);
        QVERIFY(!QFileInfo::exists(Lyrics::sidecarPath(flacPath))); // dry run
        w = Lyrics::write(*s->library, {e}, "lrc", false, false, nullptr, s->music);
        QVERIFY2(w.tags.ok, qPrintable(QJsonDocument(w.tags.toJson()).toJson()));
        QVERIFY(!w.tags.batch.isEmpty());
        const QString lrc = Lyrics::sidecarPath(flacPath);
        QVERIFY(lrc.endsWith("/01 midnight highway.lrc"));
        QCOMPARE(readAll(lrc), e.synced.toUtf8() + "\n");
        Lyrics::Entry local = Lyrics::local(flacPath);
        QCOMPARE(local.source, QString("sidecar"));
        QCOMPARE(local.synced, e.synced);
        QCOMPARE(local.plain, e.plain);
        QCOMPARE(Lyrics::forTrack(*s->library, nullptr, flac).source, QString("sidecar"));
        QVERIFY(TagWriter(*s->library).undo(w.tags.batch).ok);
        QVERIFY(!QFileInfo::exists(lrc));

        // Plain lyrics into the tag (USLT in the MP3).
        Lyrics::Entry m = e;
        m.trackId = mp3;
        w = Lyrics::write(*s->library, {m}, "tag", false, false, nullptr, s->music);
        QVERIFY(w.tags.ok);
        local = Lyrics::local(mp3Path);
        QCOMPARE(local.source, QString("embedded"));
        QCOMPARE(local.plain, e.plain);
        QVERIFY(local.synced.isEmpty());
        // ... or the LRC text, which reads back as synced.
        w = Lyrics::write(*s->library, {m}, "tag", true, false, nullptr, s->music);
        QVERIFY(w.tags.ok);
        local = Lyrics::local(mp3Path);
        QCOMPARE(local.synced, e.synced);
        QCOMPARE(local.plain, e.plain);
        // Lyrics already in that file are not written back into it.
        w = Lyrics::write(*s->library, {local}, "tag");
        QCOMPARE(w.skipped, QList<qint64>{0});
        QVERIFY(!Lyrics::write(*s->library, {m}, "sing").error.isEmpty());

        // SYLT in an MP3, in milliseconds.
        {
            TagLib::MPEG::File file(QFile::encodeName(mp3Path).constData());
            auto *sylt = new TagLib::ID3v2::SynchronizedLyricsFrame(TagLib::String::UTF8);
            sylt->setTimestampFormat(TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds);
            sylt->setSynchedText({{1500, "Down by the water"}, {3200, "Lights on the bay"}});
            file.ID3v2Tag(true)->addFrame(sylt);
            QVERIFY(file.save());
        }
        local = Lyrics::local(mp3Path);
        QCOMPARE(local.source, QString("embedded"));
        QCOMPARE(local.synced, QString("[00:01.50]Down by the water\n[00:03.20]Lights on the bay"));
    }

    // --- scrobbling -----------------------------------------------------------

    void scrobbleRule() {
        QVERIFY(!Scrobble::qualifies(29000, 29000));        // too short a track
        QVERIFY(Scrobble::qualifies(15000, 30000));         // half of it
        QVERIFY(!Scrobble::qualifies(14999, 30000));
        QVERIFY(Scrobble::qualifies(240000, 3600000));      // four minutes of an hour
        QVERIFY(!Scrobble::qualifies(239000, 3600000));
        QVERIFY(Scrobble::qualifies(240000, 0));            // length unknown
        QVERIFY(!Scrobble::qualifies(100000, 0));
    }

    void scrobbleListenBrainz() {
        auto s = library(false);
        QVERIFY(Fixture::fill(*s->library));
        Net::HttpClient http(s->library.get(), s->cache);
        Scrobble::Scrobbler scrobbler(*s->library, &http);
        QSqlQuery q(s->library->db());
        q.exec("SELECT id FROM tracks WHERE title = 'Midnight Highway'");
        QVERIFY(q.next());
        const qint64 midnight = q.value(0).toLongLong();
        q.exec("SELECT id FROM tracks WHERE title = 'Coastline'");
        QVERIFY(q.next());
        const qint64 coastline = q.value(0).toLongLong();
        q.finish();

        // No service set up: nothing is queued.
        scrobbler.trackStarted(midnight, 1792494000);
        QCOMPARE(scrobbler.trackFinished(midnight, 200000), 0);

        s->library->setSetting("listenbrainz.token", "good-token");
        QCOMPARE(scrobbler.enabledServices(), QStringList{"listenbrainz"});
        scrobbler.trackStarted(midnight, 1792494000);
        scrobbler.trackProgress(midnight, 130000);
        // Progress counts: 130 s of 245 is past half.
        QCOMPARE(scrobbler.trackFinished(midnight, 0), 1);
        scrobbler.trackStarted(coastline, 1792494300);
        QCOMPARE(scrobbler.trackFinished(coastline, 20000), 0); // skipped early
        scrobbler.trackStarted(coastline, 1792494400);
        QCOMPARE(scrobbler.trackFinished(coastline, 185000), 1);
        QCOMPARE(m_server->count(), 0); // the hooks never touch the network

        // Now playing, then both listens in one import.
        scrobbler.trackStarted(midnight);
        const Scrobble::FlushResult r = scrobbler.flush();
        QCOMPARE(r.sent, 2);
        QVERIFY(r.nowPlayingSent);
        QCOMPARE(r.remaining, 0);
        const QList<MockHttp::Request> sent = m_server->requests();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent[0].headers.value("authorization"), QByteArray("Token good-token"));
        const QJsonObject now = QJsonDocument::fromJson(sent[0].body).object();
        QCOMPARE(now["listen_type"].toString(), QString("playing_now"));
        QVERIFY(!now["payload"].toArray().first().toObject().contains("listened_at"));
        const QJsonObject import = QJsonDocument::fromJson(sent[1].body).object();
        QCOMPARE(import["listen_type"].toString(), QString("import"));
        const QJsonArray payload = import["payload"].toArray();
        QCOMPARE(payload.size(), 2);
        QCOMPARE(payload[0].toObject()["listened_at"].toInteger(), qint64(1792494000));
        const QJsonObject meta = payload[0].toObject()["track_metadata"].toObject();
        QCOMPARE(meta["artist_name"].toString(), QString("Neon Coast"));
        QCOMPARE(meta["track_name"].toString(), QString("Midnight Highway"));
        QCOMPARE(meta["release_name"].toString(), QString("Night Drive"));
        QCOMPARE(meta["additional_info"].toObject()["duration_ms"].toInteger(), qint64(245000));
        QCOMPARE(meta["additional_info"].toObject()["submission_client"].toString(), QString("Omamp"));
        QCOMPARE(scrobbler.status().first().sent, 2);

        // Offline: the listen waits, and goes when the service is back.
        m_server->setHandler([](const MockHttp::Request &) { return MockHttp::Reply{503, "{}", "application/json", {}}; });
        Net::Config::setMaxRetries(0);
        scrobbler.trackStarted(coastline, 1792495000);
        QCOMPARE(scrobbler.trackFinished(coastline, 185000), 1);
        Scrobble::FlushResult busy = scrobbler.flush();
        QCOMPARE(busy.sent, 0);
        QCOMPARE(busy.remaining, 1);
        QCOMPARE(scrobbler.status().first().pending, 1);
        m_server->setHandler([this](const MockHttp::Request &r) { return route(r); });
        busy = scrobbler.flush();
        QCOMPARE(busy.sent, 1);
        QCOMPARE(busy.remaining, 0);

        // A bad listen is set aside alone; the good one in its batch goes.
        Scrobble::Listen bad = Scrobble::Listen::fromTrack(*s->library, midnight);
        bad.artist = "Bad";
        bad.listenedAt = 1792496000;
        Scrobble::Listen good = Scrobble::Listen::fromTrack(*s->library, coastline);
        good.listenedAt = 1792496300;
        scrobbler.enqueue("listenbrainz", bad);
        scrobbler.enqueue("listenbrainz", good);
        const Scrobble::FlushResult mixed = scrobbler.flush();
        QCOMPARE(mixed.sent, 1);
        QCOMPARE(mixed.failed, 1);
        QCOMPARE(scrobbler.status().first().failed, 1);

        // A refused token keeps the listens and says what to do.
        s->library->setSetting("listenbrainz.token", "old-token");
        scrobbler.enqueue("listenbrainz", good);
        const Scrobble::FlushResult refused = scrobbler.flush();
        QCOMPARE(refused.sent, 0);
        QCOMPARE(refused.remaining, 1);
        QVERIFY(refused.errors.join(' ').contains("login listenbrainz"));

        QString user, error;
        Net::HttpClient fresh;
        QVERIFY(Scrobble::ListenBrainz::validateToken(fresh, "good-token", &user, &error));
        QCOMPARE(user, QString("testlistener"));
        QVERIFY(!Scrobble::ListenBrainz::validateToken(fresh, "old-token", &user, &error));
        QVERIFY(error.contains("invalid"));
    }

    void scrobbleLastFm() {
        // The api_sig as Last.fm documents it: md5 of sorted name+value pairs, then the secret.
        const QMap<QString, QString> params{{"method", "auth.getToken"}, {"api_key", "k"}, {"format", "json"}};
        QCOMPARE(Scrobble::LastFm::signature(params, "s"),
                 QString::fromLatin1(QCryptographicHash::hash("api_keykmethodauth.getTokens", QCryptographicHash::Md5).toHex()));
        QCOMPARE(Scrobble::LastFm::authUrl("k", "t"), QString("https://www.last.fm/api/auth/?api_key=k&token=t"));

        auto s = library(false);
        QVERIFY(Fixture::fill(*s->library));
        Net::HttpClient http(s->library.get(), s->cache);
        QString token, session, user, error;
        QVERIFY(Scrobble::LastFm::getToken(http, "k", "s", &token, &error));
        QCOMPARE(token, QString("tok123"));
        QVERIFY(!m_server->requests().last().item("api_sig").isEmpty());
        QVERIFY(Scrobble::LastFm::getSession(http, "k", "s", token, &session, &user, &error));
        QCOMPARE(session, QString("sess"));
        QCOMPARE(user, QString("fmuser"));
        QVERIFY(!Scrobble::LastFm::getSession(http, "k", "s", "unapproved", &session, &user, &error));
        QVERIFY(error.contains("Unauthorized Token"));

        s->library->setSetting("lastfm.api_key", "k");
        s->library->setSetting("lastfm.api_secret", "s");
        s->library->setSetting("lastfm.session_key", "sess");
        Scrobble::Scrobbler scrobbler(*s->library, &http);
        QCOMPARE(scrobbler.enabledServices(), QStringList{"lastfm"});
        QSqlQuery q(s->library->db());
        q.exec("SELECT id FROM tracks WHERE title IN ('Midnight Highway', 'Coastline') ORDER BY title DESC");
        QList<qint64> ids;
        while (q.next())
            ids << q.value(0).toLongLong();
        scrobbler.trackStarted(ids[0], 1792494000);
        QCOMPARE(scrobbler.trackFinished(ids[0], 245000), 1);
        scrobbler.trackStarted(ids[1], 1792494300);
        QCOMPARE(scrobbler.trackFinished(ids[1], 185000), 1);
        m_server->clear();
        const Scrobble::FlushResult r = scrobbler.flush();
        QCOMPARE(r.sent, 2);
        const MockHttp::Request sent = m_server->requests().last();
        QCOMPARE(sent.formItem("method"), QString("track.scrobble"));
        QCOMPARE(sent.formItem("artist[0]"), QString("Neon Coast"));
        QCOMPARE(sent.formItem("track[1]"), QString("Coastline"));
        QCOMPARE(sent.formItem("timestamp[0]"), QString("1792494000"));
        QCOMPARE(sent.formItem("duration[0]"), QString("245"));
        QCOMPARE(sent.formItem("format"), QString("json"));
        QVERIFY(!sent.formItem("api_sig").isEmpty());

        // A dead session keeps the listens.
        s->library->setSetting("lastfm.session_key", "expired");
        scrobbler.enqueue("lastfm", Scrobble::Listen::fromTrack(*s->library, ids[0]));
        const Scrobble::FlushResult dead = scrobbler.flush();
        QCOMPARE(dead.sent, 0);
        QCOMPARE(dead.remaining, 1);
        QVERIFY(dead.errors.join(' ').contains("login lastfm"));
    }

    // --- schema and commands --------------------------------------------------

    void schema8() {
        const QString path = m_tmp.path() + "/schema8.db";
        {
            Library old(path);
            QVERIFY(old.open(nullptr, 7));
            QCOMPARE(old.schemaVersion(), 7);
        }
        Library library(path);
        QVERIFY(library.open());
        QVERIFY(Library::latestVersion() >= 8);
        QCOMPARE(library.schemaVersion(), Library::latestVersion());
        QSqlQuery q(library.db());
        QVERIFY(q.exec("SELECT track_id, plain, synced, source, lrclib_id, instrumental, fetched FROM lyrics"));
        QVERIFY(q.exec("SELECT id, service, track_id, listened_at, duration_ms, payload, state, attempts, last_error, "
                       "created, sent FROM scrobble_queue"));
        // Only the two services are allowed.
        QVERIFY(!q.exec("INSERT INTO scrobble_queue (service, listened_at, payload, created) VALUES ('myspace', 1, '{}', 1)"));
    }

    void commands() {
        auto s = library();
        auto cli = [&](QStringList args, QString *output = nullptr) {
            args << "--db" << s->db;
            QString o, e;
            QTextStream out(&o), err(&e);
            const int code = Cli::run(args, out, err);
            if (output)
                *output = o;
            if (code != 0 && !e.isEmpty())
                qWarning().noquote() << args.join(' ') << e;
            return code;
        };
        auto jsonOf = [](const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); };
        QVERIFY(Cli::isCommand("identify") && Cli::isCommand("lyrics") && Cli::isCommand("scrobble") &&
                Cli::isCommand("fetch-art") && Cli::isCommand("net"));

        QString output;
        QCOMPARE(cli({"net", "status", "--json"}, &output), 0);
        QJsonObject j = jsonOf(output);
        QCOMPARE(j["user_agent"].toString(), Net::userAgent());
        QCOMPARE(j["acoustid_key"].toBool(), false);
        QCOMPARE(cli({"net", "set", "acoustid.key", "s3cret"}, &output), 0);
        QVERIFY(!output.contains("s3cret")); // secrets are never printed
        QCOMPARE(cli({"net", "status", "--json"}, &output), 0);
        QCOMPARE(jsonOf(output)["acoustid_key"].toBool(), true);
        QCOMPARE(cli({"net", "unset", "acoustid.key"}), 0);
        QCOMPARE(cli({"net", "set", "tags.write_ratings", "1"}), 1); // not a web setting

        // identify: a dry run by default, with the candidates and the changes.
        const QMap<QString, QByteArray> before = tree(s->music);
        QCOMPARE(cli({"identify", "album:\"night drive\"", "--json"}, &output), 0);
        j = jsonOf(output);
        QCOMPARE(j["dry_run"].toBool(), true);
        QJsonObject cluster = j["clusters"].toArray().first().toObject();
        QCOMPARE(cluster["candidates"].toArray().first().toObject()["release"].toObject()["id"].toString(), releaseA);
        QCOMPARE(cluster["changes"].toObject()["changed"].toInt(), 3);
        QVERIFY(j["notes"].toArray().first().toString().contains("AcoustID"));
        QCOMPARE(tree(s->music), before);

        // A threshold above the score refuses to write.
        QCOMPARE(cli({"identify", "album:\"night drive\"", "--apply", "--min-score", "0.999", "--json"}, &output), 0);
        QCOMPARE(jsonOf(output)["skipped"].toInt(), 1);
        QCOMPARE(tree(s->music), before);
        // --release needs one album.
        QCOMPARE(cli({"identify", "path:music", "--release", releaseA}), 1);

        QCOMPARE(cli({"identify", "album:\"night drive\"", "--apply", "--no-art", "--json"}, &output), 0);
        j = jsonOf(output);
        QCOMPARE(j["applied"].toInt(), 1);
        const QString batch =
            j["clusters"].toArray().first().toObject()["applied"].toObject()["batch"].toString();
        QVERIFY(!batch.isEmpty());
        QCOMPARE(TagWriter::readFile(s->music + "/Neon Coast/Night Drive/02 Coastline.mp3").value("mbreleaseid"), releaseA);
        QCOMPARE(cli({"undo", batch, "--json"}), 0);

        // lyrics: fetched, then written as a sidecar.
        QCOMPARE(cli({"lyrics", "title:coastline", "--fetch", "--json"}, &output), 0);
        j = jsonOf(output);
        QCOMPARE(j["found"].toInt(), 1);
        QCOMPARE(j["tracks"].toArray().first().toObject()["source"].toString(), QString("lrclib"));
        QCOMPARE(cli({"lyrics", "title:coastline", "--write", "lrc", "--json"}, &output), 0);
        QVERIFY(QFileInfo::exists(s->music + "/Neon Coast/Night Drive/02 Coastline.lrc"));
        QCOMPARE(cli({"lyrics", "title:coastline"}, &output), 0);
        QVERIFY(output.contains("synced lyrics from sidecar"));
        QVERIFY(output.contains("[00:01.50]"));
        QCOMPARE(cli({"lyrics", "title:coastline", "--write", "sing"}), 1);

        // fetch-art for albums missing art.
        QCOMPARE(cli({"fetch-art", "--missing", "--dry-run", "--json"}, &output), 0);
        j = jsonOf(output);
        QCOMPARE(j["albums"].toArray().size(), 2);
        QCOMPARE(j["counts"].toObject()["would-fetch"].toInt(), 1);

        // scrobble: log in (the token is checked first), status, flush.
        QCOMPARE(cli({"scrobble", "login", "listenbrainz", "--token", "bad-token"}), 1);
        QCOMPARE(cli({"scrobble", "login", "listenbrainz", "--token", "good-token", "--json"}, &output), 0);
        QCOMPARE(jsonOf(output)["user"].toString(), QString("testlistener"));
        QCOMPARE(cli({"scrobble", "status", "--json"}, &output), 0);
        QJsonObject lb = jsonOf(output)["services"].toArray().first().toObject();
        QCOMPARE(lb["configured"].toBool(), true);
        QCOMPARE(lb["user"].toString(), QString("testlistener"));
        QVERIFY(!output.contains("good-token"));
        Scrobble::Scrobbler(*s->library).enqueue("listenbrainz", Scrobble::Listen::fromTrack(*s->library, s->id("02 Coastline.mp3")));
        QCOMPARE(cli({"scrobble", "flush", "--json"}, &output), 0);
        QCOMPARE(jsonOf(output)["sent"].toInt(), 1);
        QCOMPARE(cli({"scrobble", "login", "lastfm"}), 1); // no API key yet
        QCOMPARE(cli({"net", "set", "lastfm.api_key", "k"}), 0);
        QCOMPARE(cli({"net", "set", "lastfm.api_secret", "s"}), 0);
        QCOMPARE(cli({"scrobble", "login", "lastfm", "--json"}, &output), 0);
        QVERIFY(jsonOf(output)["auth_url"].toString().contains("token=tok123"));
        QCOMPARE(cli({"scrobble", "login", "lastfm", "--complete", "--json"}, &output), 0);
        QCOMPARE(jsonOf(output)["user"].toString(), QString("fmuser"));
        QCOMPARE(cli({"scrobble", "logout", "listenbrainz"}), 0);
        QCOMPARE(cli({"scrobble", "status", "--json"}, &output), 0);
        QCOMPARE(jsonOf(output)["services"].toArray().first().toObject()["configured"].toBool(), false);
    }

    // --- the real services, once, by hand ---------------------------------------

    void liveSmoke() {
        if (qEnvironmentVariable("OMAMP_LIVE_TESTS") != "1")
            QSKIP("Talks to the real MusicBrainz and LRCLIB; run with OMAMP_LIVE_TESTS=1");
        // The real addresses and the real, polite pace.
        const QStringList overrides = {"OMAMP_MUSICBRAINZ_URL", "OMAMP_LRCLIB_URL"};
        QMap<QString, QByteArray> saved;
        for (const QString &name : overrides) {
            saved.insert(name, qgetenv(name.toLatin1().constData()));
            qunsetenv(name.toLatin1().constData());
        }
        auto restore = qScopeGuard([&] {
            for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
                qputenv(it.key().toLatin1().constData(), it.value());
        });
        Net::Config::resetForTests();
        Net::HttpClient http(nullptr, m_tmp.path() + "/live-cache");
        http.setCacheEnabled(false);

        // One search and one lookup: the search's shape, then a full release.
        MusicBrainz::Client mb(http);
        QString error;
        const QList<MusicBrainz::Release> found = mb.searchReleases("Nirvana", "Nevermind", 0, 5, &error);
        QVERIFY2(!found.isEmpty(), qPrintable(error));
        QVERIFY(found.first().searchScore > 0);
        QVERIFY(!found.first().id.isEmpty());
        QVERIFY(found.first().trackCount() > 0);
        const MusicBrainz::Release r = mb.release(found.first().id, &error);
        QVERIFY2(!r.id.isEmpty(), qPrintable(error));
        QVERIFY(r.complete);
        QVERIFY(!r.media.isEmpty());
        QVERIFY(!r.media.first().tracks.isEmpty());
        QVERIFY(!r.media.first().tracks.first().recordingId.isEmpty());
        QVERIFY(r.media.first().tracks.first().lengthMs > 0);
        QVERIFY(!r.releaseGroupId.isEmpty());
        QVERIFY(!MusicBrainz::creditText(r.credits).isEmpty());
        qInfo().noquote() << "live MusicBrainz:" << r.title << "by" << MusicBrainz::creditText(r.credits) << r.date
                          << r.country << r.formats() << r.trackCount() << "tracks, labels" << r.labels.size()
                          << "genres" << r.genres.size();

        // One LRCLIB get: the example in LRCLIB's own documentation.
        Lyrics::Query q;
        q.artist = "Borislav Slavov";
        q.title = "I Want to Live";
        q.album = "Baldur's Gate 3 (Original Game Soundtrack)";
        q.durationMs = 233000;
        const Lyrics::Entry e = Lyrics::fetchLrclib(http, q);
        QVERIFY2(e.source == "lrclib", qPrintable(e.error));
        QVERIFY(e.lrclibId > 0);
        QVERIFY(e.found());
        qInfo().noquote() << "live LRCLIB: id" << e.lrclibId << "synced" << !e.synced.isEmpty() << "lines"
                          << e.parsed().lines.size();
        QCOMPARE(http.requestsSent(), 3);
    }
};

int runWebTests(int argc, char **argv) {
    WebTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "web_tests.moc"
