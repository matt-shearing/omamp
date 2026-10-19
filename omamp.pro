QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = omamp
TEMPLATE = app

include(src/core/core.pri)
include(src/engine/engine.pri)
include(src/cli/cli.pri)

INCLUDEPATH += src/app

HEADERS += \
    src/app/albumgridmodel.h \
    src/app/artprovider.h \
    src/app/columnbrowser.h \
    src/app/devhooks.h \
    src/app/librarycontroller.h \
    src/app/librarytree.h \
    src/app/mpris.h \
    src/app/portal.h \
    src/app/theme.h \
    src/app/tracklistmodel.h \
    src/app/translator.h

SOURCES += \
    src/app/albumgridmodel.cpp \
    src/app/artprovider.cpp \
    src/app/columnbrowser.cpp \
    src/app/devhooks.cpp \
    src/app/librarycontroller.cpp \
    src/app/librarytree.cpp \
    src/app/main.cpp \
    src/app/mpris.cpp \
    src/app/portal.cpp \
    src/app/theme.cpp \
    src/app/tracklistmodel.cpp \
    src/app/translator.cpp

RESOURCES += src/app/resources.qrc
