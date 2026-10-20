# Phase 4: the web. Metadata from MusicBrainz, AcoustID and the Cover Art
# Archive, lyrics from LRCLIB, and scrobbling to ListenBrainz and Last.fm.
# Everything goes through one blocking HTTP client with per-service rate
# limits, retries and a response cache, so it runs on worker threads and in
# the command line alike.
QT += network
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/http.h \
    $$PWD/textmatch.h \
    $$PWD/musicbrainz.h \
    $$PWD/acoustid.h \
    $$PWD/coverart.h \
    $$PWD/identify.h \
    $$PWD/lyrics.h \
    $$PWD/scrobble.h \
    $$PWD/about.h

SOURCES += \
    $$PWD/http.cpp \
    $$PWD/textmatch.cpp \
    $$PWD/musicbrainz.cpp \
    $$PWD/acoustid.cpp \
    $$PWD/coverart.cpp \
    $$PWD/identify.cpp \
    $$PWD/lyrics.cpp \
    $$PWD/scrobble.cpp \
    $$PWD/about.cpp
