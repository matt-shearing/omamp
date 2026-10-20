// Runs every test class in one binary, offscreen. Each class prints its own
// Totals line; the exit code is non-zero if any of them failed.
//
// OMAMP_TESTS=query,palette runs only the named classes (player, library,
// query, palette, watcher, window, mask, tidy, playlists, engine, web, tidywindow, nas, subsonic, p3window, p45window, streaming, sync, syncwindow, nowplaying, radio), for a quick loop or a machine that must stay silent.
#include <QGuiApplication>
#include <QQmlEngine>
#include <QtTest>

#include "visualiser.h"

int runPlayerTests(int argc, char **argv);
int runLibraryTests(int argc, char **argv);
int runQueryTests(int argc, char **argv);
int runPaletteTests(int argc, char **argv);
int runWatcherTests(int argc, char **argv);
int runLibraryWindowTests(int argc, char **argv);
int runMaskTests(int argc, char **argv);
int runTidyTests(int argc, char **argv);
int runPlaylistTests(int argc, char **argv);
int runEngineTests(int argc, char **argv);
int runWebTests(int argc, char **argv);
int runTidyWindowTests(int argc, char **argv);
int runNasTests(int argc, char **argv);
int runSubsonicTests(int argc, char **argv);
int runP3WindowTests(int argc, char **argv);
int runP45WindowTests(int argc, char **argv);
int runStreamingTests(int argc, char **argv);
int runSyncTests(int argc, char **argv);
int runSyncWindowTests(int argc, char **argv);
int runNowPlayingTests(int argc, char **argv);
int runRadioTests(int argc, char **argv);

int main(int argc, char **argv) {
    // Never on anyone's screen, never on the GPU.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    // Never out of the speakers: the native engine's device sink aborts and
    // the Qt engine leaves out its audio output.
    qputenv("OMAMP_NO_AUDIO_DEVICE", "1");
    // Never in the desktop keyring: remote source secrets go to a file under
    // the temporary XDG_CONFIG_HOME the remote tests set.
    qputenv("OMAMP_SECRET_STORE", "file");
    QGuiApplication app(argc, argv);
    // The visualiser, as main() registers it for the app.
    qmlRegisterType<Visualiser>("Omamp", 1, 0, "Visualiser");
    const QStringList only = qEnvironmentVariable("OMAMP_TESTS").split(',', Qt::SkipEmptyParts);
    auto wanted = [&](const char *name) { return only.isEmpty() || only.contains(QLatin1String(name)); };
    int failures = 0;
    if (wanted("player"))
        failures += runPlayerTests(argc, argv);
    if (wanted("library"))
        failures += runLibraryTests(argc, argv);
    if (wanted("query"))
        failures += runQueryTests(argc, argv);
    if (wanted("palette"))
        failures += runPaletteTests(argc, argv);
    if (wanted("watcher"))
        failures += runWatcherTests(argc, argv);
    if (wanted("window"))
        failures += runLibraryWindowTests(argc, argv);
    if (wanted("mask"))
        failures += runMaskTests(argc, argv);
    if (wanted("tidy"))
        failures += runTidyTests(argc, argv);
    if (wanted("playlists"))
        failures += runPlaylistTests(argc, argv);
    if (wanted("engine"))
        failures += runEngineTests(argc, argv);
    if (wanted("web"))
        failures += runWebTests(argc, argv);
    if (wanted("tidywindow"))
        failures += runTidyWindowTests(argc, argv);
    if (wanted("nas"))
        failures += runNasTests(argc, argv);
    if (wanted("subsonic"))
        failures += runSubsonicTests(argc, argv);
    if (wanted("p3window"))
        failures += runP3WindowTests(argc, argv);
    if (wanted("p45window"))
        failures += runP45WindowTests(argc, argv);
    if (wanted("streaming"))
        failures += runStreamingTests(argc, argv);
    if (wanted("sync"))
        failures += runSyncTests(argc, argv);
    if (wanted("syncwindow"))
        failures += runSyncWindowTests(argc, argv);
    if (wanted("nowplaying"))
        failures += runNowPlayingTests(argc, argv);
    if (wanted("radio"))
        failures += runRadioTests(argc, argv);
    return failures ? 1 : 0;
}
