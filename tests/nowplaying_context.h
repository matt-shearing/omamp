#pragma once

// What Main.qml needs beside the player and the library since the now
// playing work: `appearance` (deck style, visualiser) and `nowPlaying` (the
// playing track's cover), and since the radio, `radio`. Both belong to the engine, so they go before the
// player and the library do. Settings stay in memory unless a window.conf
// is given; pictures go to the test's own art folder.

#include <QQmlContext>
#include <QQmlEngine>

#include "appearance.h"
#include "nowplaying.h"
#include "spotifycontroller.h"
#include "radiocontroller.h"

class Player;
class Theme;

inline void installNowPlaying(QQmlEngine &engine, Theme *theme, Player *player, const QString &dbPath,
                              const QString &artDir, const QString &windowConf = {}) {
    auto *appearance = new Appearance(windowConf, theme, &engine);
    auto *now = new NowPlaying(player, dbPath, artDir, &engine);
    engine.rootContext()->setContextProperty("appearance", appearance);
    engine.rootContext()->setContextProperty("nowPlayingArt", now);
    // Spotify, switched off, with its settings beside the test's pictures.
    engine.rootContext()->setContextProperty(
        "spotify", new SpotifyController(artDir + "/spotify.conf", artDir + "/spotify", &engine));
    // Internet radio: its tiles go beside the test's pictures.
    auto *radio = new RadioController(dbPath, &engine);
    radio->setCacheDir(artDir + "/radio");
    radio->setPlayer(player);
    engine.rootContext()->setContextProperty("radio", radio);
}
