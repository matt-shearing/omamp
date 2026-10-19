# The library core: database, volumes, scanner, query language, jobs,
# folder watching and the artwork cache. Qt Core, Qt SQL and Qt GUI (for
# QImage, used on the CPU only), so the app, the CLI and the tests share it
# without pulling in a window or audio.
QT += sql gui
CONFIG += link_pkgconfig
PKGCONFIG += taglib
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/artwork.h \
    $$PWD/audit.h \
    $$PWD/duplicates.h \
    $$PWD/job.h \
    $$PWD/library.h \
    $$PWD/mask.h \
    $$PWD/migrations.h \
    $$PWD/organise.h \
    $$PWD/palette.h \
    $$PWD/query.h \
    $$PWD/scanner.h \
    $$PWD/tagreader.h \
    $$PWD/tagwriter.h \
    $$PWD/volumes.h \
    $$PWD/watcher.h

SOURCES += \
    $$PWD/artwork.cpp \
    $$PWD/audit.cpp \
    $$PWD/duplicates.cpp \
    $$PWD/job.cpp \
    $$PWD/library.cpp \
    $$PWD/mask.cpp \
    $$PWD/migrations.cpp \
    $$PWD/organise.cpp \
    $$PWD/palette.cpp \
    $$PWD/query.cpp \
    $$PWD/scanner.cpp \
    $$PWD/tagreader.cpp \
    $$PWD/tagwriter.cpp \
    $$PWD/volumes.cpp \
    $$PWD/watcher.cpp
