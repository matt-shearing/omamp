QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = omamp
TEMPLATE = app

include(src/core/core.pri)
include(src/engine/engine.pri)
include(src/cli/cli.pri)

INCLUDEPATH += src/app

HEADERS += \
    src/app/mpris.h \
    src/app/portal.h \
    src/app/theme.h \
    src/app/translator.h

SOURCES += \
    src/app/main.cpp \
    src/app/mpris.cpp \
    src/app/portal.cpp \
    src/app/theme.cpp \
    src/app/translator.cpp

RESOURCES += src/app/resources.qrc
