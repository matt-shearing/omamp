QT += core gui qml quick quickcontrols2 dbus multimedia

CONFIG += c++17 release
TARGET = omamp
TEMPLATE = app

HEADERS += \
    src/player.h \
    src/playlist.h \
    src/spectrum.h \
    src/mpris.h \
    src/portal.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/player.cpp \
    src/playlist.cpp \
    src/spectrum.cpp \
    src/mpris.cpp \
    src/portal.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
