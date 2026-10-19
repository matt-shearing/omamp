// Runs every test class in one binary, offscreen. Each class prints its own
// Totals line; the exit code is non-zero if any of them failed.
#include <QGuiApplication>
#include <QtTest>

int runPlayerTests(int argc, char **argv);
int runLibraryTests(int argc, char **argv);

int main(int argc, char **argv) {
    // Never on anyone's screen, never on the GPU.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    int failures = 0;
    failures += runPlayerTests(argc, argv);
    failures += runLibraryTests(argc, argv);
    return failures ? 1 : 0;
}
