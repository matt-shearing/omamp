# Spotify through a backend program's command line (spotify_player or
# Spotifast): parsing its JSON, the settings and the backends. Core only,
# so the window, the CLI and the tests share it. docs/spotify.md.
INCLUDEPATH += $$PWD
HEADERS += $$PWD/spotify.h
SOURCES += $$PWD/spotify.cpp
