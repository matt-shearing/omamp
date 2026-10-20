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

HEADERS += mockhttp.h audiohttp.h spotify_fakes.h ../src/app/artprovider.h ../src/app/portal.h ../src/app/theme.h ../src/app/translator.h fixture_library.h \
           ../src/app/librarycontroller.h ../src/app/librarytree.h ../src/app/tracklistmodel.h \
           ../src/app/columnbrowser.h ../src/app/albumgridmodel.h \
           ../src/app/tageditor.h ../src/app/organisemodel.h ../src/app/tagfromname.h ../src/app/duplicatesmodel.h \
           ../src/app/djcontroller.h ../src/app/playlistitemsmodel.h ../src/app/playlistscontroller.h ../src/app/playrecorder.h ../src/app/rulebuilder.h ../src/app/settingscontroller.h \
           ../src/app/webcontroller.h ../src/app/nascontroller.h ../src/app/devicescontroller.h mocksubsonic.h ../src/app/streamresolver.h \
           ../src/app/appearance.h ../src/app/nowplaying.h ../src/app/visualiser.h ../src/app/mpris.h nowplaying_context.h ../src/app/spotifycontroller.h ../src/app/radiocontroller.h radiohttp.h ../src/app/windowplacement.h
SOURCES += main.cpp omamp_tests.cpp library_tests.cpp query_tests.cpp palette_tests.cpp watcher_tests.cpp \
           librarywindow_tests.cpp mask_tests.cpp tidy_tests.cpp playlist_tests.cpp engine_tests.cpp web_tests.cpp tidywindow_tests.cpp \
           nas_tests.cpp subsonic_tests.cpp p3window_tests.cpp p45window_tests.cpp streaming_tests.cpp sync_tests.cpp nowplaying_tests.cpp radio_tests.cpp \
           ../src/app/artprovider.cpp \
           ../src/app/portal.cpp ../src/app/theme.cpp ../src/app/translator.cpp \
           ../src/app/librarycontroller.cpp ../src/app/librarytree.cpp ../src/app/tracklistmodel.cpp \
           ../src/app/columnbrowser.cpp ../src/app/albumgridmodel.cpp \
           ../src/app/librarycontroller_tidy.cpp ../src/app/tageditor.cpp ../src/app/organisemodel.cpp \
           ../src/app/tagfromname.cpp ../src/app/duplicatesmodel.cpp \
           ../src/app/djcontroller.cpp ../src/app/playlistitemsmodel.cpp ../src/app/playlistscontroller.cpp ../src/app/playrecorder.cpp ../src/app/rulebuilder.cpp ../src/app/settingscontroller.cpp \
           ../src/app/webcontroller.cpp ../src/app/nascontroller.cpp ../src/app/streamresolver.cpp \
           ../src/app/devicescontroller.cpp ../src/app/librarycontroller_reports.cpp \
           ../src/app/appearance.cpp ../src/app/nowplaying.cpp ../src/app/visualiser.cpp ../src/app/mpris.cpp \
           ../src/app/spotifycontroller.cpp spotify_tests.cpp mixed_tests.cpp \
           ../src/app/radiocontroller.cpp ../src/app/windowplacement.cpp menu_tests.cpp

# The real window, for the keyboard test of advanced mode.
RESOURCES += ../src/app/resources.qrc
