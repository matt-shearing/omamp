# The command line: scan, query, stats. Needs only the core.
INCLUDEPATH += $$PWD
HEADERS += $$PWD/cli.h $$PWD/webcli.h
SOURCES += $$PWD/cli.cpp $$PWD/webcli.cpp $$PWD/clinas.cpp $$PWD/clisync.cpp $$PWD/clispotify.cpp $$PWD/cliradio.cpp
QT += dbus
