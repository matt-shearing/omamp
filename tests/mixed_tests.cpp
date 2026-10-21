// Mixed playlists and choosing several Spotify rows at once.
//
// A Thrum playlist can hold Spotify tracks beside local files: storing,
// moving and removing them; playing a mixed playlist, with Spotify's tracks
// handed to the (fake) Spotify device and back; leaving them out, once
// said, while Spotify is off; M3U files and the command line. In the real
// window, offscreen: Shift, Ctrl and Ctrl+A in a Spotify list by keyboard
// and mouse, adding a range to a Thrum playlist from the menu, and
// dragging Spotify tracks onto a playlist in the tree.
//
// Everything runs against the fake spotify_player (tests/fake-spotify) in
// temporary folders. Nothing contacts Spotify, starts the real
// spotify_player or session helper, or reaches a sound card.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
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
#include "fixture_library.h"
#include "handoffengine.h"
#include "library.h"
#include "librarycontroller.h"
#include "librarytree.h"
#include "m3u.h"
#include "nativeengine.h"
#include "nowplaying_context.h"
#include "pcmsink.h"
#include "player.h"
#include "playlistitemsmodel.h"
#include "playlistscontroller.h"
#include "playlists.h"
#include "playrecorder.h"
#include "spotifycontroller.h"
#include "theme.h"
#include "translator.h"

#include "spotify_fakes.h"

namespace {

qint64 trackNamed(Library &library, const QString &title) {
    QSqlQuery q(library.db());
    q.prepare("SELECT id FROM tracks WHERE title = ?");
    q.addBindValue(title);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

// A Spotify entry as the Spotify views make them.
Playlists::Entry spotifyEntry(const QString &id, const QString &title, const QString &artist, qint64 ms,
                              const QString &album = {}, const QString &albumId = {}) {
    Playlists::Entry e;
    e.path = "spotify://track/" + id;
    e.title = title;
    e.artist = artist;
    e.durationMs = ms;
    e.album = album;
    e.art = albumId;
    return e;
}

int cli(const QStringList &args, QJsonObject *json, QString *text = nullptr, QString *errors = nullptr) {
    QString o, e;
    QTextStream out(&o), err(&e);
    const int code = Cli::run(args, out, err, nullptr);
    out.flush();
    err.flush();
    if (json)
        *json = QJsonDocument::fromJson(o.toUtf8()).object();
    if (text)
        *text = o;
    if (errors)
        *errors = e;
    return code;
}

// The made-up library, open, with every track online (nothing is ever
// read from their paths).
bool fillLibrary(const QString &path) {
    Library library(path);
    if (!library.open() || !Fixture::fill(library))
        return false;
    QSqlQuery q(library.db());
    q.exec("UPDATE volumes SET online = 1");
    q.exec("UPDATE tracks SET availability = 'online'");
    return true;
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

// Main.qml over the made-up library with Spotify on, through the fake.
struct Window {
    FakeSpotify fake;
    QTemporaryDir dir;
    Translator translator;
    Theme theme;
    std::unique_ptr<Player> player;
    HandoffEngine *handoff = nullptr;
    std::unique_ptr<LibraryController> library;
    SpotifyController *spotify = nullptr;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    Window() : translator(dir.filePath("language")), theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl")) {}
    bool load() {
        if (!fillLibrary(dir.filePath("library.db")))
            return false;
        {
            QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
            conf.setValue("window/advanced", true);
        }
        auto *sink = new BufferSink(48000, false);
        sink->setAutoPull(20);
        // Spotify's tracks go to the fake's device, as in the app.
        handoff = new HandoffEngine(new NativeEngine(sink));
        player = std::make_unique<Player>(handoff, QString());
        library = std::make_unique<LibraryController>(dir.filePath("library.db"));
        library->setPlayer(player.get());
        library->setWatching(false);
        library->setArtCacheDir(dir.filePath("art"));
        library->recorder()->setFlushing(false);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", player.get());
        engine.rootContext()->setContextProperty("library", library.get());
        engine.rootContext()->setContextProperty("commandPalette", library->palette());
        engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(dir.filePath("window.conf")));
        installNowPlaying(engine, &theme, player.get(), dir.filePath("library.db"), dir.filePath("art"));
        spotify = engine.rootContext()->contextProperty("spotify").value<SpotifyController *>();
        // As main() wires them.
        QObject::connect(spotify, &SpotifyController::settingsChanged, library.get(), [this] {
            library->tree()->setExtraNode("spotify", "Spotify", spotify->enabled());
        });
        library->playlists()->setSpotify(spotify);
        spotify->setPlayer(player.get());
        handoff->setExternal(spotify->playback());
        engine.addImageProvider(ArtImageProvider::name,
                                new ArtImageProvider(dir.filePath("library.db"), dir.filePath("art")));
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().size() != 1)
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        return window && QTest::qWaitForWindowExposed(window);
    }
    QObject *find(const QString &name) const {
        if (QObject *o = window->findChild<QObject *>(name))
            return o;
        QQuickItem *top = window->contentItem()->parentItem() ? window->contentItem()->parentItem() : window->contentItem();
        return findItem(top, name);
    }
    // Spotify on, the Spotify node open on Liked Songs.
    bool openLiked() {
        spotify->setEnabled(true);
        if (!QTest::qWaitFor([&] { return spotify->running() && !spotify->checking(); }, 8000))
            return false;
        const int row = library->tree()->rowOfKey("spotify");
        if (row < 0)
            return false;
        library->selectNode(row);
        spotify->openSection("liked");
        return QTest::qWaitFor([&] { return spotify->items()->count() == 8 && !spotify->loading(); }, 5000);
    }
    QQuickItem *list() const { return qobject_cast<QQuickItem *>(find("spotifyList")); }
    // The middle of a row of the Spotify list, in the window.
    QPoint rowPoint(int row) const {
        QQuickItem *l = list();
        return l->mapToScene(QPointF(160, row * 52 + 26 - l->property("contentY").toReal())).toPoint();
    }
    void click(int row, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QTest::mouseClick(window, Qt::LeftButton, mods, rowPoint(row));
        QTest::qWait(20);
    }
    // A menu's entry by its text.
    static QObject *entry(QObject *menu, const QString &text) {
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, i));
            if (item && item->property("text").toString() == text)
                return item;
        }
        return nullptr;
    }
};

} // namespace

class MixedPlaylistTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }

    // --- The library ----------------------------------------------------------

    void spotifyLinksAreRecognised() {
        const QString id = "EXPa1bLHXfZAYenzuniuLN";
        const QString path = "spotify://track/" + id;
        QCOMPARE(Playlists::spotifyTrackPath("spotify:track:" + id), path);
        QCOMPARE(Playlists::spotifyTrackPath("https://open.spotify.com/track/" + id + "?si=abc123"), path);
        QCOMPARE(Playlists::spotifyTrackPath("https://open.spotify.com/intl-de/track/" + id), path);
        QCOMPARE(Playlists::spotifyTrackPath(path), path);
        QCOMPARE(Playlists::spotifyTrackPath("spotify:album:" + id), QString());
        QCOMPARE(Playlists::spotifyTrackPath("/music/a.flac"), QString());
        QCOMPARE(Playlists::spotifyWebUrl(path), "https://open.spotify.com/track/" + id);
    }

    void storesMovesAndRemovesSpotifyEntries() {
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QCOMPARE(library.schemaVersion(), 11);
        const qint64 id = Playlists::create(library, "Mixed");
        const qint64 coastline = trackNamed(library, "Coastline");
        const qint64 heatwave = trackNamed(library, "Heatwave");
        QCOMPARE(Playlists::addTracks(library, id, {coastline}), 1);
        QCOMPARE(Playlists::addEntries(library, id,
                                       {spotifyEntry("EXPa1bLHXfZAYenzuniuLN", "Pixel Rain", "Midnight Arcade", 219000,
                                                     "Pixel Rain", "V4vZIOhGZLTOOqQO8Z6HkD"),
                                        spotifyEntry("KALf6OOQFn1GoieDdz9Yha", "Velvet Signal", "Velvet Static", 201000)}),
                 2);
        QCOMPARE(Playlists::addTracks(library, id, {heatwave}), 1);
        // The same Spotify track again: a repeat, as a local one would be.
        QCOMPARE(Playlists::addEntries(library, id,
                                       {spotifyEntry("EXPa1bLHXfZAYenzuniuLN", "Pixel Rain", "Midnight Arcade", 219000)}),
                 1);

        QList<Playlists::Item> items = Playlists::items(library, id);
        QCOMPARE(items.size(), 5);
        QCOMPARE(items[0].title, QString("Coastline"));
        QCOMPARE(items[1].availability, QString("spotify"));
        QVERIFY(items[1].isSpotify());
        QCOMPARE(items[1].title, QString("Pixel Rain"));
        QCOMPARE(items[1].artist, QString("Midnight Arcade"));
        QCOMPARE(items[1].album, QString("Pixel Rain"));
        QCOMPARE(items[1].art, QString("V4vZIOhGZLTOOqQO8Z6HkD"));
        QCOMPARE(items[1].durationMs, qint64(219000));
        QCOMPARE(items[1].toJson().value("url").toString(),
                 QString("https://open.spotify.com/track/EXPa1bLHXfZAYenzuniuLN"));
        // Only library tracks count for Auto-DJ and syncing.
        QCOMPARE(Playlists::trackIds(library, id), (QList<qint64>{coastline, heatwave}));

        // The playlist view's model counts them apart from missing files.
        PlaylistItemsModel model;
        model.setLibrary(&library);
        model.setPlaylist(id);
        QCOMPARE(model.count(), 5);
        QCOMPARE(model.spotifyCount(), 3);
        QCOMPARE(model.unmatched(), 0);
        QCOMPARE(model.duplicates(), 1);
        QVERIFY(model.data(model.index(1), PlaylistItemsModel::SpotifyRole).toBool());
        QVERIFY(model.data(model.index(4), PlaylistItemsModel::DuplicateRole).toBool());
        QVERIFY(!model.data(model.index(2), PlaylistItemsModel::DuplicateRole).toBool());

        // Alt+Down on the first Spotify entry, then a drag of both Spotify
        // tracks to the top.
        model.select(1);
        QVERIFY(model.moveSelection(1));
        items = Playlists::items(library, id);
        QCOMPARE(items[2].title, QString("Pixel Rain"));
        QCOMPARE(items[1].title, QString("Velvet Signal"));
        model.select(1);
        model.select(2, Qt::ShiftModifier);
        QVERIFY(model.moveSelectionTo(0));
        items = Playlists::items(library, id);
        QCOMPARE(items[0].title, QString("Velvet Signal"));
        QCOMPARE(items[1].title, QString("Pixel Rain"));
        QCOMPARE(items[2].title, QString("Coastline"));
        // Delete takes the Spotify entries out like any other.
        model.select(0);
        model.select(1, Qt::ShiftModifier);
        QCOMPARE(model.removeSelection(), 2);
        items = Playlists::items(library, id);
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[0].title, QString("Coastline"));
        QCOMPARE(model.spotifyCount(), 1);
        QCOMPARE(model.duplicates(), 0);
    }

    // Export writes a Spotify entry as #EXTINF plus its open.spotify.com
    // link; import reads those links, and spotify: URIs, back as Spotify
    // entries.
    void m3uRoundTripKeepsSpotifyEntries() {
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        const qint64 id = Playlists::create(library, "Mixed");
        Playlists::addTracks(library, id, {trackNamed(library, "Coastline")});
        Playlists::addEntries(library, id,
                              {spotifyEntry("EXPa1bLHXfZAYenzuniuLN", "Pixel Rain", "Midnight Arcade", 219000)});
        const QString file = dir.filePath("Mixed.m3u8");
        int written = 0;
        QVERIFY(PlaylistFiles::exportFile(library, id, file, {}, &written));
        QCOMPARE(written, 2);
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY2(text.contains("#EXTINF:219,Midnight Arcade - Pixel Rain\nhttps://open.spotify.com/track/EXPa1bLHXfZAYenzuniuLN\n"),
                 qPrintable(text));

        const PlaylistFiles::ImportResult back = PlaylistFiles::import(library, file);
        QVERIFY(back.ok());
        QCOMPARE(back.entries, 2);
        QCOMPARE(back.byPath, 1);
        QCOMPARE(back.spotify, 1);
        QCOMPARE(back.unmatched, 0);
        const QList<Playlists::Item> items = Playlists::items(library, back.playlistId);
        QCOMPARE(items[0].title, QString("Coastline"));
        QVERIFY(items[1].isSpotify());
        QCOMPARE(items[1].path, QString("spotify://track/EXPa1bLHXfZAYenzuniuLN"));
        QCOMPARE(items[1].title, QString("Pixel Rain"));
        QCOMPARE(items[1].artist, QString("Midnight Arcade"));
        QCOMPARE(items[1].durationMs, qint64(219000));

        // A file written by hand, with spotify: URIs and a shared link.
        QFile hand(dir.filePath("Hand.m3u"));
        QVERIFY(hand.open(QIODevice::WriteOnly));
        hand.write("#EXTM3U\n#EXTINF:201,Velvet Static - Velvet Signal\nspotify:track:KALf6OOQFn1GoieDdz9Yha\n"
                   "https://open.spotify.com/track/7OqU7GsRSBR5QIa6RHgbtA?si=0123abc\n");
        hand.close();
        const PlaylistFiles::ImportResult r = PlaylistFiles::import(library, hand.fileName());
        QCOMPARE(r.spotify, 2);
        const QList<Playlists::Item> handItems = Playlists::items(library, r.playlistId);
        QCOMPARE(handItems[0].path, QString("spotify://track/KALf6OOQFn1GoieDdz9Yha"));
        QCOMPARE(handItems[0].title, QString("Velvet Signal"));
        QCOMPARE(handItems[1].path, QString("spotify://track/7OqU7GsRSBR5QIa6RHgbtA"));
    }

    // `thrum playlist add <playlist> spotify:track:<id>` and `show`.
    void commandLineAddsAndShowsSpotifyTracks() {
        FakeSpotify fake;
        QTemporaryDir dir;
        const QString db = dir.filePath("library.db");
        QVERIFY(fillLibrary(db));
        QJsonObject j;
        QCOMPARE(cli({"playlist", "create", "Mix", "--json", "--db", db}, &j), 0);
        QCOMPARE(cli({"playlist", "add", "Mix", "--ids", "1", "--json", "--db", db}, &j), 0);
        const QString pixel = fake.id("track:Pixel Rain"), velvet = fake.id("track:Velvet Signal");

        // Spotify off in Thrum: the tracks go in by id, and it says so.
        QString text, errors;
        QCOMPARE(cli({"playlist", "add", "Mix", "spotify:track:" + pixel, "--db", db}, nullptr, &text, &errors), 0);
        QVERIFY(text.contains("Added 1 Spotify tracks to Mix"));
        QVERIFY(errors.contains("without their names"));
        QVERIFY(fake.calls().filter("get item").isEmpty());

        // Spotify on, spotify_player running: the names come from it.
        {
            Spotify::Settings s;
            s.enabled = true;
            QVERIFY(s.save());
        }
        Spotify::SpotifyPlayerBackend backend;
        QString error;
        QVERIFY(backend.start(&error));
        QCOMPARE(cli({"playlist", "add", "Mix", "https://open.spotify.com/track/" + velvet + "?si=x", "--at", "1",
                      "--json", "--db", db},
                     &j),
                 0);
        QCOMPARE(j.value("added").toInt(), 1);
        QVERIFY(fake.calls().contains("get item --id " + velvet + " track"));

        QCOMPARE(cli({"playlist", "show", "Mix", "--json", "--db", db}, &j), 0);
        const QJsonArray items = j.value("items").toArray();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[0].toObject().value("title").toString(), QString("Velvet Signal"));
        QCOMPARE(items[0].toObject().value("artist").toString(), QString("Velvet Static"));
        QVERIFY(items[0].toObject().value("spotify").toBool());
        QCOMPARE(items[0].toObject().value("availability").toString(), QString("spotify"));
        QVERIFY(!items[1].toObject().value("spotify").toBool());
        QCOMPARE(items[2].toObject().value("path").toString(), "spotify://track/" + pixel);

        QCOMPARE(cli({"playlist", "show", "Mix", "--db", db}, nullptr, &text), 0);
        QVERIFY2(text.contains("Velvet Static - Velvet Signal") && text.contains("https://open.spotify.com/track/" + velvet)
                     && text.contains("[Spotify]"),
                 qPrintable(text));
        // A query and a link together are refused.
        QCOMPARE(cli({"playlist", "add", "Mix", "genre:pop", "spotify:track:" + pixel, "--db", db}, nullptr, nullptr,
                     &errors),
                 1);
        QVERIFY(errors.contains("not both"));
    }

    // --- Playing a mixed playlist ----------------------------------------------

    // Local, Spotify, local: the local tracks play on Thrum's engine, the
    // Spotify one is handed to the Spotify device and back. The entry's
    // stored name is enough: Thrum hasn't seen it in a Spotify list.
    void aMixedPlaylistHandsOffAndBack() {
        FakeSpotify fake;
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        auto *engine = new FakeEngine;
        SpotifyController spotify(dir.filePath("spotify.conf"), dir.filePath("spotify-art"));
        spotify.playback()->setTimings(100, 4000);
        Player player(new HandoffEngine(engine, spotify.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        spotify.setPlayer(&player);
        LibraryController c(dir.filePath("library.db"));
        c.setWatching(false);
        c.setArtCacheDir(dir.filePath("art"));
        c.setPlayer(&player);
        c.recorder()->setFlushing(false);
        QVERIFY(c.activate());
        // The made-up volume isn't mounted here; call it online so its
        // tracks have paths (the fake engine never opens them).
        {
            QSqlQuery q(c.library()->db());
            q.exec("UPDATE volumes SET online = 1");
            q.exec("UPDATE tracks SET availability = 'online'");
        }
        c.playlists()->setSpotify(&spotify);
        spotify.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(spotify.running() && !spotify.checking(), 8000);
        QCOMPARE(spotify.unavailableReason(), QString());

        Library &library = *c.library();
        const qint64 id = c.playlists()->createPlaylist("Mixed");
        const QString pixel = fake.id("track:Pixel Rain");
        Playlists::addTracks(library, id, {trackNamed(library, "Coastline")});
        Playlists::addEntries(library, id,
                              {spotifyEntry(pixel, "Pixel Rain", "Midnight Arcade", 219000, "Pixel Rain",
                                            fake.id("album:Pixel Rain"))});
        Playlists::addTracks(library, id, {trackNamed(library, "Heatwave")});
        QVERIFY(!spotify.knownTrack(pixel).isValid());

        QCOMPARE(c.playlists()->play(id, 0), 3);
        QCOMPARE(player.playlist()->count(), 3);
        QCOMPARE(player.playlist()->at(1).path, "spotify://track/" + pixel);
        QCOMPARE(player.state(), QString("playing"));
        QCOMPARE(player.currentIndex(), 0);
        QCOMPARE(player.title(), QString("Coastline"));
        // The stored entry told the Spotify side its name and album.
        QCOMPARE(spotify.knownTrack(pixel).name, QString("Pixel Rain"));
        QCOMPARE(spotify.knownTrack(pixel).albumId, fake.id("album:Pixel Rain"));

        fake.clearCalls();
        engine->end();
        QCOMPARE(player.currentIndex(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(fake.calls().contains("playback start track --id " + pixel), 5000);
        QCOMPARE(player.title(), QString("Pixel Rain"));
        QCOMPARE(engine->state(), AudioEngine::Stopped);
        QTRY_VERIFY_WITH_TIMEOUT(!player.buffering(), 5000);

        // Spotify reaches the end: back on Thrum's engine for Heatwave.
        fake.setPlayback(pixel, 219000, false);
        QTRY_COMPARE_WITH_TIMEOUT(player.currentIndex(), 2, 5000);
        QCOMPARE(engine->state(), AudioEngine::Playing);
        QCOMPARE(player.title(), QString("Heatwave"));
        player.stop();
    }

    // Spotify off: its entries are left out of Now Playing, and a notice
    // says so once.
    void spotifyOffSkipsItsEntriesWithANotice() {
        FakeSpotify fake;
        QTemporaryDir dir;
        QVERIFY(fillLibrary(dir.filePath("library.db")));
        auto *engine = new FakeEngine;
        SpotifyController spotify(dir.filePath("spotify.conf"), dir.filePath("spotify-art"));
        Player player(new HandoffEngine(engine, spotify.playback()), QString(),
                      [](const QString &path, qint64, QString *) { return QUrl(path); });
        spotify.setPlayer(&player);
        LibraryController c(dir.filePath("library.db"));
        c.setWatching(false);
        c.setArtCacheDir(dir.filePath("art"));
        c.setPlayer(&player);
        c.recorder()->setFlushing(false);
        QVERIFY(c.activate());
        // The made-up volume isn't mounted here; call it online so its
        // tracks have paths (the fake engine never opens them).
        {
            QSqlQuery q(c.library()->db());
            q.exec("UPDATE volumes SET online = 1");
            q.exec("UPDATE tracks SET availability = 'online'");
        }
        c.playlists()->setSpotify(&spotify);
        QVERIFY(!spotify.enabled());
        QVERIFY(spotify.unavailableReason().startsWith("Spotify is off"));

        Library &library = *c.library();
        const qint64 id = c.playlists()->createPlaylist("Mixed");
        Playlists::addTracks(library, id, {trackNamed(library, "Coastline")});
        Playlists::addEntries(library, id,
                              {spotifyEntry(fake.id("track:Pixel Rain"), "Pixel Rain", "Midnight Arcade", 219000),
                               spotifyEntry(fake.id("track:Velvet Signal"), "Velvet Signal", "Velvet Static", 201000)});
        Playlists::addTracks(library, id, {trackNamed(library, "Heatwave")});

        QSignalSpy toasts(&c, &LibraryController::toast);
        QCOMPARE(c.playlists()->play(id, 0), 2);
        QCOMPARE(player.playlist()->count(), 2);
        QCOMPARE(player.playlist()->at(1).title, QString("Heatwave"));
        QCOMPARE(toasts.count(), 1);
        const QString notice = toasts.first().first().toString();
        QVERIFY2(notice.contains("Spotify is off — turn it on in Settings → Spotify"), qPrintable(notice));
        // Starting from a Spotify row starts at the next track that can play.
        QCOMPARE(c.playlists()->play(id, 1), 2);
        QCOMPARE(player.currentIndex(), 1);
        // Nothing but Spotify: it says why instead of playing nothing.
        const qint64 only = c.playlists()->createPlaylist("Only Spotify");
        Playlists::addEntries(library, only,
                              {spotifyEntry(fake.id("track:Pixel Rain"), "Pixel Rain", "Midnight Arcade", 219000)});
        toasts.clear();
        QCOMPARE(c.playlists()->play(only, 0), 0);
        QCOMPARE(toasts.count(), 1);
        QVERIFY(toasts.first().first().toString().startsWith("Spotify is off"));
        QVERIFY(fake.calls().filter("playback").isEmpty());
        player.stop();
    }

    // --- The window ------------------------------------------------------------

    // Choosing rows in a Spotify list, by keyboard and mouse, as in the library.
    void choosesSeveralSpotifyRows() {
        Window w;
        QVERIFY(w.load());
        QTRY_VERIFY(w.library->ready());
        QVERIFY(w.openLiked());
        SpotifyItemsModel *rows = w.spotify->items();
        QQuickItem *list = w.list();
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible());
        QTest::qWait(100);
        QMetaObject::invokeMethod(w.find("spotifyView"), "focusList");
        QTRY_VERIFY(list->hasActiveFocus());

        // Keys: Down, then Shift+Down twice chooses three rows.
        QTest::keyClick(w.window, Qt::Key_Down);
        QCOMPARE(rows->currentRow(), 1);
        QCOMPARE(rows->selectedCount(), 1);
        QTest::keyClick(w.window, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(w.window, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(rows->selectedCount(), 3);
        QCOMPARE(rows->chosenRows(), (QList<int>{1, 2, 3}));
        QVERIFY(w.find("spotifySelectedCount")->property("visible").toBool());
        QCOMPARE(w.find("spotifySelectedCount")->property("text").toString(), QString("3 selected"));
        // Ctrl+A chooses every row; Esc lets go.
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(rows->selectedCount(), 8);
        QTest::keyClick(w.window, Qt::Key_Escape);
        QCOMPARE(rows->selectedCount(), 0);

        // Mouse: a click, a Shift+click range, a Ctrl+click more and less.
        w.click(0);
        QCOMPARE(rows->chosenRows(), (QList<int>{0}));
        w.click(3, Qt::ShiftModifier);
        QCOMPARE(rows->chosenRows(), (QList<int>{0, 1, 2, 3}));
        w.click(6, Qt::ControlModifier);
        QCOMPARE(rows->chosenRows(), (QList<int>{0, 1, 2, 3, 6}));
        w.click(2, Qt::ControlModifier);
        QCOMPARE(rows->chosenRows(), (QList<int>{0, 1, 3, 6}));
        QCOMPARE(rows->chosenTrackCount(), 4);
        // Shift+click goes from the last Ctrl+click.
        w.click(4, Qt::ShiftModifier);
        QCOMPARE(rows->chosenRows(), (QList<int>{2, 3, 4}));
        QVERIFY(list->hasActiveFocus());

        // Enter with several rows plays just those.
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_COMPARE(w.player->playlist()->count(), 3);
        QCOMPARE(w.player->playlist()->at(0).path, "spotify://track/" + w.spotify->items()->at(2).id);
        w.player->stop();

        // A double-click still plays the list from that track.
        QTest::mouseDClick(w.window, Qt::LeftButton, Qt::NoModifier, w.rowPoint(5));
        QTRY_COMPARE(w.player->playlist()->count(), 8);
        QCOMPARE(w.player->currentIndex(), 5);
        w.player->stop();

        // Copy Spotify links: one a line.
        w.click(0);
        w.click(1, Qt::ShiftModifier);
        const QString links = w.spotify->copyChosenLinks(rows);
        QCOMPARE(links, "https://open.spotify.com/track/" + rows->at(0).id + "\nhttps://open.spotify.com/track/"
                            + rows->at(1).id);
        w.window->close();
    }

    // A range added to a Thrum playlist from Add to playlist ▸, and the
    // menu's two groups.
    void addsARangeToAnThrumPlaylist() {
        Window w;
        QVERIFY(w.load());
        QTRY_VERIFY(w.library->ready());
        const qint64 mix = w.library->playlists()->createPlaylist("Mix");
        QVERIFY(mix > 0);
        w.library->playlists()->addTracks(mix, {QVariant(qint64(1))});
        QVERIFY(w.openLiked());
        SpotifyItemsModel *rows = w.spotify->items();
        QTRY_VERIFY(w.list()->isVisible());
        QTest::qWait(100);
        w.click(1);
        w.click(4, Qt::ShiftModifier);
        QCOMPARE(rows->selectedCount(), 4);

        // The header's button opens the menu under it.
        auto *button = qobject_cast<QQuickItem *>(w.find("spotifyAddToPlaylist"));
        QVERIFY(button && button->isEnabled());
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier,
                          button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        QObject *menu = w.find("spotifyAddMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(Window::entry(menu, "Thrum playlists"));
        QVERIFY(Window::entry(menu, "New Thrum playlist…"));
        QVERIFY(Window::entry(menu, "Spotify playlists"));
        // spotify_player can make playlists, so the Spotify group offers it.
        QObject *newSpotify = Window::entry(menu, "New Spotify playlist…");
        QVERIFY(newSpotify && newSpotify->property("visible").toBool());
        QVERIFY(Window::entry(menu, "Late Night Drive"));
        QObject *mixEntry = Window::entry(menu, "Mix");
        QVERIFY(mixEntry);
        QMetaObject::invokeMethod(mixEntry, "triggered");
        QMetaObject::invokeMethod(menu, "close");

        QList<Playlists::Item> items;
        QTRY_COMPARE((items = Playlists::items(*w.library->library(), mix)).size(), 5);
        QVERIFY(!items[0].isSpotify());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(items[i].isSpotify());
            QCOMPARE(items[i].path, "spotify://track/" + rows->at(i).id);
            QCOMPARE(items[i].title, rows->at(i).name);
            QCOMPARE(items[i].artist, rows->at(i).artistText());
            QCOMPARE(items[i].album, rows->at(i).album);
            QCOMPARE(items[i].art, rows->at(i).albumId);
            QCOMPARE(items[i].durationMs, rows->at(i).durationMs);
        }
        // The tree's count followed.
        const int node = w.library->tree()->reveal({"playlists", QString("playlist:%1").arg(mix)});
        QVERIFY(node >= 0);

        // The same rows into a new Spotify playlist, through the name sheet.
        w.fake.clearCalls();
        QMetaObject::invokeMethod(w.window, "runAction", Q_ARG(QVariant, QVariant("spotify-new-spotify-playlist:view")));
        QObject *sheet = w.find("nameSheet");
        QTRY_VERIFY(sheet && sheet->property("opened").toBool());
        w.find("nameField")->setProperty("text", "Fresh Finds");
        QMetaObject::invokeMethod(w.find("nameConfirm"), "clicked");
        QTRY_VERIFY_WITH_TIMEOUT(w.fake.calls().contains("playlist new -- Fresh Finds"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(w.fake.calls().filter("playlist edit --track-id").size(), 4, 5000);
        bool listed = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     listed = false;
                                     for (const QVariant &p : w.spotify->ownPlaylists())
                                         listed = listed || p.toMap().value("name") == "Fresh Finds";
                                     return listed;
                                 }()),
                                 5000);
        w.window->close();
    }

    // Spotify tracks dragged onto a playlist in the tree, then the playlist
    // view showing them with Spotify's mark, greyed once Spotify is off.
    void dragsSpotifyTracksOntoATreePlaylist() {
        Window w;
        QVERIFY(w.load());
        QTRY_VERIFY(w.library->ready());
        const qint64 id = w.library->playlists()->createPlaylist("Dropped");
        QVERIFY(id > 0);
        QVERIFY(w.openLiked());
        SpotifyItemsModel *rows = w.spotify->items();
        QTRY_VERIFY(w.list()->isVisible());
        QTest::qWait(100);
        w.click(0);
        w.click(2, Qt::ShiftModifier);
        QCOMPARE(rows->selectedCount(), 3);

        const int node = w.library->tree()->reveal({"playlists", QString("playlist:%1").arg(id)});
        QVERIFY(node >= 0);
        auto *tree = qobject_cast<QQuickItem *>(w.find("treeList"));
        QVERIFY(tree);
        QTest::qWait(100);
        const QPoint from = w.rowPoint(1);
        const QPoint to = tree->mapToScene(QPointF(60, node * 24 + 12 - tree->property("contentY").toReal())).toPoint();
        QTest::mousePress(w.window, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 12; ++i) {
            const QPoint at = from + (to - from) * i / 12;
            QMouseEvent move(QEvent::MouseMove, at, w.window->mapToGlobal(at), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QGuiApplication::sendEvent(w.window, &move);
            QTest::qWait(15);
        }
        QVERIFY(w.find("spotifyDragProxy")->property("visible").toBool());
        QTest::mouseRelease(w.window, Qt::LeftButton, Qt::NoModifier, to);
        QList<Playlists::Item> items;
        QTRY_COMPARE((items = Playlists::items(*w.library->library(), id)).size(), 3);
        QCOMPARE(items[0].title, rows->at(0).name);
        QCOMPARE(items[2].title, rows->at(2).name);
        QVERIFY(items[1].isSpotify());

        // The playlist view: three Spotify rows with the mark.
        w.library->selectNode(w.library->tree()->reveal({"playlists", QString("playlist:%1").arg(id)}));
        QTRY_COMPARE(w.library->playlists()->items()->count(), 3);
        QTRY_COMPARE(w.library->playlists()->items()->spotifyCount(), 3);
        QTRY_VERIFY(w.find("spotifyMark") && w.find("spotifyMark")->property("visible").toBool());
        QTRY_VERIFY(w.find("playlistSummary")->property("text").toString().contains("3 from Spotify"));
        w.spotify->setEnabled(false);
        QTRY_VERIFY(w.find("playlistSummary")->property("text").toString().contains("3 from Spotify (off)"));
        w.window->close();
    }
};

int runMixedPlaylistTests(int argc, char **argv) {
    MixedPlaylistTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "mixed_tests.moc"
