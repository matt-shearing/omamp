// Omamp — a Winamp-style music player that wears the Omarchy theme.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>

#include "cli.h"
#include "mpris.h"
#include "player.h"
#include "portal.h"
#include "theme.h"
#include "translator.h"

static const char *version = "0.1.0";

int main(int argc, char *argv[]) {
    // Commands run without a display: a QCoreApplication never touches the
    // GUI platform, so `omamp scan` works over SSH and in scripts.
    if (argc > 1 && Cli::isCommand(QString::fromLocal8Bit(argv[1]))) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("omamp");
        app.setApplicationVersion(version);
        QTextStream out(stdout), err(stderr);
        return Cli::run(app.arguments().mid(1), out, err);
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("omamp");
    app.setApplicationVersion(version);

    // Matches the window to omamp.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("omamp");
    app.setWindowIcon(QIcon::fromTheme("omamp"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    FolderPicker picker;
    Player player(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omamp/state.json");
    QObject::connect(&player, &Player::folderRequested, &picker, &FolderPicker::pickFolders);
    QObject::connect(&picker, &FolderPicker::picked, &player, &Player::addPicked);
    QObject::connect(&picker, &FolderPicker::failed, &player, &Player::reportError);

    // Files and folders on the command line (or from "Open with") are added
    // and the first of them starts playing. A first run with nothing to play
    // offers the Music folder.
    QStringList arguments = app.arguments().mid(1);
    if (arguments.value(0) == "open")
        arguments.removeFirst();
    if (!arguments.isEmpty()) {
        const int before = player.playlist()->count();
        if (player.addPaths(arguments) > 0)
            player.playIndex(before);
    } else if (player.playlist()->count() == 0) {
        const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        if (QFileInfo(music).isDir())
            player.addPaths({music});
    }

    QObject mprisHost;
    registerMpris(&mprisHost, &player);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &player);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (auto *root = mprisHost.findChild<MprisRoot *>()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QObject::connect(root, &MprisRoot::raiseRequested, window, [window] {
            window->show();
            window->raise();
            window->requestActivate();
        });
    }

    return app.exec();
}
