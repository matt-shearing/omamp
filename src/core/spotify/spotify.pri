# Spotify through a backend program's command line (spotify_player or
# Spotifast): parsing its JSON, the settings and the backends; the queue
# that keeps its commands from bursting at Spotify's Web API; and following
# its playback over MPRIS. Core only, so the window, the CLI and the tests
# share it. docs/spotify.md.
QT += dbus
INCLUDEPATH += $$PWD
HEADERS += $$PWD/spotify.h $$PWD/spotifyrequests.h $$PWD/spotifympris.h $$PWD/spotifysession.h
SOURCES += $$PWD/spotify.cpp $$PWD/spotifyrequests.cpp $$PWD/spotifympris.cpp $$PWD/spotifysession.cpp
