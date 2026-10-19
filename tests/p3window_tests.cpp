// Phase 3 window: playlists in the tree and their view, the auto-playlist
// editor's live count and rule builder, Auto-DJ, party mode, the Settings
// page, and counting plays, driven through the controllers and the real
// Main.qml offscreen.
//
// Every library and music file lives in a temporary folder. Nothing reaches
// a sound card: the player runs on the native engine into an in-memory sink
// (BufferSink), as the engine tests do, and main() sets
// OMAMP_NO_AUDIO_DEVICE for anything else.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "artprovider.h"
#include "autodj.h"
#include "fixture_library.h"
#include "library.h"
#include "librarycontroller.h"
#include "nowplaying_context.h"
#include "m3u.h"
#include "nativeengine.h"
#include "pcmsink.h"
#include "player.h"
#include "playlists.h"
#include "query.h"
#include "playrecorder.h"
#include "rulebuilder.h"
#include "scanner.h"
#include "theme.h"
#include "translator.h"
#include "volumes.h"

namespace {

bool haveFfmpeg() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }

bool tone(const QString &file, const QStringList &metadata, double seconds = 1) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QStringList args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds), "-af", "volume=0.05",
                        "-map_metadata", "-1"};
    for (const QString &m : metadata)
        args << "-metadata" << m;
    args << file;
    QProcess p;
    p.start("ffmpeg", args);
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

QVariant sql(Library &library, const QString &query, const QVariantList &params = {}) {
    QSqlQuery q(library.db());
    q.prepare(query);
    for (const QVariant &p : params)
        q.addBindValue(p);
    return q.exec() && q.next() ? q.value(0) : QVariant();
}

// A player that never reaches a device: the native engine into memory,
// pulled 20 times faster than real time.
Player *silentPlayer(const QString &statePath = {}) {
    auto *sink = new BufferSink(48000, false);
    sink->setAutoPull(20);
    return new Player(new NativeEngine(sink), statePath);
}

// The made-up fixture library (no files) in a temporary folder.
struct FixtureLibrary {
    QTemporaryDir dir;
    std::unique_ptr<Player> player;
    std::unique_ptr<LibraryController> controller;
    bool open() {
        {
            Library library(dir.filePath("library.db"));
            if (!library.open() || !Fixture::fill(library))
                return false;
        }
        player.reset(silentPlayer());
        controller = std::make_unique<LibraryController>(dir.filePath("library.db"));
        controller->setWatching(false);
        controller->setArtCacheDir(dir.filePath("art"));
        controller->setPlayer(player.get());
        controller->recorder()->setFlushing(false);
        if (!controller->activate())
            return false;
        // The made-up volume isn't mounted here; call it online so its
        // tracks have paths (nothing is ever played from them).
        QSqlQuery q(controller->library()->db());
        q.exec("UPDATE volumes SET online = 1");
        q.exec("UPDATE tracks SET availability = 'online'");
        controller->refreshAll();
        return true;
    }
    qint64 id(const QString &title) {
        return sql(*controller->library(), "SELECT id FROM tracks WHERE title = ?", {title}).toLongLong();
    }
};

} // namespace

class P3WindowTests : public QObject {
    Q_OBJECT

private slots:
    // A short file played to its end through the in-memory sink counts once;
    // the list shows the new play count.
    void playingATrackCountsIt() {
        if (!haveFfmpeg())
            QSKIP("needs ffmpeg for the fixtures");
        QTemporaryDir dir;
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        QVERIFY(tone(root + "/music/A/01 One.flac", {"title=One", "artist=A"}, 2));
        QVERIFY(tone(root + "/music/A/02 Two.flac", {"title=Two", "artist=A"}, 2));
        {
            Library library(root + "/library.db");
            QVERIFY(library.open());
            Scanner(library, VolumeResolver()).scan({root + "/music"});
        }
        std::unique_ptr<Player> player(silentPlayer());
        LibraryController c(root + "/library.db");
        c.setWatching(false);
        c.setArtCacheDir(root + "/art");
        c.setPlayer(player.get());
        c.recorder()->setFlushing(false);
        QVERIFY(c.activate());
        QCOMPARE(c.tracks()->count(), 2);
        const qint64 one = sql(*c.library(), "SELECT id FROM tracks WHERE title = 'One'").toLongLong();
        const qint64 two = sql(*c.library(), "SELECT id FROM tracks WHERE title = 'Two'").toLongLong();

        QSignalSpy recorded(c.recorder(), &PlayRecorder::recorded);
        QSignalSpy finished(player.get(), &Player::playFinished);
        c.tracks()->select(0);
        QVERIFY(c.playRow(0) > 0);
        // Both play through and stop at the end of the list.
        QTRY_COMPARE_WITH_TIMEOUT(recorded.size(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(player->state(), QString("stopped"), 5000);
        QCOMPARE(recorded.at(0).at(1).toString(), QString("counted"));
        QCOMPARE(recorded.at(1).at(1).toString(), QString("counted"));
        // Heard about the whole two seconds each (not the position alone).
        QVERIFY2(finished.at(0).at(1).toLongLong() >= 1800, qPrintable(finished.at(0).at(1).toString()));
        QVERIFY(!finished.at(0).at(3).toBool());
        QCOMPARE(sql(*c.library(), "SELECT play_count FROM tracks WHERE id = ?", {one}).toInt(), 1);
        QCOMPARE(sql(*c.library(), "SELECT play_count FROM tracks WHERE id = ?", {two}).toInt(), 1);
        QVERIFY(sql(*c.library(), "SELECT last_played FROM tracks WHERE id = ?", {one}).toLongLong() > 0);
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM play_history").toInt(), 2);
        // The list's Plays column reads the new count.
        const int row = c.tracks()->rowOfTrack(one);
        QTRY_COMPARE(c.tracks()->data(c.tracks()->index(row, TrackListModel::Plays), Qt::DisplayRole).toString(),
                     QString("1"));
        // Nothing was queued for scrobbling: no service is set up.
        QCOMPARE(sql(*c.library(), "SELECT count(*) FROM scrobble_queue").toInt(), 0);

        // Next straight after starting is a skip.
        recorded.clear();
        QVERIFY(c.playRow(0) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(player->heardMs() > 0 || recorded.size() > 0, 5000);
        player->next();
        QTRY_VERIFY_WITH_TIMEOUT(recorded.size() >= 1, 5000);
        QCOMPARE(recorded.at(0).at(1).toString(), QString("skipped"));
        QCOMPARE(sql(*c.library(), "SELECT skip_count FROM tracks WHERE id = ?", {recorded.at(0).at(0)}).toInt(), 1);
        player->stop();
    }

    void ruleBuilderRoundTrips() {
        QStringList wrong;
        auto check = [&wrong](const QString &text, bool representable) {
            const RuleBuilder::Rules r = RuleBuilder::fromQuery(text);
            if (r.representable != representable)
                wrong << text + ": " + r.reason;
            return r;
        };
        RuleBuilder::Rules r = check("genre:jazz rating:4.. -artist:\"neon coast\" sort:-rating limit:50", true);
        QVERIFY(!r.any);
        QCOMPARE(r.rules.size(), 3);
        QCOMPARE(r.rules[0].field, QString("genre"));
        QCOMPARE(r.rules[0].op, QString("contains"));
        QCOMPARE(r.rules[1].op, QString("between"));
        QCOMPARE(r.rules[1].value, QString("4"));
        QCOMPARE(r.rules[2].op, QString("not_contains"));
        QCOMPARE(r.rules[2].value, QString("neon coast"));
        QCOMPARE(r.sort, QString("-rating"));
        QCOMPARE(r.limitKind, QString("count"));
        QCOMPARE(r.limitValue, QString("50"));

        r = check("genre:rock OR genre:metal", true);
        QVERIFY(r.any);
        QCOMPARE(RuleBuilder::toQuery(r), QString("genre:rock OR genre:metal"));
        // Aliases come back as the field's own name.
        r = check("ge:pop year:>2000 played:never sort:random limit:2h", true);
        QCOMPARE(r.rules[0].field, QString("genre"));
        QCOMPARE(r.rules[1].op, QString("gt"));
        QCOMPARE(r.rules[2].op, QString("empty"));
        QCOMPARE(r.limitKind, QString("time"));
        QCOMPARE(RuleBuilder::toQuery(r), QString("genre:pop year:>2000 played:none sort:random limit:2h"));

        // Things the rows can't hold.
        check("genre:rock OR genre:metal year:2000", false);
        check("(genre:rock year:1990..1999) OR rating:5", false);
        check("+neo", false);
        check("genre:\"unterminated", false);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(" | ")));

        // Rows to text: half-filled rows are left out, values are quoted.
        RuleBuilder::Rules built;
        built.rules = {{"artist", "is", "Pet Dogs", {}}, {"title", "matches", "^live", {}}, {"year", "contains", "", {}}};
        built.sort = "artist";
        built.limitKind = "size";
        built.limitValue = "700mb";
        QCOMPARE(RuleBuilder::toQuery(built), QString("artist:=\"Pet Dogs\" title:/^live/ sort:artist limit:700mb"));
        QVERIFY(RuleBuilder::fromQuery(RuleBuilder::toQuery(built)).representable);
    }

    // Create, add, duplicates, reorder by keys and drag, remove, unmatched
    // entries, export and import, Save Now Playing.
    void playlistsInTheTreeAndTheirView() {
        FixtureLibrary f;
        QVERIFY(f.open());
        LibraryController &c = *f.controller;
        PlaylistsController *p = c.playlists();
        const qint64 folder = p->createFolder("Road trips");
        QVERIFY(folder > 0);
        const qint64 list = p->createPlaylist("Summer", folder);
        QVERIFY(list > 0);
        QVERIFY(!p->createPlaylist("", 0)); // a name is needed
        const qint64 one = f.id("Coastline"), two = f.id("Go West"), three = f.id("夜の歌");
        QVERIFY(one && two && three);
        QCOMPARE(p->addTracks(list, {one, two, three}), 3);
        QCOMPARE(p->addTracks(list, {one}), 1); // duplicates allowed

        // The tree shows the folder, and the playlist inside it with its count.
        const int row = p->show(list);
        QVERIFY(row >= 0);
        QCOMPARE(c.currentKind(), QString("playlist"));
        QCOMPARE(c.tree()->key(c.tree()->parentRow(row)), QString("playlist:%1").arg(folder));
        QCOMPARE(c.tree()->data(c.tree()->index(row), LibraryTree::TrackCountRole).toLongLong(), 4);
        PlaylistItemsModel *items = p->items();
        QCOMPARE(items->playlistId(), list);
        QCOMPARE(items->count(), 4);
        QCOMPARE(items->duplicates(), 1);
        QVERIFY(items->data(items->index(0), PlaylistItemsModel::DuplicateRole).toBool());
        QVERIFY(!items->data(items->index(1), PlaylistItemsModel::DuplicateRole).toBool());

        // Alt+Down on the first: Neon Coast moves below Go West.
        items->select(0);
        QVERIFY(items->moveSelection(1));
        QCOMPARE(items->trackId(0), two);
        QCOMPARE(items->trackId(1), one);
        QCOMPARE(items->currentRow(), 1);
        // A drag of rows 0 and 1 to the end.
        items->select(0);
        items->select(1, Qt::ShiftModifier);
        QVERIFY(items->moveSelectionTo(items->count()));
        QCOMPARE(items->trackId(0), three);
        QCOMPARE(items->trackId(1), one);
        QCOMPARE(items->trackId(2), two);
        QCOMPARE(items->trackId(3), one);
        QCOMPARE(Playlists::trackIds(*c.library(), list), (QList<qint64>{three, one, two, one}));
        // Delete takes the selected entry out.
        items->select(3);
        QCOMPARE(items->removeSelection(), 1);
        QCOMPARE(items->count(), 3);
        QCOMPARE(items->duplicates(), 0);

        // An entry whose track left the library stays, unmatched.
        QSqlQuery(c.library()->db()).exec(QString("DELETE FROM tracks WHERE id = %1").arg(three));
        items->reload();
        QCOMPARE(items->count(), 3);
        QCOMPARE(items->unmatched(), 1);
        QCOMPARE(items->data(items->index(0), PlaylistItemsModel::AvailabilityRole).toString(), QString("unmatched"));

        // Export to M3U8 in the temporary folder, and import it back.
        const QString file = f.dir.filePath("export/Summer.m3u8");
        QDir().mkpath(f.dir.filePath("export"));
        const QVariantMap exported = p->exportTo(list, file);
        QVERIFY2(exported.value("ok").toBool(), qPrintable(exported.value("error").toString()));
        QCOMPARE(exported.value("written").toInt(), 3);
        QFile m3u(file);
        QVERIFY(m3u.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(m3u.readAll());
        QVERIFY(text.startsWith("#EXTM3U"));
        QVERIFY(text.contains("/music/"));
        QCOMPARE(text.count("#EXTINF"), 3);
        const QVariantMap imported = p->importFile(file);
        QVERIFY(imported.value("ok").toBool());
        QCOMPARE(imported.value("entries").toInt(), 3);
        QCOMPARE(imported.value("unmatched").toInt(), 1);

        // Rename, move to the top level, delete.
        QVERIFY(p->rename(list, "Summer 2026"));
        QCOMPARE(p->get(list).value("name").toString(), QString("Summer 2026"));
        QVERIFY(p->moveTo(list, 0));
        QVERIFY(!p->moveTo(folder, folder)); // not into itself
        QCOMPARE(p->get(list).value("parentId").toLongLong(), 0);
        QVERIFY(c.tree()->rowOfKey(QString("playlist:%1").arg(list)) >= 0);
        QVERIFY(p->remove(list));
        QVERIFY(p->get(list).isEmpty());
        QCOMPARE(c.tree()->rowOfKey(QString("playlist:%1").arg(list)), -1);

        // Save Now Playing keeps its order.
        f.player->appendTracks({{"/music/a.flac", "A", "X", 1000}, {c.library()->absolutePath(two), "Go West", "Pet Dogs", 290000}});
        const qint64 saved = p->saveNowPlaying("Tonight");
        QVERIFY(saved > 0);
        const QList<Playlists::Item> rows = Playlists::items(*c.library(), saved);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].trackId, 0);
        QCOMPARE(rows[1].trackId, two);
    }

    void autoPlaylistCountsLive() {
        FixtureLibrary f;
        QVERIFY(f.open());
        PlaylistsController *p = f.controller->playlists();
        const qint64 all = sql(*f.controller->library(), "SELECT count(*) FROM tracks").toLongLong();
        QVariantMap r = p->checkQuery("");
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(r.value("count").toLongLong(), all);
        r = p->checkQuery("genre:pop");
        QVERIFY(r.value("ok").toBool());
        const qint64 pop = sql(*f.controller->library(),
                               "SELECT count(*) FROM tracks WHERE id IN (SELECT track_id FROM track_genres tg "
                               "JOIN genres g ON g.id = tg.genre_id WHERE g.name LIKE '%pop%')")
                               .toLongLong();
        QVERIFY(pop > 0);
        QCOMPARE(r.value("count").toLongLong(), pop);
        QCOMPARE(r.value("preview").toList().size(), int(pop));
        r = p->checkQuery("genre:pop limit:1");
        QCOMPARE(r.value("count").toLongLong(), 1);
        r = p->checkQuery("year:abc");
        QVERIFY(!r.value("ok").toBool());
        QVERIFY(!r.value("error").toString().isEmpty());

        const qint64 id = p->createAutoPlaylist("Pop", "genre:pop");
        QVERIFY(id > 0);
        QVERIFY(!p->createAutoPlaylist("Broken", "year:abc"));
        QVERIFY(p->show(id) >= 0);
        QCOMPARE(f.controller->currentKind(), QString("autoplaylist"));
        QCOMPARE(p->items()->count(), int(pop));
        QVERIFY(!p->items()->editable());
        QVERIFY(p->saveAutoPlaylist(id, "Pop, rated", "genre:pop limit:1"));
        QCOMPARE(p->items()->count(), 1);
    }

    void autoDjKeepsTheQueueFull() {
        FixtureLibrary f;
        QVERIFY(f.open());
        DjController *dj = f.controller->dj();
        dj->saveSettings({{"upcoming", 3}, {"artistSeparation", 0}, {"skipPlayedWithinDays", 0}});
        QCOMPARE(dj->settings().value("upcoming").toInt(), 3);
        dj->setEnabled(true);
        QVERIFY(dj->enabled());
        Player &player = *f.player;
        // An empty Now Playing: the current track and three after it.
        QTRY_COMPARE(player.playlist()->count(), 4);
        QCOMPARE(player.currentIndex(), 0);
        player.next();
        QTRY_COMPARE(player.playlist()->count(), 5);
        QCOMPARE(player.playlist()->count() - player.currentIndex() - 1, 3);
        player.next();
        QTRY_COMPARE(player.playlist()->count() - player.currentIndex() - 1, 3);
        // Off: nothing more is added.
        dj->setEnabled(false);
        player.next();
        QTest::qWait(50);
        QCOMPARE(player.playlist()->count() - player.currentIndex() - 1, 2);
        player.stop();

        // The setting and the source survive a restart of the controller.
        dj->saveSettings({{"source", "query:genre:pop"}});
        LibraryController again(f.dir.filePath("library.db"));
        again.setWatching(false);
        QVERIFY(again.activate());
        QCOMPARE(again.dj()->settings().value("source").toString(), QString("query:genre:pop"));
        QCOMPARE(again.dj()->settings().value("upcoming").toInt(), 3);
        QVERIFY(!again.dj()->sourceChoices().isEmpty());
    }

    void partyModeLocksAndUnlocks() {
        FixtureLibrary f;
        QVERIFY(f.open());
        DjController *dj = f.controller->dj();
        QVERIFY(dj->allowed("edit-tags"));
        QVERIFY(dj->enableParty("jukebox", "1234", true));
        QVERIFY(dj->partyOn());
        QVERIFY(dj->partyHasPin());
        QVERIFY(dj->partyDisplay());
        QVERIFY(!dj->allowed("edit-tags"));
        QVERIFY(!dj->allowed("edit-playlists"));
        QVERIFY(!dj->allowed("settings"));
        QVERIFY(!dj->allowed("quit"));
        QVERIFY(dj->allowed("add"));
        QVERIFY(!dj->disableParty("0000"));
        QVERIFY(dj->partyOn());
        // Party mode is read at start, before the library window opens.
        LibraryController second(f.dir.filePath("library.db"));
        second.dj()->loadSaved(f.dir.filePath("library.db"));
        QVERIFY(second.dj()->partyOn());
        QVERIFY(dj->disableParty("1234"));
        QVERIFY(!dj->partyOn());
        QVERIFY(dj->allowed("edit-tags"));
    }

    void settingsRoundTrip() {
        FixtureLibrary f;
        QVERIFY(f.open());
        SettingsController *s = f.controller->appSettings();
        QVERIFY(s->setValue("acoustid.key", "abc123"));
        QVERIFY(s->setValue("coverart.size", 1200));
        s->setFlag("tags.write_ratings", true);
        s->setFlag("watch.enabled", false);
        QVERIFY(s->setValue("watch.network_minutes", 5));
        QVERIFY(s->setValue("identify.countries", "AU,GB"));
        QVERIFY(!s->setValue("listenbrainz.token", "x")); // only through validation
        QVERIFY(!s->setValue("party_mode", "{}"));
        QSqlQuery(f.controller->library()->db()).exec("INSERT INTO folders (volume_id, rel_path, is_root, added) VALUES (1, '', 1, 0)");
        QCOMPARE(s->folders(), QStringList{"/music"});
        f.controller.reset();
        LibraryController again(f.dir.filePath("library.db"));
        again.setWatching(false);
        QVERIFY(again.activate());
        SettingsController *t = again.appSettings();
        QCOMPARE(t->value("acoustid.key").toString(), QString("abc123"));
        QCOMPARE(t->value("coverart.size").toInt(), 1200);
        QVERIFY(t->flag("tags.write_ratings", false));
        QVERIFY(!t->flag("watch.enabled", true));
        QCOMPARE(t->value("watch.network_minutes").toInt(), 5);
        QCOMPARE(t->value("identify.countries").toString(), QString("AU,GB"));
        QCOMPARE(t->scrobbleStatus().size(), 2);

        // The player's side: omamp.conf beside the state file.
        QTemporaryDir dir;
        const QString state = dir.filePath("state.json");
        {
            std::unique_ptr<Player> player(silentPlayer(state));
            player->setCrossfadeSeconds(3);
            player->setReplayGainMode("album");
            player->setReplayGainPreamp(2);
            player->setClipPrevention(false);
            player->setEqEnabled(true);
            player->applyEqPreset("Rock");
            player->setEqPreamp(-3);
            player->setPreferredEngine("native");
        }
        std::unique_ptr<Player> player(silentPlayer(state));
        QCOMPARE(player->crossfadeSeconds(), 3.0);
        QCOMPARE(player->replayGainMode(), QString("album"));
        QCOMPARE(player->replayGainPreamp(), 2.0);
        QVERIFY(!player->clipPrevention());
        QVERIFY(player->eqEnabled());
        QCOMPARE(player->eqPreamp(), -3.0);
        QCOMPARE(player->preferredEngine(), QString("native"));
        QCOMPARE(QSettings(dir.filePath("omamp.conf"), QSettings::IniFormat).value("playback/engine").toString(),
                 QString("native"));
    }

    void collectionsWithTheirOwnNodes() {
        FixtureLibrary f;
        QVERIFY(f.open());
        PlaylistsController *p = f.controller->playlists();
        const qint64 id = p->saveCollection({{"name", "Pop only"},
                                             {"query", "genre:pop"},
                                             {"tree", QStringList{"artist", "year"}},
                                             {"columns", QStringList{"title", "artist", "year"}}});
        QVERIFY(id > 0);
        QVERIFY(!p->saveCollection({{"name", "Bad"}, {"query", "year:abc"}}));
        LibraryTree *tree = f.controller->tree();
        const int row = tree->reveal({"collections", QString("collection:%1").arg(id)});
        QVERIFY(row >= 0);
        QVERIFY(tree->isExpandable(row));
        tree->expand(row);
        auto label = [&](int r) { return tree->data(tree->index(r), LibraryTree::LabelRole).toString(); };
        QCOMPARE(label(row + 1), QString("Artist"));
        QCOMPARE(label(row + 2), QString("Year"));
        tree->expand(row + 1);
        QCOMPARE(tree->kind(row + 2), QString("cvalue"));
        f.controller->selectNode(row + 2);
        QVERIFY(f.controller->tracks()->count() > 0);
        p->setCollectionColumns(id, {"title", "rating"});
        QCOMPARE(p->collection(id).value("columns").toStringList(), (QStringList{"title", "rating"}));
    }
};

// The same from the keyboard and the palette, in the real window.
class P3WindowKeysTests : public QObject {
    Q_OBJECT

    // Main.qml over the made-up library, with a player that never reaches a device.
    struct Window {
        QTemporaryDir dir;
        Translator translator;
        Theme theme;
        std::unique_ptr<Player> player;
        std::unique_ptr<LibraryController> library;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;

        Window()
            : translator(dir.filePath("language")), theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl")) {}
        bool load() {
            {
                Library lib(dir.filePath("library.db"));
                if (!lib.open() || !Fixture::fill(lib))
                    return false;
                QSqlQuery q(lib.db());
                q.exec("UPDATE tracks SET availability = 'online'");
            }
            {
                QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
                conf.setValue("window/advanced", true);
            }
            player.reset(silentPlayer());
            library = std::make_unique<LibraryController>(dir.filePath("library.db"));
            library->setPlayer(player.get());
            library->setWatching(false);
            library->setArtCacheDir(dir.filePath("art"));
            library->setWriteSandbox(dir.path());
            library->recorder()->setFlushing(false);
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("backend", player.get());
            engine.rootContext()->setContextProperty("library", library.get());
            engine.rootContext()->setContextProperty("commandPalette", library->palette());
            engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(dir.filePath("window.conf")));
            installNowPlaying(engine, &theme, player.get(), dir.filePath("library.db"), dir.filePath("art"));
            engine.addImageProvider(ArtImageProvider::name,
                                    new ArtImageProvider(dir.filePath("library.db"), dir.filePath("art")));
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
        // Runs a palette action by typing its title.
        bool palette(const QString &text, const QString &id) {
            QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
            type(">" + text);
            if (!QTest::qWaitFor([&] { return library->palette()->get(0).value("itemId").toString() == id; }, 3000))
                return false;
            QTest::keyClick(window, Qt::Key_Return);
            return true;
        }
    };

private slots:
    void playlistFromThePaletteReorderAndRemove() {
        Window w;
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        QTRY_VERIFY(c.ready());
        QTRY_VERIFY(w.find("advancedView"));

        // New playlist from the palette: the name sheet, a name, Enter.
        QVERIFY(w.palette("new playlist", "new-playlist"));
        QObject *sheet = w.find("nameSheet");
        QVERIFY(sheet);
        QTRY_VERIFY(sheet->property("opened").toBool());
        QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
        w.type("Mix");
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!sheet->property("opened").toBool());
        QCOMPARE(c.currentKind(), QString("playlist"));
        QCOMPARE(c.currentLabel(), QString("Mix"));
        const qint64 id = c.playlists()->items()->playlistId();
        QVERIFY(id > 0);
        QObject *view = w.find("playlistView");
        QTRY_VERIFY(view && view->property("visible").toBool());
        QVERIFY(!w.find("trackTable")->property("visible").toBool());

        // Three tracks in, as a drop would add them.
        const qint64 a = sql(*c.library(), "SELECT id FROM tracks WHERE title = 'Coastline'").toLongLong();
        const qint64 b = sql(*c.library(), "SELECT id FROM tracks WHERE title = 'Go West'").toLongLong();
        const qint64 d = sql(*c.library(), "SELECT id FROM tracks WHERE title = 'Heatwave'").toLongLong();
        QCOMPARE(c.playlists()->addTracks(id, {a, b, d}), 3);
        PlaylistItemsModel *items = c.playlists()->items();
        QCOMPARE(items->count(), 3);
        // The tree's count follows.
        const int row = c.tree()->rowOfKey(QString("playlist:%1").arg(id));
        QCOMPARE(c.tree()->data(c.tree()->index(row), LibraryTree::TrackCountRole).toLongLong(), 3);

        // From the keyboard: Down to the second entry, Alt+Up moves it first,
        // Delete takes out the cursor's entry.
        QMetaObject::invokeMethod(w.find("advancedView"), "focusList");
        QTRY_VERIFY(w.find("playlistList")->property("activeFocus").toBool());
        QTest::keyClick(w.window, Qt::Key_Down);
        QTest::keyClick(w.window, Qt::Key_Down);
        QCOMPARE(items->currentRow(), 1);
        QTest::keyClick(w.window, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(Playlists::trackIds(*c.library(), id), (QList<qint64>{b, a, d}));
        QTest::keyClick(w.window, Qt::Key_End);
        QTest::keyClick(w.window, Qt::Key_Delete);
        QCOMPARE(Playlists::trackIds(*c.library(), id), (QList<qint64>{b, a}));
        QCOMPARE(w.player->state(), QString("stopped"));

        // Export to the temporary folder writes an M3U8 of both.
        const QVariantMap r = c.playlists()->exportTo(id, w.dir.filePath("Mix.m3u8"));
        QVERIFY(r.value("ok").toBool());
        QFile m3u(w.dir.filePath("Mix.m3u8"));
        QVERIFY(m3u.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(m3u.readAll()).count("#EXTINF"), 2);

        // Delete from the palette asks first.
        QVERIFY(w.palette("delete the chosen playlist", "delete-playlist"));
        QObject *confirm = w.find("confirmSheet");
        QTRY_VERIFY(confirm->property("opened").toBool());
        QTest::keyClick(w.window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!confirm->property("opened").toBool());
        QVERIFY(c.playlists()->get(id).isEmpty());
        w.window->close();
    }

    // Tracks dragged with the mouse from the list onto a playlist in the tree.
    void dragTracksOntoAPlaylist() {
        Window w;
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        QTRY_VERIFY(c.ready());
        const qint64 id = c.playlists()->createPlaylist("Dropped");
        QVERIFY(id > 0);
        c.selectNode(0);
        QTRY_COMPARE(c.tracks()->count(), int(Fixture::rows().size()));
        const int node = c.tree()->reveal({"playlists", QString("playlist:%1").arg(id)});
        QVERIFY(node >= 0);
        c.selectNode(0);
        c.tracks()->select(0);
        c.tracks()->select(1, Qt::ShiftModifier);
        QCOMPARE(c.tracks()->selectedCount(), 2);
        auto *table = qobject_cast<QQuickItem *>(w.find("trackTable"));
        auto *tree = qobject_cast<QQuickItem *>(w.find("treeList"));
        QVERIFY(table && tree);
        QTRY_VERIFY(table->isVisible());
        QTest::qWait(100);
        // The first row's title cell, and the playlist's node in the tree.
        const QPoint from = table->mapToScene(QPointF(200, 12)).toPoint();
        const QPoint to = tree->mapToScene(QPointF(60, node * 24 + 12 - tree->property("contentY").toReal())).toPoint();
        QTest::mousePress(w.window, Qt::LeftButton, Qt::NoModifier, from);
        // Moved in steps, as a hand would.
        for (int i = 1; i <= 12; ++i) {
            const QPoint at = from + (to - from) * i / 12;
            QMouseEvent move(QEvent::MouseMove, at, w.window->mapToGlobal(at), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QGuiApplication::sendEvent(w.window, &move);
            QTest::qWait(15);
        }
        QTest::mouseRelease(w.window, Qt::LeftButton, Qt::NoModifier, to);
        QTRY_COMPARE(Playlists::trackIds(*c.library(), id).size(), 2);
        // Both selected tracks went in, in list order.
        QCOMPARE(Playlists::trackIds(*c.library(), id).first(), c.tracks()->trackId(0));
        w.window->close();
    }

    void autoPlaylistEditorCountsAsYouType() {
        Window w;
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        QTRY_VERIFY(c.ready());
        QVERIFY(w.palette("new auto-playlist", "new-autoplaylist"));
        QObject *sheet = w.find("autoPlaylistSheet");
        QTRY_VERIFY(sheet && sheet->property("opened").toBool());
        QObject *query = w.find("autoQuery");
        QObject *check = w.find("autoCheck");
        QVERIFY(query && check);
        const qint64 pop = Query::run(*c.library(), "genre:pop").total;
        QVERIFY(pop > 0);
        query->setProperty("text", "genre:pop");
        QTRY_COMPARE(check->property("text").toString(), QString("%1 tracks match").arg(pop));
        QCOMPARE(sheet->property("check").toMap().value("count").toLongLong(), pop);
        // The rules show it: one row, genre contains pop.
        QVariantMap rules = sheet->property("rules").toMap();
        QVERIFY(rules.value("representable").toBool());
        QCOMPARE(rules.value("rules").toList().size(), 1);
        // A query the rules can't hold.
        query->setProperty("text", "(genre:pop year:2004) OR rating:5");
        QTRY_VERIFY(!sheet->property("rules").toMap().value("representable").toBool());
        QTRY_VERIFY(w.find("advancedNote")->property("visible").toBool());
        // A broken one says where.
        query->setProperty("text", "year:abc");
        QTRY_VERIFY(!sheet->property("check").toMap().value("ok").toBool());
        QVERIFY(check->property("text").toString().contains("character"));
        query->setProperty("text", "genre:pop limit:1");
        // (The tests install no translator, so the English plural form is not applied.)
        QTRY_VERIFY(check->property("text").toString().startsWith("1 track"));
        // Ctrl+Enter creates it and shows it.
        QTest::keyClick(w.window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!sheet->property("opened").toBool());
        QCOMPARE(c.currentKind(), QString("autoplaylist"));
        QCOMPARE(c.playlists()->items()->count(), 1);
        w.window->close();
    }

    void autoDjAndSettingsFromTheKeys() {
        Window w;
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        QTRY_VERIFY(c.ready());
        // The made-up volume isn't mounted here; call it online.
        QSqlQuery(c.library()->db()).exec("UPDATE volumes SET online = 1");
        QSqlQuery(c.library()->db()).exec("UPDATE tracks SET availability = 'online'");
        c.refreshAll();
        c.dj()->saveSettings({{"upcoming", 2}, {"artistSeparation", 0}, {"skipPlayedWithinDays", 0}});
        QTest::keyClick(w.window, Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(c.dj()->enabled());
        QTRY_COMPARE(w.player->playlist()->count(), 3);
        QVERIFY(w.find("autoDjLamp")->property("on").toBool());
        QTest::keyClick(w.window, Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(!c.dj()->enabled());

        // Ctrl+, opens Settings; a playback change goes to the player.
        QTest::keyClick(w.window, Qt::Key_Comma, Qt::ControlModifier);
        QObject *settings = w.find("settingsSheet");
        QTRY_VERIFY(settings && settings->property("opened").toBool());
        QVERIFY(w.window->property("sheetOpen").toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());
        w.window->close();
    }

    void partyModeLocksTheWindow() {
        Window w;
        QVERIFY(w.load());
        LibraryController &c = *w.library;
        QTRY_VERIFY(c.ready());
        QTRY_COMPARE(c.tracks()->count(), int(Fixture::rows().size()));
        QVERIFY(c.dj()->enableParty("jukebox", "1234", false));
        QTRY_VERIFY(w.find("partyLamp")->property("on").toBool());

        // Ctrl+E does nothing but say so.
        c.tracks()->select(0);
        QTRY_VERIFY(w.find("trackTable")->property("activeFocus").toBool());
        QTest::keyClick(w.window, Qt::Key_E, Qt::ControlModifier);
        QVERIFY(!w.find("tagEditor")->property("opened").toBool());
        QObject *toast = w.find("toast");
        QTRY_VERIFY(toast->property("visible").toBool());
        QCOMPARE(toast->property("text").toString(), QString("Locked in party mode"));
        // Settings and new playlists are locked too.
        QTest::keyClick(w.window, Qt::Key_Comma, Qt::ControlModifier);
        QVERIFY(!w.find("settingsSheet")->property("opened").toBool());
        // Q asks for the PIN instead of quitting.
        QTest::keyClick(w.window, Qt::Key_Q);
        QObject *party = w.find("partySheet");
        QTRY_VERIFY(party->property("opened").toBool());
        QVERIFY(w.window->isVisible());
        // A wrong PIN keeps it locked; Esc, then leave from the palette with the right one.
        QObject *pin = w.find("partyPin");
        QMetaObject::invokeMethod(pin, "forceActiveFocus");
        QTRY_VERIFY(pin->property("activeFocus").toBool());
        w.type("0000");
        QTest::keyClick(w.window, Qt::Key_Return);
        QVERIFY(c.dj()->partyOn());
        QVERIFY(party->property("wrongPin").toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        QTRY_VERIFY(!party->property("opened").toBool());
        QVERIFY(w.window->isVisible());
        party->setProperty("then", "leave");
        QMetaObject::invokeMethod(party, "openSheet", Q_ARG(QVariant, QString("leave")));
        QTRY_VERIFY(party->property("opened").toBool());
        QMetaObject::invokeMethod(pin, "forceActiveFocus");
        w.type("1234");
        QTest::keyClick(w.window, Qt::Key_Return);
        QTRY_VERIFY(!c.dj()->partyOn());
        QTRY_VERIFY(!party->property("opened").toBool());
        QVERIFY(w.window->isVisible());
        // Unlocked: Ctrl+E opens the editor again.
        QMetaObject::invokeMethod(w.find("advancedView"), "focusList");
        QTest::keyClick(w.window, Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(w.find("tagEditor")->property("opened").toBool());
        QTest::keyClick(w.window, Qt::Key_Escape);
        w.window->close();
    }
};

int runP3WindowTests(int argc, char **argv) {
    P3WindowTests tests;
    P3WindowKeysTests keys;
    return QTest::qExec(&tests, argc, argv) + QTest::qExec(&keys, argc, argv);
}

#include "p3window_tests.moc"
