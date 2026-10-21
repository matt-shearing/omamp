// The library window's layout: the Art column, the sidebar and its About
// sections, and the large now playing bar (OMAMP_TESTS=layout).
//
// Offscreen, in software, with no audio device. MusicBrainz, Wikidata and
// Wikipedia are a MockHttp on 127.0.0.1: nothing here reaches the internet.
//
// OMAMP_LAYOUT_BENCH=1 also scrolls a library of 200,000 tracks
// (OMAMP_LAYOUT_BENCH_TRACKS to change it) with the Art column on and
// prints the frame times.
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "about.h"
#include "aboutcontroller.h"
#include "artprovider.h"
#include "artwork.h"
#include "http.h"
#include "layoutprefs.h"
#include "library.h"
#include "librarycontroller.h"
#include "mockhttp.h"
#include "nowplaying_context.h"
#include "player.h"
#include "theme.h"
#include "tracklistmodel.h"
#include "translator.h"

namespace {

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

QByteArray png(int side, QColor colour) {
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(colour);
    QPainter p(&image);
    p.fillRect(side / 4, side / 4, side / 2, side / 2, colour.darker(180));
    p.end();
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return buffer.data();
}

// A made-up library in SQL: `albums` albums of `perAlbum` tracks under
// root/music, each album folder with a cover.png (but every coverEvery-th),
// no audio files (nothing here plays).
void makeLibrary(const QString &dbPath, const QString &root, int albums, int perAlbum, int coverEvery = 1) {
    Library library(dbPath);
    QVERIFY(library.open());
    QSqlDatabase db = library.db();
    db.transaction();
    QSqlQuery q(db);
    QVERIFY(q.exec(QString("INSERT INTO volumes (id, key, kind, root_path, online, added) "
                           "VALUES (1, 'uuid:LAYOUT', 'local', '%1', 1, 0)").arg(root)));
    QSqlQuery album(db), track(db);
    album.prepare("INSERT INTO albums (id, title, album_artist, artist_key, title_key, year) VALUES (?, ?, ?, ?, ?, ?)");
    track.prepare("INSERT INTO tracks (volume_id, rel_path, file_name, format, size, mtime_ns, title, artist, "
                  "album_artist, sort_artist, album, album_id, year, disc_no, track_no, duration_ms, added, bitrate) "
                  "VALUES (1, ?, ?, 'flac', 1000, 0, ?, ?, ?, ?, ?, ?, ?, 1, ?, 200000, 0, 900)");
    const QStringList artists = {"Neon Coast", "Aurora Vale", "Copper Fox", "Harbour Lights", "Iron Orchard",
                                 "Kite Season", "Lumen Drift", "Pale Comet"};
    for (int a = 0; a < albums; ++a) {
        const QString artist = artists[a % artists.size()] + (a >= artists.size() ? QString(" %1").arg(a / artists.size()) : QString());
        const QString title = a == 0 ? QString("Night Drive") : QString("Album %1").arg(a);
        album.addBindValue(a + 1);
        album.addBindValue(title);
        album.addBindValue(artist);
        album.addBindValue(artist.toLower());
        album.addBindValue(title.toLower());
        album.addBindValue(1990 + a % 30);
        QVERIFY(album.exec());
        const QString folder = artist + "/" + title;
        if (coverEvery > 0 && a % coverEvery == 0)
            writeFile(root + "/" + folder + "/cover.png", png(64, QColor::fromHsv((a * 37) % 360, 160, 220)));
        for (int t = 1; t <= perAlbum; ++t) {
            const QString name = QString("%1 Track %2.flac").arg(t, 2, 10, QChar('0')).arg(t);
            track.addBindValue(folder + "/" + name);
            track.addBindValue(name);
            track.addBindValue(QString("Track %1").arg(t));
            track.addBindValue(artist);
            track.addBindValue(artist);
            track.addBindValue(artist);
            track.addBindValue(title);
            track.addBindValue(a + 1);
            track.addBindValue(1990 + a % 30);
            track.addBindValue(t);
            QVERIFY(track.exec());
        }
    }
    QVERIFY(db.commit());
}

// MusicBrainz, Wikidata and Wikipedia, as much of them as the sidebar asks.
MockHttp::Reply aboutHandler(const MockHttp::Request &r, const QString &base) {
    const QByteArray path = r.path;
    if (path == "/ws/2/artist")
        return {200, R"({"artists":[{"id":"aaaaaaaa-0000-4000-8000-000000000001","name":"Neon Coast","score":100}]})"};
    if (path == "/ws/2/artist/aaaaaaaa-0000-4000-8000-000000000001")
        return {200, R"({"relations":[{"type":"wikidata","url":{"resource":"https://www.wikidata.org/wiki/Q4242"}}]})"};
    if (path == "/ws/2/release-group")
        return {200, R"({"release-groups":[{"id":"bbbbbbbb-0000-4000-8000-000000000002","title":"Night Drive","score":100}]})"};
    if (path == "/ws/2/release-group/bbbbbbbb-0000-4000-8000-000000000002")
        return {200, R"({"relations":[{"type":"wikidata","url":{"resource":"https://www.wikidata.org/wiki/Q4343"}}]})"};
    if (path == "/w/api.php") {
        const QString id = r.item("ids");
        const QString title = id == "Q4242" ? "Neon Coast (band)" : id == "Q4343" ? "Night Drive (album)" : "";
        if (title.isEmpty())
            return {200, R"({"entities":{}})"};
        return {200, QString(R"({"entities":{"%1":{"sitelinks":{"enwiki":{"site":"enwiki","title":"%2"}}}}})")
                         .arg(id, title).toUtf8()};
    }
    if (path == "/wp/en/api/rest_v1/page/summary/Neon_Coast_(band)")
        return {200, QString(R"j({"type":"standard","title":"Neon Coast (band)","description":"Made-up synthwave duo",)j"
                             R"j("extract":"Neon Coast is a made-up synthwave duo from a test fixture. Nothing here is real.",)j"
                             R"j("thumbnail":{"source":"%1/img/neon.png","width":64,"height":64},)j"
                             R"j("content_urls":{"desktop":{"page":"https://en.wikipedia.org/wiki/Neon_Coast_(band)"}}})j")
                         .arg(base).toUtf8()};
    if (path == "/wp/en/api/rest_v1/page/summary/Night_Drive_(album)")
        return {200, R"j({"type":"standard","title":"Night Drive (album)","extract":"Night Drive is a made-up album.",)j"
                     R"j("content_urls":{"desktop":{"page":"https://en.wikipedia.org/wiki/Night_Drive_(album)"}}})j"};
    if (path == "/img/neon.png")
        return {200, png(64, Qt::magenta), "image/png"};
    return {404, R"({"error":"not found"})"};
}

} // namespace

class LayoutTests : public QObject {
    Q_OBJECT

    struct Window {
        QTemporaryDir &dir;
        Translator translator;
        Theme theme;
        Player player;
        LibraryController library;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;

        explicit Window(QTemporaryDir &d)
            : dir(d), translator(d.filePath("language")), theme(d.filePath("theme"), nullptr, d.filePath("no-hyprctl")),
              library(d.filePath("library.db")) {
            player.setMuted(true);
            library.setPlayer(&player);
            library.setWatching(false);
            library.setArtCacheDir(d.filePath("art"));
            library.about()->setCacheDir(d.filePath("about"));
            library.about()->setDelay(10);
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("backend", &player);
            engine.rootContext()->setContextProperty("library", &library);
            engine.rootContext()->setContextProperty("commandPalette", library.palette());
            engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(d.filePath("window.conf")));
            installNowPlaying(engine, &theme, &player, d.filePath("library.db"), d.filePath("art"),
                              d.filePath("window.conf"));
            engine.addImageProvider(ArtImageProvider::name,
                                    new ArtImageProvider(d.filePath("library.db"), d.filePath("art")));
        }
        bool load() {
            engine.load(QUrl("qrc:/Main.qml"));
            if (engine.rootObjects().size() != 1)
                return false;
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            return window && QTest::qWaitForWindowExposed(window);
        }
        static void findAll(QQuickItem *item, const QString &name, QList<QQuickItem *> *out) {
            if (item->objectName() == name)
                out->append(item);
            for (QQuickItem *child : item->childItems())
                findAll(child, name, out);
        }
        QList<QQuickItem *> all(const QString &name) const {
            QList<QQuickItem *> out;
            findAll(window->contentItem(), name, &out);
            return out;
        }
        QObject *find(const QString &name) const {
            QObject *o = window->findChild<QObject *>(name);
            if (!o) {
                const auto items = all(name);
                o = items.isEmpty() ? nullptr : items.first();
            }
            return o;
        }
        QVariant saved(const QString &key) const {
            return QSettings(dir.filePath("window.conf"), QSettings::IniFormat).value(key);
        }
        LayoutPrefs *layout() const {
            return engine.rootContext()->contextProperty("appearance").value<QObject *>()->property("layout").value<LayoutPrefs *>();
        }
        qreal columnWidth(int place) const {
            auto *table = window->findChild<QQuickItem *>("trackTable");
            qreal width = 0;
            QMetaObject::invokeMethod(table, "columnWidth", Q_RETURN_ARG(qreal, width), Q_ARG(int, place));
            return width;
        }
        // Thumbnails in the Art column that have a picture.
        int readyThumbs() const {
            int ready = 0;
            for (QQuickItem *i : all("artThumb"))
                if (i->isVisible() && i->property("status").toInt() == 1 && !i->property("source").toString().isEmpty())
                    ++ready;
            return ready;
        }
    };

    static void startInAdvancedMode(QTemporaryDir &dir, const QMap<QString, QVariant> &more = {}) {
        QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
        conf.setValue("window/advanced", true);
        conf.setValue("window/libraryHintDone", true);
        for (auto it = more.cbegin(); it != more.cend(); ++it)
            conf.setValue(it.key(), it.value());
    }

    std::unique_ptr<MockHttp> m_server;
    QTemporaryDir m_cache;
    QByteArray m_oldCache;

    void setEnv(const char *name, const QString &value) { qputenv(name, value.toUtf8()); }

private slots:
    void initTestCase() {
        m_server = std::make_unique<MockHttp>();
        const QString base = m_server->url();
        m_server->setHandler([base](const MockHttp::Request &r) { return aboutHandler(r, base); });
        setEnv("OMAMP_MUSICBRAINZ_URL", base + "/ws/2");
        setEnv("OMAMP_WIKIDATA_URL", base);
        setEnv("OMAMP_WIKIPEDIA_URL", base + "/wp/{lang}");
        m_oldCache = qgetenv("XDG_CACHE_HOME");
        setEnv("XDG_CACHE_HOME", m_cache.path());
        for (Net::Service s : {Net::Service::MusicBrainz, Net::Service::Wikidata, Net::Service::Wikipedia})
            Net::Config::setIntervalMs(s, 0);
    }
    void cleanupTestCase() {
        for (const char *name : {"OMAMP_MUSICBRAINZ_URL", "OMAMP_WIKIDATA_URL", "OMAMP_WIKIPEDIA_URL"})
            qunsetenv(name);
        if (m_oldCache.isEmpty())
            qunsetenv("XDG_CACHE_HOME");
        else
            qputenv("XDG_CACHE_HOME", m_oldCache);
        Net::Config::resetForTests();
        m_server.reset();
    }

    // LayoutPrefs: defaults, every choice kept in window.conf, Reset.
    void layoutChoicesPersist() {
        QTemporaryDir dir;
        const QString conf = dir.filePath("window.conf");
        {
            LayoutPrefs p(conf);
            QVERIFY(p.artColumn());
            QCOMPARE(p.rowPixels(), 24);
            QCOMPARE(p.sections(), LayoutPrefs::allSections());
            QCOMPARE(p.barSize(), QString("compact"));
            QCOMPARE(p.barPosition(), QString("top"));
            p.setArtColumn(false);
            p.setRowHeight("large");
            p.setRowHeight("enormous"); // refused
            p.moveSection("lyrics", -2);
            p.setSectionShown("credits", false);
            p.setCollapsed("details", true);
            p.setFollow("playing");
            p.setBarSize("large");
            p.setBarPosition("bottom");
        }
        LayoutPrefs again(conf);
        QVERIFY(!again.artColumn());
        QCOMPARE(again.rowHeight(), QString("large"));
        QCOMPARE(again.rowPixels(), 44);
        QCOMPARE(again.artPixels(), 40);
        QCOMPARE(again.sections().first(), QString("lyrics"));
        QVERIFY(!again.sectionShown("credits"));
        QVERIFY(again.isCollapsed("details"));
        QCOMPARE(again.follow(), QString("playing"));
        QCOMPARE(again.barSize(), QString("large"));
        QCOMPARE(again.barPosition(), QString("bottom"));
        // An unknown section in the file is dropped; a missing one joins the end.
        {
            QSettings s(conf, QSettings::IniFormat);
            s.setValue("layout/sections", QStringList{"upnext", "nonsense", "cover"});
        }
        LayoutPrefs mixed(conf);
        QCOMPARE(mixed.sections().mid(0, 2), (QStringList{"upnext", "cover"}));
        QCOMPARE(mixed.sections().size(), LayoutPrefs::allSections().size());
        QSignalSpy reset(&mixed, &LayoutPrefs::resetRequested);
        mixed.reset();
        QCOMPARE(reset.count(), 1);
        QVERIFY(mixed.artColumn());
        QCOMPARE(mixed.sections(), LayoutPrefs::allSections());
        QVERIFY(!QSettings(conf, QSettings::IniFormat).contains("layout/barSize"));
    }

    // Notes that come with an album, and the small helpers of the route.
    void packagedNotesAndHelpers() {
        QTemporaryDir dir;
        const QString album = dir.filePath("Neon Coast/Night Drive");
        writeFile(album + "/album.txt", "Recorded in a made-up studio.\r\n");
        writeFile(album + "/Liner Notes.md", "# Liner notes\nThanks to nobody real.");
        writeFile(album + "/release.nfo", "Ripped by \xe9quipe\n"); // Latin-1, as old .nfo files are
        writeFile(album + "/lyrics.txt", "not notes");
        writeFile(album + "/cover.jpg", "\xff\xd8\xff");
        writeFile(album + "/CD1/01 a.flac", "");
        const auto notes = About::packagedNotes(album);
        QCOMPARE(notes.size(), 3);
        QCOMPARE(notes[0].name, QString("album.txt"));
        QCOMPARE(notes[0].text, QString("Recorded in a made-up studio."));
        QCOMPARE(notes[1].name, QString("Liner Notes.md"));
        QCOMPARE(notes[2].text, QString::fromUtf8("Ripped by équipe"));
        // A disc folder also reads the album's folder above it.
        QCOMPARE(About::packagedNotes(album + "/CD1").size(), 3);
        QVERIFY(About::isNoteFile("README"));
        QVERIFY(About::isNoteFile("readme.txt"));
        QVERIFY(!About::isNoteFile("cover.jpg"));
        QVERIFY(!About::isNoteFile("album.pdf"));

        QCOMPARE(About::wikiLanguage("pt_BR"), QString("pt"));
        QCOMPARE(About::wikiLanguage("de-AT.UTF-8"), QString("de"));
        QCOMPARE(About::wikiLanguage(""), QString("en"));
        QCOMPARE(About::wikiLanguage("C"), QString("en"));
        QCOMPARE(About::wikidataIdFromUrl("https://www.wikidata.org/wiki/Q1299"), QString("Q1299"));
        QString wiki;
        const QJsonArray rels = QJsonDocument::fromJson(
            "[{\"type\":\"wikipedia\",\"url\":{\"resource\":\"https://en.wikipedia.org/wiki/X\"}},"
            " {\"type\":\"wikidata\",\"url\":{\"resource\":\"https://www.wikidata.org/wiki/Q7\"}}]").array();
        QCOMPARE(About::wikidataFromRelations(rels, &wiki), QString("Q7"));
        QCOMPARE(wiki, QString("https://en.wikipedia.org/wiki/X"));

        QCOMPARE(ArtworkCache::pictureFormat("jpeg\n"), QByteArray());
        QCOMPARE(ArtworkCache::pictureFormat(png(4, Qt::red)), QByteArray("png"));
    }

    // MusicBrainz → Wikidata → Wikipedia against the mock, then the cache.
    void wikipediaSummaryThroughMusicBrainz() {
        QTemporaryDir dir;
        m_server->clear();
        Net::HttpClient http(nullptr, dir.filePath("http"));
        About::Client client(http, dir.filePath("about"));
        const About::Summary a = client.artist("Neon Coast", {}, "en");
        QVERIFY2(a.found(), qPrintable(client.lastError()));
        QCOMPARE(a.title, QString("Neon Coast (band)"));
        QVERIFY(a.extract.startsWith("Neon Coast is a made-up"));
        QCOMPARE(a.pageUrl, QString("https://en.wikipedia.org/wiki/Neon_Coast_(band)"));
        QCOMPARE(a.wikidataId, QString("Q4242"));
        QVERIFY(a.imageFile.endsWith(".png") && QFileInfo::exists(a.imageFile));
        // Every request said who is asking.
        for (const auto &r : m_server->requests())
            QVERIFY(r.headers.value("user-agent").startsWith("Omamp/"));
        // A German reader with no German article gets the English one.
        const About::Summary b = client.album("Neon Coast", "Night Drive", {}, "de");
        QVERIFY(b.found());
        QCOMPARE(b.lang, QString("en"));
        QCOMPARE(b.mbid, QString("bbbbbbbb-0000-4000-8000-000000000002"));
        // Asked again: all from the cache.
        const int asked = m_server->count();
        QVERIFY(client.artist("Neon Coast", {}, "en").found());
        QCOMPARE(m_server->count(), asked);
        // A name MusicBrainz doesn't know well enough: nothing, quietly.
        m_server->setHandler([](const MockHttp::Request &) { return MockHttp::Reply{200, R"({"artists":[]})"}; });
        QVERIFY(!client.artist("Nobody At All", {}, "en").found());
        const QString base = m_server->url();
        m_server->setHandler([base](const MockHttp::Request &r) { return aboutHandler(r, base); });
    }

    // The window: the Art column first with pictures, the sidebar's
    // sections, notes and Wikipedia, and the large bar at the bottom.
    void artColumnSidebarAndBarInTheWindow() {
        QTemporaryDir dir;
        makeLibrary(dir.filePath("library.db"), dir.filePath("music"), 6, 4, 2);
        writeFile(dir.filePath("music/Neon Coast/Night Drive/album.txt"), "Made up for the layout test.\n");
        startInAdvancedMode(dir, {{"window/columns", R"({"hidden":[false,false,false,false,false,false,false,false,false,true,false],"widths":{"2":150}})"}});
        Window w(dir);
        QVERIFY(w.load());
        LibraryController &library = w.library;
        QTRY_COMPARE(library.tracks()->count(), 24);
        LayoutPrefs *layout = w.layout();
        QVERIFY(layout);

        // Older column settings: Art joins at the start, Artist keeps its width.
        auto *table = w.window->findChild<QQuickItem *>("trackTable");
        QVERIFY(table);
        QObject *pane = table->parent();
        QTRY_COMPARE(pane->property("order").toList().value(0).toInt(), int(TrackListModel::Art));
        QTRY_COMPARE(w.columnWidth(0), qreal(layout->artPixels() + 14));
        QTRY_COMPARE(w.columnWidth(3), qreal(150)); // Art, #, Title, Artist
        // Every other album has a cover; their rows show it.
        QTRY_VERIFY_WITH_TIMEOUT(w.readyThumbs() >= 12, 15000);
        // Track 1 is Neon Coast's first, on album 1.
        const int neon = library.tracks()->rowOfTrack(1);
        QCOMPARE(library.tracks()->data(library.tracks()->index(neon, TrackListModel::Art), TrackListModel::ArtRole)
                     .toString().left(28), QString("image://omamp-art/album/1/96"));

        // Off from the View menu's action, and on again; rows grow taller.
        layout->setArtColumn(false);
        QTRY_VERIFY(w.columnWidth(0) <= 0); // hidden
        layout->setArtColumn(true);
        layout->setRowHeight("large");
        QTRY_COMPARE(w.columnWidth(0), qreal(40 + 14));
        QTRY_COMPARE(pane->property("rowH").toInt(), 44);

        // I: the sidebar for the track under the cursor.
        library.tracks()->setCurrentRow(neon);
        QTest::keyClick(w.window, Qt::Key_I);
        QObject *panel = w.find("infoPanel");
        QTRY_VERIFY(panel && panel->property("visible").toBool());
        QTRY_COMPARE(w.find("infoTitle")->property("text").toString(), QString("Track 1"));
        // Its notes, then Wikipedia for the artist and the album (the mock).
        AboutController *about = library.about();
        QTRY_VERIFY_WITH_TIMEOUT(about->notes().size() == 1, 10000);
        QCOMPARE(about->notes().first().toMap().value("name").toString(), QString("album.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(about->artistState(), QString("found"), 15000);
        QTRY_COMPARE(about->albumState(), QString("found"));
        QTRY_VERIFY(w.find("albumNote_0") && w.find("albumNote_0")->property("text").toString().startsWith("Made up"));
        QTRY_VERIFY(w.find("aboutExtract_artist") &&
                    w.find("aboutExtract_artist")->property("text").toString().startsWith("Neon Coast is"));
        QCOMPARE(about->artist().value("pageUrl").toString(), QString("https://en.wikipedia.org/wiki/Neon_Coast_(band)"));

        // Sections: hide, move and fold, and the choices are kept.
        auto shown = [&](const char *name) { QObject *o = w.find(name); return o && o->property("visible").toBool(); };
        QTRY_VERIFY(shown("sideSection_credits")); // the year, from the tags
        layout->setSectionShown("credits", false);
        QTRY_VERIFY(!shown("sideSection_credits"));
        layout->moveSection("details", -5);
        layout->setCollapsed("lyrics", true);
        QTRY_COMPARE(w.saved("layout/hiddenSections").toStringList(), QStringList{"credits"});
        QCOMPARE(w.saved("layout/sections").toStringList().indexOf("details"), 2);
        QCOMPARE(w.saved("layout/collapsedSections").toStringList(), QStringList{"lyrics"});
        QObject *lyrics = w.find("lyricsView");
        QVERIFY(!lyrics || !lyrics->property("visible").toBool());
        // The palette's Lyrics unfolds them again.
        QMetaObject::invokeMethod(w.window, "runAction", Q_ARG(QVariant, QVariant("lyrics")));
        QTRY_VERIFY(!layout->isCollapsed("lyrics"));
        QTRY_VERIFY(w.find("lyricsView") && w.find("lyricsView")->property("visible").toBool());

        // The large bar, at the bottom.
        QVERIFY(!w.find("bigBar"));
        layout->setBarSize("large");
        layout->setBarPosition("bottom");
        QTRY_VERIFY(w.find("bigBar"));
        QObject *bottom = w.find("bottomBarSlot");
        QVERIFY(bottom && bottom->property("active").toBool());
        QVERIFY(!w.find("topBarSlot")->property("active").toBool());
        // (The bar is made again in its new place, so it is looked up each time.)
        auto barTop = [&] {
            auto *bar = qobject_cast<QQuickItem *>(w.find("bigBar"));
            return bar ? bar->mapToScene(QPointF(0, 0)).y() : -1.0;
        };
        QTRY_VERIFY(barTop() > w.window->height() / 2);
        QTRY_COMPARE(w.saved("layout/barSize").toString(), QString("large"));
        QCOMPARE(w.saved("layout/barPosition").toString(), QString("bottom"));
        // Its sidebar switch hides the sidebar.
        QObject *sideButton = w.find("bigBarSidebar");
        QVERIFY(sideButton);
        QMetaObject::invokeMethod(sideButton, "clicked");
        QTRY_VERIFY(!panel->property("visible").toBool());
        QVERIFY(!layout->sidebarShown());

        // Reset layout: everything as it starts, the sidebar on.
        layout->reset();
        QTRY_VERIFY(layout->sidebarShown());
        QTRY_VERIFY(w.find("deckStrip"));
        QVERIFY(w.find("topBarSlot")->property("active").toBool());
        QCOMPARE(pane->property("rowH").toInt(), 24);
        QCOMPARE(pane->property("order").toList().value(0).toInt(), int(TrackListModel::Art));

        // The window's columns are saved with their order.
        w.window->close();
        QTRY_VERIFY(w.saved("window/columns").toString().contains("\"order\":[11,0,1"));
    }

    // A drag on a header moves its column, and the order is saved.
    void columnsMoveAndKeepTheirOrder() {
        QTemporaryDir dir;
        makeLibrary(dir.filePath("library.db"), dir.filePath("music"), 2, 3, 0);
        startInAdvancedMode(dir);
        {
            Window w(dir);
            QVERIFY(w.load());
            QTRY_COMPARE(w.library.tracks()->count(), 6);
            auto *table = w.window->findChild<QQuickItem *>("trackTable");
            QObject *pane = table->parent();
            QTRY_COMPARE(pane->property("order").toList().value(0).toInt(), 11);
            // Album (3) to the front, as a header drag does.
            QVERIFY(QMetaObject::invokeMethod(table, "moveColumn", Q_ARG(int, 4), Q_ARG(int, 0)));
            QTRY_COMPARE(pane->property("order").toList().value(0).toInt(), 3);
            QCOMPARE(pane->property("order").toList().value(1).toInt(), 11);
            w.window->close();
            QTRY_VERIFY(w.saved("window/columns").toString().contains("\"order\":[3,11,0"));
        }
        Window again(dir);
        QVERIFY(again.load());
        QTRY_COMPARE(again.library.tracks()->count(), 6);
        QObject *pane = again.window->findChild<QQuickItem *>("trackTable")->parent();
        QTRY_COMPARE(pane->property("order").toList().value(0).toInt(), 3);
        // The Album column is at the front on screen too.
        QTRY_COMPARE(again.columnWidth(0), qreal(180));
    }

    // Opt-in: scroll 200,000 tracks with the Art column and time the frames.
    void artColumnAt200k() {
        if (qEnvironmentVariableIntValue("OMAMP_LAYOUT_BENCH") <= 0)
            QSKIP("set OMAMP_LAYOUT_BENCH=1 to measure");
        const int tracks = qEnvironmentVariableIntValue("OMAMP_LAYOUT_BENCH_TRACKS") > 0
                               ? qEnvironmentVariableIntValue("OMAMP_LAYOUT_BENCH_TRACKS") : 200000;
        QTemporaryDir dir;
        QElapsedTimer made;
        made.start();
        // 11 tracks an album, every album with its own cover.
        makeLibrary(dir.filePath("library.db"), dir.filePath("music"), tracks / 11, 11, 1);
        qInfo("made %d tracks in %d albums in %lld ms", tracks / 11 * 11, tracks / 11, made.elapsed());
        // OMAMP_LAYOUT_BENCH_ART=0: the same without the Art column, to compare.
        const bool art = qEnvironmentVariable("OMAMP_LAYOUT_BENCH_ART") != "0";
        startInAdvancedMode(dir, {{"window/advancedWidth", 1400}, {"window/advancedHeight", 860}, {"layout/artColumn", art}});
        Window w(dir);
        QVERIFY(w.load());
        QTRY_VERIFY_WITH_TIMEOUT(w.library.tracks()->count() == tracks / 11 * 11, 30000);
        qInfo("Art column %s", art ? "on" : "off");
        auto *table = w.window->findChild<QQuickItem *>("trackTable");
        QVERIFY(table);
        QList<double> frames;
        QElapsedTimer clock;
        connect(w.window, &QQuickWindow::frameSwapped, this, [&] {
            if (clock.isValid())
                frames << clock.nsecsElapsed() / 1e6;
            clock.restart();
        });
        auto report = [&](const char *what) {
            QList<double> f = frames;
            std::sort(f.begin(), f.end());
            if (f.isEmpty())
                return;
            qInfo("%s: %lld frames, median %.1f ms, 95th %.1f ms, worst %.1f ms", what, qint64(f.size()),
                  f[f.size() / 2], f[int(f.size() * 0.95)], f.last());
        };
        // Steady scrolling: three rows a frame, 400 frames, as a fast wheel.
        auto scroll = [&](int start) {
            frames.clear();
            clock.invalidate();
            for (int i = 0; i < 400; ++i) {
                table->setProperty("contentY", (start + i * 3) * 24.0);
                QTest::qWait(1);
                w.window->update();
                QSignalSpy swapped(w.window, &QQuickWindow::frameSwapped);
                swapped.wait(200);
            }
        };
        const int before = ArtImageProvider::requestCount();
        scroll(1000);
        report("scroll, covers not made yet");
        QElapsedTimer settle;
        settle.start();
        if (art)
            QTRY_VERIFY_WITH_TIMEOUT(w.readyThumbs() >= 20, 30000);
        qInfo("thumbnails on screen %lld ms after the scroll stopped; %d requests, %d dropped before they started",
              settle.elapsed(), ArtImageProvider::requestCount() - before, ArtImageProvider::cancelledCount());
        scroll(1000);
        report("the same scroll again, covers made");
        // Jumps: anywhere in the list, time to the next frame.
        frames.clear();
        QList<double> jumps;
        for (int i = 0; i < 20; ++i) {
            QElapsedTimer t;
            t.start();
            table->setProperty("contentY", QRandomGenerator::global()->bounded(tracks - 40) * 24.0);
            QSignalSpy swapped(w.window, &QQuickWindow::frameSwapped);
            swapped.wait(1000);
            jumps << t.nsecsElapsed() / 1e6;
        }
        std::sort(jumps.begin(), jumps.end());
        qInfo("jump anywhere: median %.1f ms, worst %.1f ms", jumps[jumps.size() / 2], jumps.last());
    }
};

int runLayoutTests(int argc, char **argv) {
    LayoutTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "layout_tests.moc"
