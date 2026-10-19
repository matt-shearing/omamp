// Runs every test class in one binary, offscreen. Each class prints its own
// Totals line; the exit code is non-zero if any of them failed.
//
// OMAMP_TESTS=query,palette runs only the named classes (player, library,
// query, palette), for a quick loop or a machine that must stay silent.
#include <QGuiApplication>
#include <QtTest>

int runPlayerTests(int argc, char **argv);
int runLibraryTests(int argc, char **argv);
int runQueryTests(int argc, char **argv);
int runPaletteTests(int argc, char **argv);
int runWatcherTests(int argc, char **argv);
int runLibraryWindowTests(int argc, char **argv);

int main(int argc, char **argv) {
    // Never on anyone's screen, never on the GPU.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
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
    return failures ? 1 : 0;
}
