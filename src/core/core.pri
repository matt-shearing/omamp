# The library core: database, volumes, scanner, query language, jobs.
# Qt Core and Qt SQL only, so the app, the CLI and the tests share it
# without pulling in a window or audio.
QT += sql
CONFIG += link_pkgconfig
PKGCONFIG += taglib
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/job.h \
    $$PWD/library.h \
    $$PWD/migrations.h \
    $$PWD/palette.h \
    $$PWD/query.h \
    $$PWD/scanner.h \
    $$PWD/tagreader.h \
    $$PWD/volumes.h

SOURCES += \
    $$PWD/job.cpp \
    $$PWD/library.cpp \
    $$PWD/migrations.cpp \
    $$PWD/palette.cpp \
    $$PWD/query.cpp \
    $$PWD/scanner.cpp \
    $$PWD/tagreader.cpp \
    $$PWD/volumes.cpp
