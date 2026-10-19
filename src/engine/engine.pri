# Playback: the play queue, QMediaPlayer and the analyser. Phase 3 replaces
# QMediaPlayer with an FFmpeg engine behind the same Player.
QT += multimedia
INCLUDEPATH += $$PWD

HEADERS += $$PWD/player.h $$PWD/playlist.h $$PWD/spectrum.h
SOURCES += $$PWD/player.cpp $$PWD/playlist.cpp $$PWD/spectrum.cpp
