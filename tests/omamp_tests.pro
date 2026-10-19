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

HEADERS += ../src/app/portal.h ../src/app/theme.h ../src/app/translator.h \
           ../src/app/librarycontroller.h ../src/app/librarytree.h ../src/app/tracklistmodel.h
SOURCES += main.cpp omamp_tests.cpp library_tests.cpp librarywindow_tests.cpp \
           ../src/app/portal.cpp ../src/app/theme.cpp ../src/app/translator.cpp \
           ../src/app/librarycontroller.cpp ../src/app/librarytree.cpp ../src/app/tracklistmodel.cpp
