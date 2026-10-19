QT += core gui qml quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = omamp_tests
TEMPLATE = app

include(../src/core/core.pri)
include(../src/engine/engine.pri)
include(../src/cli/cli.pri)
INCLUDEPATH += ../src/app

# Where make-fixtures lives, so the tests can build their audio files.
DEFINES += OMAMP_TESTS_DIR=\\\"$$PWD\\\"
# Where the QML lives, so the palette test can load CommandPalette.qml.
DEFINES += OMAMP_APP_DIR=\\\"$$PWD/../src/app\\\"

HEADERS += ../src/app/artprovider.h ../src/app/portal.h ../src/app/theme.h ../src/app/translator.h fixture_library.h \
           ../src/app/librarycontroller.h ../src/app/librarytree.h ../src/app/tracklistmodel.h \
           ../src/app/columnbrowser.h ../src/app/albumgridmodel.h
SOURCES += main.cpp omamp_tests.cpp library_tests.cpp query_tests.cpp palette_tests.cpp watcher_tests.cpp \
           librarywindow_tests.cpp mask_tests.cpp tidy_tests.cpp engine_tests.cpp \
           ../src/app/artprovider.cpp \
           ../src/app/portal.cpp ../src/app/theme.cpp ../src/app/translator.cpp \
           ../src/app/librarycontroller.cpp ../src/app/librarytree.cpp ../src/app/tracklistmodel.cpp \
           ../src/app/columnbrowser.cpp ../src/app/albumgridmodel.cpp

# The real window, for the keyboard test of advanced mode.
RESOURCES += ../src/app/resources.qrc
