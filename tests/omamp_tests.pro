QT += core gui quick quickcontrols2 testlib dbus
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

HEADERS += ../src/app/artprovider.h ../src/app/portal.h ../src/app/theme.h ../src/app/translator.h fixture_library.h
SOURCES += main.cpp omamp_tests.cpp library_tests.cpp query_tests.cpp palette_tests.cpp watcher_tests.cpp \
           ../src/app/artprovider.cpp \
           ../src/app/portal.cpp ../src/app/theme.cpp ../src/app/translator.cpp
