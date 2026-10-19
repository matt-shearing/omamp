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
    src/app/duplicatesmodel.h \
    src/app/librarycontroller.h \
    src/app/librarytree.h \
    src/app/mpris.h \
    src/app/organisemodel.h \
    src/app/portal.h \
    src/app/tageditor.h \
    src/app/tagfromname.h \
    src/app/theme.h \
    src/app/tracklistmodel.h \
    src/app/translator.h \
    src/app/djcontroller.h \
    src/app/playlistitemsmodel.h \
    src/app/playlistscontroller.h \
    src/app/playrecorder.h \
    src/app/rulebuilder.h \
    src/app/settingscontroller.h \
    src/app/streamresolver.h

SOURCES += \
    src/app/albumgridmodel.cpp \
    src/app/artprovider.cpp \
    src/app/columnbrowser.cpp \
    src/app/devhooks.cpp \
    src/app/duplicatesmodel.cpp \
    src/app/librarycontroller.cpp \
    src/app/librarycontroller_tidy.cpp \
    src/app/librarytree.cpp \
    src/app/main.cpp \
    src/app/mpris.cpp \
    src/app/organisemodel.cpp \
    src/app/portal.cpp \
    src/app/tageditor.cpp \
    src/app/tagfromname.cpp \
    src/app/theme.cpp \
    src/app/tracklistmodel.cpp \
    src/app/translator.cpp \
    src/app/djcontroller.cpp \
    src/app/playlistitemsmodel.cpp \
    src/app/playlistscontroller.cpp \
    src/app/playrecorder.cpp \
    src/app/rulebuilder.cpp \
    src/app/settingscontroller.cpp \
    src/app/streamresolver.cpp

RESOURCES += src/app/resources.qrc
