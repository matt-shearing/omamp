QT += core gui quick quickcontrols2 testlib dbus multimedia
CONFIG += c++17 testcase
TARGET = omamp_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/player.h ../src/playlist.h ../src/spectrum.h ../src/portal.h ../src/theme.h ../src/translator.h
SOURCES += omamp_tests.cpp ../src/player.cpp ../src/playlist.cpp ../src/spectrum.cpp ../src/portal.cpp \
           ../src/theme.cpp ../src/translator.cpp
