// Thrum — a Winamp-style music player that wears the Omarchy theme.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>

#include "appearance.h"
#include "levelsfeed.h"
#include "artprovider.h"
#include "cli.h"
#include "devhooks.h"
#include "firstrun.h"
#include "handoffengine.h"
#include "librarycontroller.h"
#include "mpris.h"
#include "nowplaying.h"
#include "organisemodel.h"
#include "player.h"
#include "playrecorder.h"
#include "portal.h"
#include "playlistscontroller.h"
#include "spotifycontroller.h"
#include "radiocontroller.h"
#include "streamresolver.h"
#include "tageditor.h"
#include "theme.h"
#include "windowplacement.h"
#include "translator.h"
#include "visualiser.h"

static const char *version = "0.4.0";

int main(int argc, char *argv[]) {
    DevHooks::markStart();
    // Commands run without a display: a QCoreApplication never touches the
    // GUI platform, so `thrum scan` works over SSH and in scripts.
    if (argc > 1 && Cli::isCommand(QString::fromLocal8Bit(argv[1]))) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("thrum");
        app.setApplicationVersion(version);
        QTextStream out(stdout), err(stderr);
        return Cli::run(app.arguments().mid(1), out, err);
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("thrum");
    app.setApplicationVersion(version);

    // Matches the window to thrum.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("thrum");
    app.setWindowIcon(QIcon::fromTheme("thrum"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    FolderPicker picker;
    // Tracks on a Subsonic server play through their signed stream URL,
    // asked for when they play and never saved.
    StreamResolver streams({});
    // Internet radio: a station in the queue (radio://<key>) plays from the
    // address the radio picks for it.
    RadioController radio;
    streams.setRadioResolver([&radio](const QString &path, QString *error) { return radio.resolve(path, error); });
    // Spotify (docs/spotify.md), off until switched on in Settings. Its
    // tracks sit in Now Playing and play on the backend's Spotify Connect
    // device: HandoffEngine passes them over and pauses Thrum's own engine.
    SpotifyController spotify;
    const QString statePath =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/thrum/state.json";
    Player player(new HandoffEngine(Player::createEngine(statePath), spotify.playback()), statePath,
                  streams.function());
    spotify.setPlayer(&player);
    QObject::connect(&player, &Player::folderRequested, &picker, &FolderPicker::pickFolders);
    QObject::connect(&picker, &FolderPicker::picked, &player, &Player::addPicked);
    QObject::connect(&picker, &FolderPicker::failed, &player, &Player::reportError);

    // Files and folders on the command line (or from "Open with") are added
    // and the first of them starts playing. A first run with nothing to play
    // offers the Music folder.
    QStringList arguments = app.arguments().mid(1);
    // `thrum mini` and `thrum micro` open the compact players, so a
    // Hyprland rule can float and pin them as they open (docs/now-playing.md).
    QString startMode;
    if (arguments.value(0) == "mini" || arguments.value(0) == "micro")
        startMode = arguments.takeFirst();
    if (arguments.value(0) == "open")
        arguments.removeFirst();
    // radio://<station> on the command line (`thrum radio play` when Thrum
    // isn't running) tunes in once the window is up.
    QStringList stations;
    for (int i = arguments.size() - 1; i >= 0; --i)
        if (arguments[i].startsWith("radio://"))
            stations.prepend(arguments.takeAt(i));
    if (!arguments.isEmpty()) {
        const int before = player.playlist()->count();
        if (player.addPaths(arguments) > 0)
            player.playIndex(before);
    } else if (player.playlist()->count() == 0 && stations.isEmpty()) {
        const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        if (QFileInfo(music).isDir())
            player.addPaths({music});
    }

    // The library window's controller. The library opens the first time
    // advanced mode shows; "Add folder to library" has its own picker so its
    // folders go to the library, not to Now Playing.
    LibraryController library;
    FolderPicker libraryPicker;
    library.setPlayer(&player);
    library.setPicker(&libraryPicker);
    // The tag editor's picture and organise's destination each get their own.
    FolderPicker artworkPicker, destinationPicker;
    library.tagEditor()->setPicker(&artworkPicker);
    library.organiser()->setPicker(&destinationPicker);
    // The track list marks the track that is playing.
    auto markPlaying = [&] {
        const int row = player.currentIndex();
        library.tracks()->setPlayingPath(row >= 0 ? player.playlist()->at(row).path : QString());
    };
    QObject::connect(&player, &Player::trackChanged, &library, markPlaying);
    markPlaying();
    // Party mode holds across a restart; scrobbles queued while offline go
    // out at start (only when a service has credentials).
    library.dj()->loadSaved(library.databasePath());
    library.recorder()->flushIfConfigured();
    // Quitting part-way through a track still counts what was heard.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &player, [&player] { player.finishPlay(false); });

    // Spotify in the palette, the tree and the deck's cover.
    spotify.setPalette(library.palette());
    // Mixed playlists: Spotify tracks in Thrum playlists.
    library.playlists()->setSpotify(&spotify);
    NowPlaying::setReferenceArt([&spotify](const QString &path) { return spotify.art()->fileForPath(path); });
    auto spotifyNode = [&] { library.tree()->setExtraNode("spotify", QObject::tr("Spotify"), spotify.enabled()); };
    QObject::connect(&spotify, &SpotifyController::settingsChanged, &library, spotifyNode);
    spotifyNode();
    radio.setPlayer(&player);
    QObject::connect(&radio, &RadioController::scrobbleQueued, library.recorder(), &PlayRecorder::flushIfConfigured);
    if (!stations.isEmpty())
        radio.tune(stations.first());

    // The playing track's cover, for the window and for MPRIS.
    NowPlaying nowPlaying(&player);
    QObject::connect(&spotify, &SpotifyController::playingArtChanged, &nowPlaying, &NowPlaying::refreshSoon);
    QObject::connect(&library, &LibraryController::revisionChanged, &nowPlaying, &NowPlaying::refreshSoon);
    const QString windowConf =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/thrum/window.conf";
    // Before anything reads window.conf: a new install gets the setup, an
    // older one the sidebar switched on once (firstrun.h).
    FirstRun firstRun(windowConf);
    Appearance appearance(windowConf, &theme);
    qmlRegisterType<Visualiser>("Thrum", 1, 0, "Visualiser");

    QObject mprisHost;
    registerMpris(&mprisHost, &player, &nowPlaying);
    setMprisUriHandler(&mprisHost, [&radio](const QString &uri) { return radio.openUri(uri); });

    // The analyser for the Omarchy bar plugin, over a local socket
    // (levelsfeed.h). Idle until the plugin connects and music plays.
    LevelsFeed levelsFeed(windowConf);
    levelsFeed.setLevelsSource([&player] { return player.levels(); });
    auto syncLevelsFeed = [&] {
        levelsFeed.setStyle(appearance.visualiser());
        levelsFeed.setReduceMotion(appearance.reduceMotion());
        levelsFeed.setActive(player.state() == "playing" && !player.playsElsewhere());
    };
    QObject::connect(&player, &Player::stateChanged, &levelsFeed, syncLevelsFeed);
    QObject::connect(&player, &Player::trackChanged, &levelsFeed, syncLevelsFeed);
    QObject::connect(&appearance, &Appearance::changed, &levelsFeed, syncLevelsFeed);
    // The analyser's auto-range follows Settings > Appearance > Visualiser.
    player.setVisualiserSensitivity(appearance.visualiserSensitivity());
    QObject::connect(&appearance, &Appearance::changed, &player,
                     [&] { player.setVisualiserSensitivity(appearance.visualiserSensitivity()); });
    syncLevelsFeed();
    levelsFeed.start();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &player);
    engine.rootContext()->setContextProperty("library", &library);
    engine.rootContext()->setContextProperty("nowPlayingArt", &nowPlaying);
    engine.rootContext()->setContextProperty("appearance", &appearance);
    engine.rootContext()->setContextProperty("levelsFeed", &levelsFeed);
    engine.rootContext()->setContextProperty("firstRun", &firstRun);
    engine.rootContext()->setContextProperty("spotify", &spotify);
    engine.rootContext()->setContextProperty("radio", &radio);
    engine.rootContext()->setContextProperty("startMode", startMode);
    // Whether Hyprland tiles the window, so it never tries to resize itself there.
    WindowPlacement placement;
    engine.rootContext()->setContextProperty("windowPlacement", &placement);
    // The command palette (Ctrl+K) searches the library once it is open, and
    // lists the actions Main.qml registers before that.
    engine.rootContext()->setContextProperty("commandPalette", library.palette());
    // Album and track pictures: image://thrum-art/album/<id>/<size>.
    engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider);
    engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(windowConf));
    DevHooks hooks(&engine, &library);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (auto *root = mprisHost.findChild<MprisRoot *>()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QObject::connect(root, &MprisRoot::raiseRequested, window, [window] {
            window->show();
            window->raise();
            window->requestActivate();
        });
    }

    return app.exec();
}
