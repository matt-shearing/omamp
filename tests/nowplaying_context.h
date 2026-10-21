#pragma once

// What Main.qml needs beside the player and the library since the now
// playing work: `appearance` (deck style, visualiser) and `nowPlaying` (the
// playing track's cover), and since the radio, `radio`. Both belong to the engine, so they go before the
// player and the library do. Settings stay in memory unless a window.conf
// is given; pictures go to the test's own art folder.

#include <QQmlContext>
#include <QQmlEngine>

#include "appearance.h"
#include "firstrun.h"
#include "nowplaying.h"
#include "player.h"
#include "spotifycontroller.h"
#include "radiocontroller.h"
#include "windowplacement.h"

class Player;
class Theme;

inline void installNowPlaying(QQmlEngine &engine, Theme *theme, Player *player, const QString &dbPath,
                              const QString &artDir, const QString &windowConf = {}) {
    auto *appearance = new Appearance(windowConf, theme, &engine);
    auto *now = new NowPlaying(player, dbPath, artDir, &engine);
    engine.rootContext()->setContextProperty("appearance", appearance);
    // The analyser's auto-range follows the setting, as main() wires it.
    if (player) {
        player->setVisualiserSensitivity(appearance->visualiserSensitivity());
        QObject::connect(appearance, &Appearance::changed, player,
                         [appearance, player] { player->setVisualiserSensitivity(appearance->visualiserSensitivity()); });
    }
    engine.rootContext()->setContextProperty("nowPlayingArt", now);
    // Spotify, switched off, with its settings beside the test's pictures.
    engine.rootContext()->setContextProperty(
        "spotify", new SpotifyController(artDir + "/spotify.conf", artDir + "/spotify", &engine));
    // Internet radio: its tiles go beside the test's pictures.
    auto *radio = new RadioController(dbPath, &engine);
    radio->setCacheDir(artDir + "/radio");
    radio->setPlayer(player);
    engine.rootContext()->setContextProperty("radio", radio);
    // Never asks the real Hyprland: the window is never tiled in tests.
    engine.rootContext()->setContextProperty("windowPlacement", new WindowPlacement(&engine, "/nonexistent/hyprctl"));
    // Already set up, in memory: the first-run setup never opens over a test's
    // window unless the test gives it a window.conf of its own.
    engine.rootContext()->setContextProperty("firstRun", new FirstRun({}, &engine));
}
