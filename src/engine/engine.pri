# Playback: the play queue, the analyser and two engines behind one
# interface (audioengine.h). QtEngine is QMediaPlayer; NativeEngine decodes
# with FFmpeg's libraries into one QAudioSink, for gapless playback,
# crossfade, ReplayGain and the equaliser. Linking FFmpeg is a deliberate
# exception to Omarchy's "call the ffmpeg command" rule: playback can't
# work through a subprocess.
QT += multimedia
CONFIG += link_pkgconfig
PKGCONFIG += libavformat libavcodec libswresample libavutil
INCLUDEPATH += $$PWD

HEADERS += $$PWD/audioengine.h $$PWD/decoder.h $$PWD/equalizer.h $$PWD/nativeengine.h $$PWD/outputfollower.h $$PWD/pcmsink.h \
           $$PWD/player.h $$PWD/playlist.h $$PWD/handoffengine.h $$PWD/qtengine.h $$PWD/spectrum.h $$PWD/spotifyaudio.h $$PWD/streaming.h
SOURCES += $$PWD/decoder.cpp $$PWD/equalizer.cpp $$PWD/nativeengine.cpp $$PWD/outputfollower.cpp $$PWD/pcmsink.cpp \
           $$PWD/player.cpp $$PWD/playlist.cpp $$PWD/handoffengine.cpp $$PWD/qtengine.cpp $$PWD/spectrum.cpp $$PWD/spotifyaudio.cpp $$PWD/streaming.cpp
