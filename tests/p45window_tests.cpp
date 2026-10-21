// Phases 4 and 5 in the window: identify with MusicBrainz (review, save,
// undo), the lyrics view, the Volumes page with a share that hangs, the
// mount guide, Navidrome / Subsonic servers, and the scrobbling log-ins.
//
// Every web service is MockHttp on 127.0.0.1 serving the fixtures in
// tests/fixtures/web; the Subsonic server is MockSubsonic. Nothing here
// reaches the internet, a sound card, the desktop keyring or a real mount:
// shares are a fake mount table and a stat() hook, secrets go to a file
// under a temporary XDG_CONFIG_HOME (main() sets THRUM_SECRET_STORE=file).

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <sys/stat.h>

#include "artprovider.h"
#include "artwork.h"
#include "http.h"
#include "library.h"
#include "librarycontroller.h"
#include "nowplaying_context.h"
#include "mockhttp.h"
#include "mocksubsonic.h"
#include "mounts.h"
#include "nascontroller.h"
#include "player.h"
#include "playrecorder.h"
#include "remote.h"
#include "scanner.h"
#include "scrobble.h"
#include "settingscontroller.h"
#include "tagwriter.h"
#include "theme.h"
#include "translator.h"
#include "volumes.h"
#include "webcontroller.h"

namespace {

const QString releaseA = "a1b2c3d4-0001-4000-8000-000000000001";
const QString releaseB = "a1b2c3d4-0002-4000-8000-000000000002";
const QString groupId = "b0000000-0000-4000-8000-0000000000aa";

QByteArray readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void write(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
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

bool haveFfmpeg() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }

// A quiet tone of a given length with tags; nothing is played.
bool tone(const QString &file, double seconds, const QStringList &extra) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds),
                        "-af", "volume=0.05", "-map_metadata", "-1"};
    QProcess p;
    p.start("ffmpeg", args + extra + QStringList{file});
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

QVariant sql(Library &library, const QString &query, const QVariantList &params = {}) {
    QSqlQuery q(library.db());
    q.prepare(query);
    for (const QVariant &p : params)
        q.addBindValue(p);
    return q.exec() && q.next() ? q.value(0) : QVariant();
}

// Picks a waiting signal up even if it already came (QSignalSpy::wait()
// only counts signals that arrive while it waits).
bool seen(QSignalSpy &spy, int timeout = 30000) { return !spy.isEmpty() || spy.wait(timeout); }

// A fake machine's mount table, and a stat() hook that hangs a share.
struct FakeMounts {
    QTemporaryDir dir;
    QString mountInfo() const { return dir.filePath("mountinfo"); }
    void setMounts(const QList<QStringList> &mounts) {
        QByteArray text = "1 0 0:1 / / rw - rootfs rootfs rw\n";
        int id = 2;
        for (const QStringList &m : mounts)
            text += QString("%1 1 0:%1 %2 %3 rw,relatime shared:1 - %4 %5 rw\n").arg(id++).arg(m[1], m[0], m[2], m[3]).toUtf8();
        write(mountInfo(), text);
    }
    VolumeResolver resolver() const { return VolumeResolver(mountInfo(), dir.filePath("by-uuid")); }
};

struct StatTrap {
    std::mutex mutex;
    std::condition_variable released;
    QByteArray prefix;
    bool hang = false, open = false;
    void install() {
        PathProbe::setStatHook([this](const QByteArray &path, struct stat *st) {
            std::unique_lock<std::mutex> lock(mutex);
            if (hang && !prefix.isEmpty() && path.startsWith(prefix))
                released.wait_for(lock, std::chrono::seconds(10), [&] { return open; });
            lock.unlock();
            return ::stat(path.constData(), st) == 0 ? 0 : errno;
        });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
            hang = false;
        }
        released.notify_all();
    }
};

// A player that never reaches a device (main() sets THRUM_NO_AUDIO_DEVICE).
Player *quietPlayer(const QString &statePath) { return new Player(statePath); }

// Main.qml over a library, with everything in temporary folders.
struct Window {
    QString root;
    Translator translator;
    Theme theme;
    std::unique_ptr<Player> player;
    std::unique_ptr<LibraryController> library;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    explicit Window(const QString &folder)
        : root(folder), translator(folder + "/language"), theme(folder + "/theme", nullptr, folder + "/no-hyprctl") {}

    // configure runs on the controller before the window loads. advanced
    // false starts on the deck, so the library opens only when asked.
    bool load(const std::function<void(LibraryController &)> &configure = {}, bool advanced = true) {
        {
            QSettings conf(root + "/window.conf", QSettings::IniFormat);
            conf.setValue("window/advanced", advanced);
        }
        player.reset(quietPlayer(root + "/state.json"));
        library = std::make_unique<LibraryController>(root + "/library.db");
        library->setPlayer(player.get());
        library->setWatching(false);
        library->setArtCacheDir(root + "/art");
        library->setWriteSandbox(root);
        library->recorder()->setFlushing(false);
        library->web()->setPreviewDir(root + "/previews");
        if (configure)
            configure(*library);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", player.get());
        engine.rootContext()->setContextProperty("library", library.get());
        engine.rootContext()->setContextProperty("commandPalette", library->palette());
        engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(root + "/window.conf"));
        installNowPlaying(engine, &theme, player.get(), root + "/library.db", root + "/art");
        engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider(root + "/library.db", root + "/art"));
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().size() != 1)
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        return window && QTest::qWaitForWindowExposed(window);
    }
    static QObject *findItem(QQuickItem *item, const QString &name) {
        if (item->objectName() == name)
            return item;
        for (QQuickItem *child : item->childItems())
            if (QObject *o = findItem(child, name))
                return o;
        return nullptr;
    }
    QObject *find(const char *name) const {
        QObject *o = window->findChild<QObject *>(name);
        if (!o)
            o = findItem(window->contentItem(), QString::fromLatin1(name));
        if (!o && window->contentItem()->parentItem())
            o = findItem(window->contentItem()->parentItem(), QString::fromLatin1(name));
        return o;
    }
    void type(const QString &text) {
        for (const QChar c : text)
            QTest::keyClick(window, c.toLatin1());
    }
    bool palette(const QString &text, const QString &id) {
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        type(">" + text);
        if (!QTest::qWaitFor([&] { return library->palette()->get(0).value("itemId").toString() == id; }, 3000))
            return false;
        QTest::keyClick(window, Qt::Key_Return);
        return true;
    }
};

} // namespace

class P45WindowTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<MockHttp> m_server;
    QString m_pristine;
    int m_counter = 0;
    QMap<QByteArray, QByteArray> m_savedEnv;
    StatTrap m_trap;

    void setEnv(const char *name, const QByteArray &value) {
        if (!m_savedEnv.contains(name))
            m_savedEnv.insert(name, qgetenv(name));
        qputenv(name, value);
    }
    QByteArray fixture(const QString &name) const {
        QByteArray b = readAll(QString(THRUM_TESTS_DIR) + "/fixtures/web/" + name);
        b.replace("{{BASE}}", (m_server->url() + "/caa").toUtf8());
        return b;
    }

    // MusicBrainz, the Cover Art Archive, LRCLIB, ListenBrainz and Last.fm,
    // as their documentation shows them.
    MockHttp::Reply route(const MockHttp::Request &r) {
        const QString path = QString::fromUtf8(r.path);
        auto json = [](const QByteArray &body, int status = 200) { return MockHttp::Reply{status, body, "application/json", {}}; };
        if (path == "/ws/2/release/" + releaseA)
            return json(fixture("mb-release-a.json"));
        if (path == "/ws/2/release/" + releaseB)
            return json(fixture("mb-release-b.json"));
        if (path.startsWith("/ws/2/release/"))
            return json("{\"error\":\"Not Found\"}", 404);
        if (path == "/ws/2/release")
            return json(fixture("mb-release-search.json"));
        if (path == "/ws/2/recording")
            return json(fixture("mb-recording-search.json"));
        if (path == "/caa/release/" + releaseA || path == "/caa/release-group/" + groupId)
            return json(fixture("caa-release.json"));
        if (path.startsWith("/caa/release"))
            return json("", 404);
        if (path.startsWith("/caa/img/"))
            return MockHttp::Reply{200, png(path.contains("front") ? QColor(200, 30, 30) : QColor(30, 30, 200)), "image/png", {}};
        if (path == "/api/get") {
            if (r.item("track_name") == "Coastline")
                return json(fixture("lrclib-get.json"));
            return json("{\"code\":404,\"name\":\"TrackNotFound\",\"message\":\"Failed to find specified track\"}", 404);
        }
        if (path == "/api/search")
            return json("[]");
        if (path == "/1/validate-token") {
            if (r.headers.value("authorization") == "Token good-token")
                return json(fixture("listenbrainz-validate.json"));
            return json("{\"code\":200,\"message\":\"Token invalid.\",\"valid\":false}");
        }
        if (path == "/2.0/") {
            const QString method = r.method == "POST" ? r.formItem("method") : r.item("method");
            if (method == "auth.getToken")
                return json("{\"token\":\"tok123\"}");
            if (method == "auth.getSession")
                return r.item("token") == "tok123"
                           ? json("{\"session\":{\"name\":\"fmuser\",\"key\":\"sess\",\"subscriber\":0}}")
                           : json("{\"error\":14,\"message\":\"Unauthorized Token - This token has not been authorized\"}", 403);
        }
        return json("{\"error\":\"unexpected request\"}", 404);
    }

    void fastNet() {
        Net::Config::resetForTests();
        for (Net::Service s : {Net::Service::MusicBrainz, Net::Service::AcoustId, Net::Service::CoverArt,
                               Net::Service::Lrclib, Net::Service::ListenBrainz, Net::Service::LastFm})
            Net::Config::setIntervalMs(s, 0);
        Net::Config::setBackoffMs(10);
    }

    // A folder with a copy of the three Night Drive files, scanned.
    QString musicCase() {
        const QString root = m_tmp.path() + QString("/case%1").arg(++m_counter);
        QDirIterator it(m_pristine, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString from = it.next();
            const QString to = root + "/music" + from.mid(m_pristine.size());
            QDir().mkpath(QFileInfo(to).absolutePath());
            QFile::copy(from, to);
        }
        Library library(root + "/library.db");
        library.open();
        Scanner(library, VolumeResolver()).scan({root + "/music"});
        return root;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        m_server = std::make_unique<MockHttp>();
        m_server->setHandler([this](const MockHttp::Request &r) { return route(r); });
        setEnv("XDG_CACHE_HOME", (m_tmp.path() + "/xdg-cache").toUtf8());
        setEnv("XDG_DATA_HOME", (m_tmp.path() + "/xdg-data").toUtf8());
        setEnv("XDG_CONFIG_HOME", (m_tmp.path() + "/xdg-config").toUtf8());
        const QByteArray base = m_server->url().toUtf8();
        setEnv("THRUM_MUSICBRAINZ_URL", base + "/ws/2");
        setEnv("THRUM_ACOUSTID_URL", base + "/v2");
        setEnv("THRUM_COVERART_URL", base + "/caa");
        setEnv("THRUM_LRCLIB_URL", base + "/api");
        setEnv("THRUM_LISTENBRAINZ_URL", base + "/1");
        setEnv("THRUM_LASTFM_URL", base + "/2.0");
        for (const char *name : {"THRUM_ACOUSTID_KEY", "THRUM_LISTENBRAINZ_TOKEN", "THRUM_LASTFM_API_KEY",
                                 "THRUM_LASTFM_API_SECRET", "THRUM_OFFLINE"})
            setEnv(name, "");
        fastNet();
        m_trap.install();
        if (!haveFfmpeg())
            return;
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
    }

    void cleanupTestCase() {
        PathProbe::setStatHook({});
        PathProbe::setDefaultTimeoutMs(1500);
        Net::Config::resetForTests();
        RemoteSources::clearSecretCache();
        for (auto it = m_savedEnv.constBegin(); it != m_savedEnv.constEnd(); ++it) {
            if (it.value().isEmpty())
                qunsetenv(it.key().constData());
            else
                qputenv(it.key().constData(), it.value());
        }
    }

    void init() {
        fastNet();
        m_server->setHandler([this](const MockHttp::Request &r) { return route(r); });
        m_server->clear();
    }

    void releaseIdsFromText() {
        QCOMPARE(WebController::releaseIdFrom("https://musicbrainz.org/release/" + releaseA + "?x=1"), releaseA);
        QCOMPARE(WebController::releaseIdFrom("  " + releaseB.toUpper() + " "), releaseB);
        QCOMPARE(WebController::releaseIdFrom("https://musicbrainz.org/artist/" + releaseA), QString());
        QCOMPARE(WebController::releaseIdFrom("Night Drive"), QString());
        QCOMPARE(NasController::normaliseUrl("nas:4533/"), QString("http://nas:4533"));
        QCOMPARE(NasController::normaliseUrl("https://music.example.org"), QString("https://music.example.org"));
    }

    // Ctrl+I on three files: the review shows the right release first, the
    // file-to-track pairing and every tag change; another release can be
    // pasted in; Ctrl+Enter saves (with the cover) and Ctrl+Z puts it back.
    void identifyReviewSaveAndUndo() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the audio fixtures");
        const QString root = musicCase();
        Window w(root);
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        WebController *web = c.web();
        QTRY_VERIFY(c.ready());
        QTRY_COMPARE(c.tracks()->count(), 3);
        c.tracks()->selectAll();
        QMetaObject::invokeMethod(w.find("advancedView"), "focusList");
        QTest::keyClick(w.window, Qt::Key_I, Qt::ControlModifier);
        QObject *sheet = w.find("identifySheet");
        QVERIFY(sheet);
        QTRY_VERIFY(sheet->property("opened").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(web->identifyState(), QString("ready"), 30000);

        // No AcoustID key: said so, with the way to Settings.
        QVERIFY(!web->acoustIdKeySet());
        QVERIFY(w.find("acoustidNote")->property("visible").toBool());
        QVERIFY(!web->identifyOffline());

        QVariantList clusters = web->clusters();
        QCOMPARE(clusters.size(), 1);
        QVariantMap cluster = clusters.first().toMap();
        QCOMPARE(cluster.value("album").toString(), QString("Night Drive"));
        QVariantList candidates = cluster.value("candidates").toList();
        QCOMPARE(candidates.size(), 2);
        QCOMPARE(candidates[0].toMap().value("mbid").toString(), releaseA);
        QCOMPARE(candidates[0].toMap().value("confidence").toString(), QString("high"));
        QVERIFY(candidates[0].toMap().value("score").toInt() > candidates[1].toMap().value("score").toInt());
        QVERIFY(candidates[0].toMap().value("why").toString().contains("search"));
        QVERIFY(cluster.value("included").toBool());
        // Every file paired, each with its own score.
        const QVariantList mapping = cluster.value("mapping").toList();
        QCOMPARE(mapping.size(), 3);
        for (const QVariant &m : mapping) {
            QVERIFY(m.toMap().value("matched").toBool());
            QVERIFY(m.toMap().value("score").toInt() >= 65);
        }
        QCOMPARE(mapping[0].toMap().value("title").toString(), QString("Midnight Highway"));
        // The dry run's changes, old and new; nothing written yet.
        auto hasChange = [](const QVariantMap &cl, const QString &field, const QString &from, const QString &to) {
            for (const QVariant &v : cl.value("changes").toList()) {
                const QVariantMap ch = v.toMap();
                if (ch.value("field") == field && ch.value("oldValue") == from && ch.value("newValue").toString().contains(to))
                    return true;
            }
            return false;
        };
        QVERIFY(hasChange(cluster, "title", "midnight highway", "Midnight Highway"));
        QVERIFY(hasChange(cluster, "mbreleaseid", "", releaseA));
        const QString first = c.library()->absolutePath(mapping[0].toMap().value("trackId").toLongLong());
        QCOMPARE(TagWriter::readFile(first).value("title"), QString("midnight highway"));
        QObject *changes = w.find("changeList");
        QVERIFY(changes);
        QVERIFY(changes->property("count").toInt() > 5);

        // The other candidate, then a release pasted in by its link.
        web->chooseCandidate(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(web->identifyState(), QString("ready"), 10000);
        cluster = web->clusters().first().toMap();
        QCOMPARE(cluster.value("chosen").toInt(), 1);
        QVERIFY(hasChange(cluster, "title", "midnight highway", "Remastered"));
        QVERIFY(!web->useRelease(0, "not a release"));
        QVERIFY(web->useRelease(0, "https://musicbrainz.org/release/" + releaseA));
        QTRY_COMPARE_WITH_TIMEOUT(web->identifyState(), QString("ready"), 10000);
        cluster = web->clusters().first().toMap();
        QCOMPARE(cluster.value("chosen").toInt(), 0);
        candidates = cluster.value("candidates").toList();
        QCOMPARE(candidates[0].toMap().value("mbid").toString(), releaseA);
        QCOMPARE(candidates.size(), 2); // the pasted release replaced its own entry
        QVERIFY(hasChange(cluster, "title", "midnight highway", "Midnight Highway"));

        // Ctrl+Enter saves, with the front cover, and closes the sheet.
        QSignalSpy applied(web, &WebController::identifyApplied);
        QMetaObject::invokeMethod(sheet, "forceActiveFocus");
        QTest::keyClick(w.window, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(seen(applied));
        QCOMPARE(applied.first().at(0).toInt(), 3);
        QCOMPARE(applied.first().at(1).toInt(), 0);
        QTRY_VERIFY(!sheet->property("opened").toBool());
        QMap<QString, QString> tags = TagWriter::readFile(first);
        QCOMPARE(tags.value("title"), QString("Midnight Highway"));
        QCOMPARE(tags.value("mbreleaseid"), releaseA);
        QVERIFY(!tags.value("artwork").isEmpty());
        QCOMPARE(sql(*c.library(), "SELECT title FROM tracks WHERE rel_path LIKE '%midnight highway.flac'").toString(),
                 QString("Midnight Highway"));
        QTRY_VERIFY(c.undoText().contains("MusicBrainz"));

        // Ctrl+Z puts every tag back.
        QMetaObject::invokeMethod(w.find("advancedView"), "focusList");
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(TagWriter::readFile(first).value("title"), QString("midnight highway"), 15000);
        QTRY_COMPARE(TagWriter::readFile(first).value("mbreleaseid"), QString());
        QTRY_COMPARE(sql(*c.library(), "SELECT title FROM tracks WHERE rel_path LIKE '%midnight highway.flac'").toString(),
                     QString("midnight highway"));
        w.window->close();
    }

    // MusicBrainz unreachable: said plainly, nothing offered.
    void identifyOffline() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the audio fixtures");
        const QString root = musicCase();
        LibraryController c(root + "/library.db");
        c.setWatching(false);
        c.setWriteSandbox(root);
        QVERIFY(c.activate());
        // A port nobody listens on: refused, so offline at once.
        qputenv("THRUM_MUSICBRAINZ_URL", "http://127.0.0.1:9/ws/2");
        fastNet();
        QVariantList ids;
        QSqlQuery q(c.library()->db());
        q.exec("SELECT id FROM tracks");
        while (q.next())
            ids << q.value(0);
        QVERIFY(c.web()->identify(ids));
        QTRY_COMPARE_WITH_TIMEOUT(c.web()->identifyState(), QString("ready"), 20000);
        QVERIFY(c.web()->identifyOffline());
        const QVariantMap cluster = c.web()->clusters().value(0).toMap();
        QVERIFY(cluster.value("candidates").toList().isEmpty());
        QVERIFY2(cluster.value("error").toString().contains("Offline"), qPrintable(cluster.value("error").toString()));
        QVERIFY(!c.web()->applyIdentify({}));
        qputenv("THRUM_MUSICBRAINZ_URL", (m_server->url() + "/ws/2").toUtf8());
        fastNet();
    }

    // Synced lyrics from a .lrc beside the file light the line for the
    // position; a track without any fetches from LRCLIB, saves as a .lrc,
    // and Ctrl+Z takes the file away again.
    void lyricsFollowThePositionAndSave() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the audio fixtures");
        const QString root = musicCase();
        const QString album = root + "/music/Neon Coast/Night Drive";
        write(album + "/01 midnight highway.lrc",
              "[ti:Midnight Highway]\n[00:01.00]First line\n[00:05.00]Second line\n[00:09.50]Third line\n");
        Window w(root);
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        WebController *web = c.web();
        QTRY_COMPARE(c.tracks()->count(), 3);
        const qint64 highway = sql(*c.library(), "SELECT id FROM tracks WHERE rel_path LIKE '%midnight highway.flac'").toLongLong();
        const qint64 coastline = sql(*c.library(), "SELECT id FROM tracks WHERE rel_path LIKE '%Coastline.mp3'").toLongLong();
        c.tracks()->setCurrentRow(c.tracks()->rowOfTrack(highway));

        // The info panel's Lyrics tab, from the palette.
        QVERIFY(w.palette("show lyrics", "lyrics"));
        QObject *view = w.find("lyricsView");
        QVERIFY(view);
        QTRY_VERIFY(view->property("visible").toBool());
        QTRY_COMPARE(web->lyricsTrack(), highway);
        QTRY_COMPARE(web->lyricsState(), QString("found"));
        QVERIFY(web->lyricsSynced());
        QCOMPARE(web->lyricsSource(), QString("sidecar"));
        QCOMPARE(web->lyricsLines().size(), 3);
        QCOMPARE(web->lyricsLineAt(500), -1);
        QCOMPARE(web->lyricsLineAt(1000), 0);
        QCOMPARE(web->lyricsLineAt(6000), 1);
        QCOMPARE(web->lyricsLineAt(60000), 2);
        // Not playing: nothing lit. Playing at 6 s: the second line.
        QCOMPARE(view->property("currentLine").toInt(), -1);
        view->setProperty("follow", true);
        view->setProperty("position", 6000);
        QCOMPARE(view->property("currentLine").toInt(), 1);
        view->setProperty("position", 9600);
        QCOMPARE(view->property("currentLine").toInt(), 2);
        view->setProperty("position", 200);
        QCOMPARE(view->property("currentLine").toInt(), -1);
        view->setProperty("follow", false);

        // Coastline has none until LRCLIB is asked.
        c.tracks()->setCurrentRow(c.tracks()->rowOfTrack(coastline));
        QTRY_COMPARE(web->lyricsTrack(), coastline);
        QTRY_COMPARE(web->lyricsState(), QString("none"));
        QCOMPARE(m_server->count(), 0); // nothing asked without the button
        QObject *fetch = w.find("fetchLyrics");
        QVERIFY(fetch && fetch->property("visible").toBool());
        QMetaObject::invokeMethod(fetch, "clicked");
        QTRY_COMPARE(web->lyricsState(), QString("found"));
        QCOMPARE(web->lyricsSource(), QString("lrclib"));
        QVERIFY(web->lyricsSynced());
        QCOMPARE(web->lyricsLineAt(3500), 1);

        // Save as .lrc (journalled), then Ctrl+Z.
        QSignalSpy finished(&c, &LibraryController::jobFinished);
        QVERIFY(web->saveLyrics("lrc"));
        QVERIFY(seen(finished));
        const QString lrc = album + "/02 Coastline.lrc";
        QTRY_VERIFY(QFileInfo::exists(lrc));
        QVERIFY(readAll(lrc).contains("Lights on the bay"));
        QTRY_COMPARE(web->lyricsSource(), QString("sidecar"));
        QMetaObject::invokeMethod(w.find("advancedView"), "focusList");
        QTest::keyClick(w.window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(lrc), 15000);
        w.window->close();
    }

    // A share that hangs: opening the library and the Volumes page never
    // wait on it, it shows as not answering, its tracks say why they can't
    // play, and when it answers again it is rescanned.
    void volumesShowAHungShareWithoutBlocking() {
        QTemporaryDir dir;
        FakeMounts mounts;
        const QString share = QFileInfo(dir.path()).canonicalFilePath() + "/share";
        for (int i = 1; i <= 3; ++i)
            write(QString("%1/Artist/Album/%2 Song.mp3").arg(share).arg(i, 2, 10, QChar('0')), QByteArray(1000 + i, 'x'));
        mounts.setMounts({{share, "/music", "cifs", "//nas/music"}});
        const QString db = dir.filePath("library.db");
        {
            Library library(db);
            QVERIFY(library.open());
            QVERIFY(Scanner(library, mounts.resolver()).scan({share}).ok);
            QCOMPARE(sql(library, "SELECT kind FROM volumes").toString(), QString("network"));
        }
        PathProbe::setDefaultTimeoutMs(300);
        m_trap.prefix = QFile::encodeName(share);
        m_trap.open = false;
        m_trap.hang = true;

        Window w(dir.path());
        QElapsedTimer opening;
        QVERIFY(w.load(
            [&](LibraryController &c) {
                c.setMountMonitoring(true);
                c.nas()->setResolver(mounts.resolver());
                c.nas()->setPollInterval(600000);
            },
            false));
        LibraryController &c = *w.library;
        NasController *nas = c.nas();
        // The window stays responsive while the probe waits: a 10 ms timer
        // never misses much.
        QElapsedTimer gap;
        gap.start();
        qint64 worst = 0;
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, [&] {
            worst = qMax(worst, gap.restart());
        });
        tick.start(10);
        opening.start();
        QSignalSpy checked(nas, &NasController::volumesChecked);
        QVERIFY(c.activate());
        QVERIFY2(opening.elapsed() < 250, qPrintable(QString::number(opening.elapsed())));
        QVERIFY(seen(checked, 5000));
        tick.stop();
        QVERIFY2(worst < 200, qPrintable(QString("the window stalled for %1 ms").arg(worst)));

        QCOMPARE(nas->volumes().size(), 1);
        QVariantMap v = nas->volumes().first().toMap();
        QCOMPARE(v.value("kind").toString(), QString("network"));
        QCOMPARE(v.value("kindText").toString(), QString("SMB share"));
        QVERIFY(!v.value("online").toBool());
        QCOMPARE(v.value("state").toString(), QString("timed-out"));
        QVERIFY(v.value("stateText").toString().contains("Not answering"));
        QCOMPARE(v.value("tracks").toInt(), 3);
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM tracks WHERE availability = 'offline'").toInt(), 3);
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM tracks WHERE availability = 'missing'").toInt(), 0);
        // The list greys them and says why.
        QTRY_COMPARE(c.tracks()->count(), 3);
        QTRY_VERIFY(!c.tracks()->data(c.tracks()->index(0, 1), TrackListModel::AvailableRole).toBool());
        const QString note = c.tracks()->data(c.tracks()->index(0, 1), TrackListModel::OfflineNoteRole).toString();
        QVERIFY2(note.startsWith("On ") && note.endsWith("which isn't connected"), qPrintable(note));

        // The Volumes page, from the palette, shows the same.
        QVERIFY(w.palette("volumes", "volumes"));
        QObject *sheet = w.find("volumesSheet");
        QTRY_VERIFY(sheet->property("opened").toBool());
        QObject *list = w.find("volumeList");
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QObject *state = w.find("volumeState");
        QVERIFY(state);
        QVERIFY(state->property("text").toString().contains("Not answering"));
        // Checking again while it is still hung costs nothing.
        checked.clear();
        QElapsedTimer again;
        again.start();
        nas->checkNow();
        QVERIFY(again.elapsed() < 100);
        QVERIFY(seen(checked, 5000));
        QCOMPARE(nas->volumes().first().toMap().value("state").toString(), QString("still-hung"));

        // It answers again: back online, rescanned, the note in the jobs bar
        // shown and then cleared.
        m_trap.release();
        QTRY_COMPARE(PathProbe::stuckThreads(), 0);
        QSignalSpy jobs(&c, &LibraryController::jobFinished);
        checked.clear();
        nas->checkNow();
        QVERIFY(seen(checked, 5000));
        QVERIFY(nas->volumes().first().toMap().value("online").toBool());
        QVERIFY(seen(jobs, 10000));
        QCOMPARE(jobs.first().at(0).toString(), QString("scan"));
        QTRY_COMPARE(nas->shareNote(), QString());
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM tracks WHERE availability = 'online'").toInt(), 3);
        QTRY_VERIFY(c.tracks()->data(c.tracks()->index(0, 1), TrackListModel::AvailableRole).toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!sheet->property("opened").toBool());
        w.window->close();
        PathProbe::setDefaultTimeoutMs(1500);
    }

    // The Connect a NAS helper writes out units and commands and runs nothing.
    void mountGuideFromTheForm() {
        QTemporaryDir dir;
        LibraryController c(dir.filePath("library.db"));
        NasController *nas = c.nas();
        const QVariantMap smb = nas->mountGuide("smb", "//nas/music", "/mnt/music");
        QVERIFY2(smb.value("ok").toBool(), qPrintable(smb.value("error").toString()));
        const QVariantList units = smb.value("units").toList();
        QCOMPARE(units.size(), 2);
        QCOMPARE(units[0].toMap().value("name").toString(), QString("mnt-music.mount"));
        QVERIFY(units[0].toMap().value("text").toString().contains("Type=cifs"));
        QCOMPARE(units[1].toMap().value("name").toString(), QString("mnt-music.automount"));
        const QStringList commands = smb.value("commands").toStringList();
        QVERIFY(commands.join('\n').contains("sudoedit /etc/samba/credentials/nas-music"));
        QVERIFY(smb.value("fstab").toString().contains("x-systemd.automount"));
        QVERIFY(smb.value("text").toString().contains("Thrum never runs sudo"));
        const QVariantMap nfs = nas->mountGuide("nfs", "nas:/volume1/music", "/mnt/music");
        QVERIFY(nfs.value("ok").toBool());
        QVERIFY(nfs.value("units").toList()[0].toMap().value("text").toString().contains("vers=4.1"));
        // Mistakes, in the form's own words.
        QVERIFY(nas->mountGuide("smb", "nas/music", "/mnt/music").value("error").toString().contains("//nas/music"));
        QVERIFY(nas->mountGuide("nfs", "nas", "/mnt/music").value("error").toString().contains("colon"));
        QVERIFY(nas->mountGuide("smb", "//nas/music", "mnt").value("error").toString().contains("slash"));
        // The full guide is compiled in.
        QVERIFY(nas->nasGuideText().contains("SMB"));
    }

    // A Navidrome server added from its sheet: tested, its secret in the
    // 0600 file and not the library, synced into tracks with a cloud mark
    // and the server's covers; then synced again and removed.
    void addARemoteSourceAndSync() {
        QTemporaryDir dir;
        MockSubsonic server;
        server.realPictures = true;
        Window w(dir.path());
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        NasController *nas = c.nas();
        QTRY_VERIFY(c.ready());

        // The test button: a wrong password, then the right one.
        QSignalSpy tested(nas, &NasController::tested);
        nas->testServer(server.url().toString(), "listener", "wrong", false);
        QVERIFY(seen(tested, 10000));
        QVERIFY(!tested.first().at(0).toBool());
        QVERIFY2(tested.first().at(1).toString().contains("Wrong username or password"), qPrintable(tested.first().at(1).toString()));
        tested.clear();
        nas->testServer(server.url().toString(), "listener", "sesame", false);
        QVERIFY(seen(tested, 10000));
        QVERIFY(tested.first().at(0).toBool());
        QVERIFY2(tested.first().at(1).toString().contains("Navidrome 0.53.3"), qPrintable(tested.first().at(1).toString()));

        // From the palette: the sheet, the fields, Add and sync.
        QVERIFY(w.palette("add navidrome", "add-server"));
        QObject *sheet = w.find("remoteSourceSheet");
        QTRY_VERIFY(sheet->property("opened").toBool());
        w.find("serverAddress")->setProperty("text", server.url().toString());
        w.find("serverUser")->setProperty("text", "listener");
        w.find("serverSecret")->setProperty("text", "sesame");
        QObject *add = w.find("addServerButton");
        QTRY_VERIFY(add->property("enabled").toBool());
        QMetaObject::invokeMethod(add, "clicked");
        QTRY_VERIFY_WITH_TIMEOUT(!sheet->property("opened").toBool(), 15000);
        QCOMPARE(nas->servers().size(), 1);
        const QVariantMap s = nas->servers().first().toMap();
        QCOMPARE(s.value("tracks").toInt(), 4);
        QVERIFY(s.value("server").toString().startsWith("Navidrome"));
        QCOMPARE(s.value("auth").toString(), QString("token"));

        // The secret lives in the private file, never in the library.
        const QString credentials = Secrets::filePath();
        QVERIFY(credentials.startsWith(m_tmp.path()));
        QVERIFY(readAll(credentials).contains("sesame"));
        QCOMPARE(QFile(credentials).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
        QSqlQuery q(c.library()->db());
        QVERIFY(q.exec("SELECT count(*) FROM settings WHERE value LIKE '%sesame%'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        q.finish();

        // The tracks, with a cloud, in the list; a volume of kind subsonic.
        c.selectNode(0);
        QTRY_COMPARE(c.tracks()->count(), 4);
        QVERIFY(c.tracks()->data(c.tracks()->index(0, 1), TrackListModel::RemoteRole).toBool());
        bool listed = false;
        for (const QVariant &v : nas->volumes())
            listed = listed || (v.toMap().value("kind") == "subsonic" && v.toMap().value("tracks").toInt() == 4);
        QVERIFY(listed);

        // The server's cover, through the window's picture provider.
        const qint64 track = c.tracks()->data(c.tracks()->index(0, 1), TrackListModel::TrackIdRole).toLongLong();
        ArtImageProvider provider(dir.filePath("library.db"), dir.filePath("art"));
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(QString("track/%1/96").arg(track), QSize()));
        QSignalSpy done(response.get(), &QQuickImageResponse::finished);
        QVERIFY(seen(done, 10000));
        QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        QCOMPARE(texture->image().pixelColor(0, 0), QColor(30, 160, 200));
        QVERIFY(server.count("getCoverArt") >= 1);

        // Sync again: nothing new, two requests.
        server.methods.clear();
        QSignalSpy jobs(&c, &LibraryController::jobFinished);
        QVERIFY(nas->syncServer(s.value("id").toLongLong(), false));
        QVERIFY(seen(jobs));
        QCOMPARE(jobs.first().at(1).toJsonObject().value("tracks_added").toInt(), 0);
        // (The list's Art column may fetch covers meanwhile; they aren't the sync's.)
        QCOMPARE(server.methods.size() - server.count("getCoverArt"), 2);

        // A sync that can't reach the server says so on the page.
        server.refuse = true;
        jobs.clear();
        QVERIFY(nas->syncServer(s.value("id").toLongLong(), false));
        QVERIFY(seen(jobs));
        QVERIFY(!nas->servers().first().toMap().value("lastError").toString().isEmpty());
        server.refuse = false;

        // Remove: tracks and secret go.
        jobs.clear();
        QVERIFY(nas->removeServer(s.value("id").toLongLong(), false));
        QVERIFY(seen(jobs));
        QCOMPARE(nas->servers().size(), 0);
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM tracks").toInt(), 0);
        QVERIFY(!readAll(credentials).contains("sesame"));
        w.window->close();
    }

    // Tokens and log-ins survive a restart; nothing goes to a real service.
    void scrobbleSettingsRoundTrip() {
        QTemporaryDir dir;
        const QString db = dir.filePath("library.db");
        QStringList opened;
        {
            LibraryController c(db);
            c.setWatching(false);
            QVERIFY(c.activate());
            SettingsController *s = c.appSettings();
            s->setUrlOpener([&](const QUrl &url) {
                opened << url.toString();
                return true;
            });

            // ListenBrainz: a bad token is refused, a good one saved with the user.
            QSignalSpy validated(s, &SettingsController::validated);
            s->validateListenBrainz("bad-token");
            QVERIFY(seen(validated, 10000));
            QVERIFY(!validated.first().at(0).toBool());
            validated.clear();
            s->validateListenBrainz("good-token");
            QVERIFY(seen(validated, 10000));
            QVERIFY(validated.first().at(0).toBool());
            QCOMPARE(validated.first().at(1).toString(), QString("testlistener"));

            // Last.fm: no key yet; then the key, the page, and the session.
            QSignalSpy lastfm(s, &SettingsController::lastFmResult);
            s->beginLastFm();
            QCOMPARE(lastfm.size(), 1);
            QVERIFY(!lastfm.first().at(1).toBool());
            lastfm.clear();
            QVERIFY(s->setValue("lastfm.api_key", "key123"));
            QVERIFY(s->setValue("lastfm.api_secret", "secret456"));
            QVERIFY(!s->setValue("lastfm.session_key", "forged")); // only through the log-in
            s->beginLastFm();
            QVERIFY(seen(lastfm, 10000));
            QVERIFY2(lastfm.first().at(1).toBool(), qPrintable(lastfm.first().at(2).toString()));
            QCOMPARE(opened.size(), 1);
            QVERIFY(opened.first().contains("token=tok123"));
            QVERIFY(opened.first().contains("api_key=key123"));
            QVERIFY(s->lastFmWaiting());
            lastfm.clear();
            s->finishLastFm();
            QVERIFY(seen(lastfm, 10000));
            QVERIFY2(lastfm.first().at(1).toBool(), qPrintable(lastfm.first().at(2).toString()));
            QVERIFY(!s->lastFmWaiting());

            // Sending the queue now, with nothing in it.
            QSignalSpy flushed(s, &SettingsController::flushed);
            s->flushScrobbles();
            QVERIFY(seen(flushed, 10000));
            QCOMPARE(flushed.first().at(0).toMap().value("sent").toInt(), 0);
        }
        // A restart: both still logged in.
        {
            LibraryController c(db);
            c.setWatching(false);
            QVERIFY(c.activate());
            QMap<QString, QVariantMap> status;
            for (const QVariant &v : c.appSettings()->scrobbleStatus())
                status.insert(v.toMap().value("service").toString(), v.toMap());
            QVERIFY(status["listenbrainz"].value("configured").toBool());
            QCOMPARE(status["listenbrainz"].value("user").toString(), QString("testlistener"));
            QVERIFY(status["lastfm"].value("configured").toBool());
            QCOMPARE(status["lastfm"].value("user").toString(), QString("fmuser"));
            c.appSettings()->forgetLastFm();
            c.appSettings()->forgetListenBrainz();
        }
        {
            LibraryController c(db);
            c.setWatching(false);
            QVERIFY(c.activate());
            for (const QVariant &v : c.appSettings()->scrobbleStatus())
                QVERIFY(!v.toMap().value("configured").toBool());
        }
    }

    // Find missing covers from the Files to Edit node's menu command.
    void findMissingCoversAsAJob() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the audio fixtures");
        const QString root = musicCase();
        // The release ID in every file, so the album's cover is found directly.
        {
            Library library(root + "/library.db");
            QVERIFY(library.open());
            QList<TagEdit> edits;
            QSqlQuery q(library.db());
            q.exec("SELECT id FROM tracks");
            while (q.next())
                edits << TagEdit{q.value(0).toLongLong(), "mbreleaseid", releaseA, {}, {}};
            q.finish();
            QVERIFY(TagWriter(library).apply(edits).ok);
        }
        LibraryController c(root + "/library.db");
        c.setWatching(false);
        c.setWriteSandbox(root);
        c.setArtCacheDir(root + "/art");
        QVERIFY(c.activate());
        c.library()->setSetting("identify.cover_embed", "0");
        QSignalSpy fetched(c.web(), &WebController::coversFetched);
        QVERIFY(c.web()->findMissingCovers());
        QVERIFY(seen(fetched));
        QCOMPARE(fetched.first().at(0).toInt(), 1);
        const qint64 album = sql(*c.library(), "SELECT album_id FROM tracks LIMIT 1").toLongLong();
        QVERIFY(!ArtworkCache(*c.library(), root + "/art").albumArtwork(album, 256).isEmpty());
        // Nothing left to find.
        fetched.clear();
        QVERIFY(c.web()->findMissingCovers());
        QVERIFY(seen(fetched));
        QCOMPARE(fetched.first().at(0).toInt(), 0);
    }
};

int runP45WindowTests(int argc, char **argv) {
    P45WindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "p45window_tests.moc"
