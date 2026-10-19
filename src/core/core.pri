# The library core: database, volumes, scanner, query language, jobs,
# folder watching, the artwork cache, playlists, play statistics, Auto-DJ
# and the MediaMonkey importer. Qt Core, Qt SQL and Qt GUI (for QImage, used
# on the CPU only), so the app, the CLI and the tests share it without
# pulling in a window or audio. The importer links the SQLite C library
# directly, to register MediaMonkey's custom collations on its copy.
QT += sql gui
CONFIG += link_pkgconfig
PKGCONFIG += taglib sqlite3
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/artwork.h \
    $$PWD/autodj.h \
    $$PWD/m3u.h \
    $$PWD/mmimport.h \
    $$PWD/playlists.h \
    $$PWD/stats.h \
    $$PWD/audit.h \
    $$PWD/duplicates.h \
    $$PWD/job.h \
    $$PWD/library.h \
    $$PWD/mask.h \
    $$PWD/migrations.h \
    $$PWD/organise.h \
    $$PWD/ownwrites.h \
    $$PWD/palette.h \
    $$PWD/query.h \
    $$PWD/scanner.h \
    $$PWD/tagreader.h \
    $$PWD/tagwriter.h \
    $$PWD/volumes.h \
    $$PWD/watcher.h

SOURCES += \
    $$PWD/artwork.cpp \
    $$PWD/autodj.cpp \
    $$PWD/m3u.cpp \
    $$PWD/mmimport.cpp \
    $$PWD/playlists.cpp \
    $$PWD/stats.cpp \
    $$PWD/audit.cpp \
    $$PWD/duplicates.cpp \
    $$PWD/job.cpp \
    $$PWD/library.cpp \
    $$PWD/mask.cpp \
    $$PWD/migrations.cpp \
    $$PWD/organise.cpp \
    $$PWD/ownwrites.cpp \
    $$PWD/palette.cpp \
    $$PWD/query.cpp \
    $$PWD/scanner.cpp \
    $$PWD/tagreader.cpp \
    $$PWD/tagwriter.cpp \
    $$PWD/volumes.cpp \
    $$PWD/watcher.cpp

include($$PWD/net/net.pri)
# Phase 5: the NAS. Network volumes (probes that never hang, the mount
# monitor, the folder walker with its filesystem seam), the waveform cache,
# and the OpenSubsonic client that mirrors a Navidrome library.
QT += network
HEADERS += $$PWD/mounts.h $$PWD/peaks.h $$PWD/remote.h $$PWD/subsonic.h $$PWD/walker.h
SOURCES += $$PWD/mounts.cpp $$PWD/peaks.cpp $$PWD/remote.cpp $$PWD/subsonic.cpp $$PWD/walker.cpp
# Phase 6: device sync profiles (copy and convert with the ffmpeg command)
# and track-list reports (CSV and HTML).
HEADERS += $$PWD/devicesync.h $$PWD/report.h
SOURCES += $$PWD/devicesync.cpp $$PWD/report.cpp
