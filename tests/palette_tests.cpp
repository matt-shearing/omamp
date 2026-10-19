// The command palette: its ranking, its actions, and the QML popup driven
// offscreen from the keyboard, with a window that has Omamp's own one-key
// shortcuts to prove the popup keeps them from firing while typing.

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "library.h"
#include "palette.h"
#include "theme.h"

#include "fixture_library.h"

namespace {

QStringList column(const Palette &p, int role) {
    QStringList out;
    for (int i = 0; i < p.rowCount(); ++i)
        out << p.data(p.index(i), role).toString();
    return out;
}

// Every row as "kind:title", in order.
QStringList rows(const Palette &p) {
    QStringList out;
    for (int i = 0; i < p.rowCount(); ++i)
        out << p.data(p.index(i), Palette::KindRole).toString() + ':' + p.data(p.index(i), Palette::TitleRole).toString();
    return out;
}

int rowOf(const Palette &p, const QString &kindTitle) { return int(rows(p).indexOf(kindTitle)); }

// QTest::keyClicks is for widgets only; this types into a window.
void typeText(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

// What the window registers, as Main.qml would.
void addActions(Palette &p) {
    p.addAction("play-pause", "Play/Pause", {"pause", "resume", "start"}, "Space");
    p.addAction("next", "Next track", {"skip", "forward"}, "B");
    p.addAction("previous", "Previous track", {"back"}, "Z");
    p.addAction("stop", "Stop", {}, "V");
    p.addAction("shuffle", "Shuffle", {"random"}, "S");
    p.addAction("repeat", "Repeat", {"loop"}, "R");
    p.addAction("add-folder", "Add folder…", {"open", "import", "music"}, "O");
    p.addAction("rescan", "Rescan library", {"scan", "refresh", "update"}, "");
    p.addAction("advanced", "Toggle advanced mode", {"library", "manager", "simple"}, "Ctrl+M");
    p.addAction("shortcuts", "Show keyboard shortcuts", {"help", "keys"}, "?");
    p.addAction("quit", "Quit", {"exit", "close"}, "Q");
}

} // namespace

class PaletteTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Library *m_library = nullptr;

private slots:
    void initTestCase() {
        m_library = new Library(m_dir.filePath("library.db"));
        QVERIFY(m_library->open());
        QVERIFY(Fixture::fill(*m_library));
    }
    void cleanupTestCase() { delete m_library; }

    void scoresMatches() {
        QCOMPARE(Palette::score("coast", "Coast"), 100.0);
        QCOMPARE(Palette::score("bjork", "Björk"), 100.0);           // accents and case ignored
        QCOMPARE(Palette::score("BJÖRK", "bjork"), 100.0);
        const double prefix = Palette::score("coast", "Coastline");
        const double wordStart = Palette::score("coast", "Neon Coast");
        const double inside = Palette::score("oast", "Neon Coast");
        const double scattered = Palette::score("drive neon", "Neon Coast Night Drive");
        QVERIFY(prefix > wordStart);
        QVERIFY(wordStart > inside);
        QVERIFY(scattered >= wordStart - 10 && scattered > inside);
        QVERIFY(Palette::score("coast drive", "Neon Coast") == 0);   // every word must match
        QCOMPARE(Palette::score("", "x"), 0.0);
        QCOMPARE(Palette::score("x", ""), 0.0);

        // Fuzzy only where asked, and only from a word start.
        QCOMPARE(Palette::score("pp", "Play/Pause"), 0.0);
        QVERIFY(Palette::score("pp", "Play/Pause", true) > 0);
        QVERIFY(Palette::score("tam", "Toggle advanced mode", true) > 0);
        QVERIFY(Palette::score("tam", "Toggle advanced mode", true) > Palette::score("tgl", "Toggle advanced mode", true));
        QCOMPARE(Palette::score("lp", "Play/Pause", true), 0.0);    // starts mid-word
        QCOMPARE(Palette::score("zz", "Play/Pause", true), 0.0);
        QVERIFY(Palette::score("pp", "Play/Pause", true) < Palette::score("play", "Play/Pause", true));
    }

    void listsAndFindsActions() {
        Palette p;
        addActions(p);
        QCOMPARE(p.count(), 11);
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Play/Pause")); // registration order
        QCOMPARE(column(p, Palette::ShortcutRole).first(), QString("Space"));

        p.setText("pp");
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Play/Pause"));
        p.setText("rescan");
        QCOMPARE(column(p, Palette::TitleRole), QStringList{"Rescan library"});
        p.setText("pause");                                      // a keyword
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Play/Pause"));
        p.setText("adv");
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Toggle advanced mode"));
        p.setText("tam");
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Toggle advanced mode"));
        p.setText("folder");
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Add folder…"));
        p.setText("xyzzy");
        QCOMPARE(p.count(), 0); // no library, so no "search the library" row
        QVERIFY(p.lastSearchMs() >= 0);

        // At most six actions for a search; ">" lists every match.
        p.setText("e");
        QCOMPARE(p.count(), 6);
        p.setText(">e");
        QVERIFY(p.count() > 6);
        QCOMPARE(column(p, Palette::KindRole).filter("action").size(), p.count());

        // Activating runs the action and moves it to the top of the empty list.
        QSignalSpy triggered(&p, &Palette::actionTriggered);
        QSignalSpy activated(&p, &Palette::activated);
        p.setText("next");
        QVERIFY(p.activate(0));
        QCOMPARE(triggered.size(), 1);
        QCOMPARE(triggered[0][0].toString(), QString("next"));
        QCOMPARE(activated[0][0].toMap()["itemId"].toString(), QString("next"));
        QCOMPARE(activated[0][0].toMap()["kind"].toString(), QString("action"));
        p.setText("");
        QCOMPARE(column(p, Palette::TitleRole).first(), QString("Next track"));
        QVERIFY(!p.activate(-1));
        QVERIFY(!p.activate(99));

        // Disabled actions are listed but do nothing.
        p.setActionEnabled("rescan", false);
        p.setText("rescan");
        QCOMPARE(p.count(), 1);
        QCOMPARE(p.data(p.index(0), Palette::EnabledRole).toBool(), false);
        QVERIFY(!p.activate(0));
        QCOMPARE(triggered.size(), 1);
        p.setActionEnabled("rescan", true);
        QVERIFY(p.activate(0));

        // Adding an id again updates it; removing takes it away.
        p.addAction("rescan", "Rescan every folder", {}, "F5");
        QCOMPARE(column(p, Palette::TitleRole), QStringList{"Rescan every folder"});
        QCOMPARE(p.actionIds().size(), 11);
        p.removeAction("rescan");
        QCOMPARE(p.count(), 0);
        QCOMPARE(p.actionIds().size(), 10);
        QCOMPARE(p.get(5), QVariantMap());
    }

    void mixesLibraryResults() {
        Palette p;
        addActions(p);
        p.setLibrary(m_library);

        p.setText("coast");
        QStringList r = rows(p);
        QVERIFY2(r.contains("artist:Neon Coast"), qPrintable(r.join(" | ")));
        QVERIFY2(r.contains("album:Night Drive"), qPrintable(r.join(" | ")));
        QVERIFY(r.contains("track:Coastline"));
        QVERIFY(r.contains("track:Midnight Highway")); // by its artist
        QCOMPARE(r.first(), QString("track:Coastline"));   // a title that starts with the text
        QCOMPARE(r.last(), QString("search:Search the library for “coast”"));
        QVERIFY(rowOf(p, "artist:Neon Coast") < rowOf(p, "track:Midnight Highway"));
        QCOMPARE(p.data(p.index(rowOf(p, "artist:Neon Coast")), Palette::DetailRole).toString(), QString("3 tracks"));
        QCOMPARE(p.data(p.index(rowOf(p, "album:Night Drive")), Palette::SubtitleRole).toString(), QString("Neon Coast"));
        QCOMPARE(p.data(p.index(rowOf(p, "album:Night Drive")), Palette::DetailRole).toString(), QString());
        const int coastline = rowOf(p, "track:Coastline");
        QCOMPARE(p.data(p.index(coastline), Palette::SubtitleRole).toString(), QString("Neon Coast · Night Drive"));
        QCOMPARE(p.data(p.index(coastline), Palette::DetailRole).toString(), QString("3:05"));

        p.setText("bjork");
        r = rows(p);
        QVERIFY(r.contains("artist:Björk Tribute"));
        QVERIFY(r.contains("track:Tidal"));

        p.setText("neon night drive");
        QCOMPARE(rows(p).first(), QString("album:Night Drive"));

        // Two letters: names, but no tracks (they would need a full scan).
        p.setText("ne");
        r = rows(p);
        QVERIFY(r.contains("artist:Neon Coast"));
        QVERIFY(!r.join(' ').contains("track:"));

        // Actions and library together.
        p.setText("re");
        r = rows(p);
        QVERIFY(r.contains("action:Rescan library"));
        QVERIFY(r.contains("artist:Reader"));

        // The query language lists the tracks a query selects, in its order.
        p.setText("artist:neon sort:-length");
        QCOMPARE(rows(p).mid(0, 3), (QStringList{"track:Afterglow", "track:Midnight Highway", "track:Coastline"}));
        QVERIFY(rows(p).filter(QRegularExpression("^artist:")).isEmpty());
        p.setText("played:never type:music sort:title limit:1");
        QCOMPARE(rows(p).first(), QString("track:Afterglow"));
        // Unfinished query text does no harm.
        for (const QString &text : {"artist:", "(neon", "title:/(/", "\"neo", "-", "+", "sort:x"}) {
            p.setText(text);
            QVERIFY(rows(p).last().startsWith("search:"));
        }
        // The untitled track shows its file name.
        p.setText("untagged");
        QVERIFY(rows(p).contains("track:untagged-file"));
    }

    void activatesLibraryRows() {
        Palette p;
        p.setLibrary(m_library);
        QSignalSpy artist(&p, &Palette::artistActivated), album(&p, &Palette::albumActivated),
            track(&p, &Palette::trackActivated), search(&p, &Palette::searchRequested), any(&p, &Palette::activated);
        p.setText("coast");
        QVERIFY(p.activate(rowOf(p, "artist:Neon Coast")));
        QCOMPARE(artist.size(), 1);
        QCOMPARE(artist[0][1].toString(), QString("Neon Coast"));
        QVERIFY(artist[0][0].toLongLong() > 0);
        QVERIFY(p.activate(rowOf(p, "album:Night Drive")));
        QCOMPARE(album[0][1].toString(), QString("Night Drive"));
        QCOMPARE(album[0][2].toString(), QString("Neon Coast"));
        QVERIFY(p.activate(rowOf(p, "track:Coastline")));
        QSqlQuery q(m_library->db());
        q.exec("SELECT id FROM tracks WHERE title = 'Coastline'");
        QVERIFY(q.next());
        QCOMPARE(track[0][0].toLongLong(), q.value(0).toLongLong());
        QVERIFY(p.activate(p.count() - 1));
        QCOMPARE(search[0][0].toString(), QString("coast"));
        QCOMPARE(any.size(), 4);
        QCOMPARE(any[3][0].toMap()["kind"].toString(), QString("search"));
    }

    void followsLibraryChanges() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QVERIFY(Fixture::fill(library));
        Palette p;
        p.setLibrary(&library);
        p.setText("zephyr");
        QCOMPARE(p.count(), 1); // only the search row
        QSqlQuery q(library.db());
        QVERIFY(q.exec("INSERT INTO artists (name) VALUES ('Zephyr Lane')"));
        QVERIFY(q.exec("INSERT INTO tracks (volume_id, rel_path, file_name, size, mtime_ns, title, artist, added) "
                       "VALUES (1, 'z.flac', 'z', 1, 0, 'Zephyr Song', 'Zephyr Lane', 0)"));
        QVERIFY(q.exec("INSERT INTO track_artists (track_id, artist_id, role) VALUES "
                       "((SELECT max(id) FROM tracks), (SELECT id FROM artists WHERE name = 'Zephyr Lane'), 'artist')"));
        p.refresh();
        QVERIFY(rows(p).contains("artist:Zephyr Lane"));
        QVERIFY(rows(p).contains("track:Zephyr Song"));
        p.setLibrary(nullptr);
        QCOMPARE(p.count(), 0);
    }

    // ---- The popup ------------------------------------------------------------

    void popupWorksFromTheKeyboard() {
        Palette model;
        addActions(model);
        model.setLibrary(m_library);

        QQmlEngine engine;
        QVariantMap theme{{"c", Theme::defaults()},
                          {"accent", Theme::defaults().value("accent")},
                          {"accentForeground", "black"},
                          {"listSelection", Theme::defaults().value("selection")},
                          {"light", false},
                          {"radius", 0}};
        engine.rootContext()->setContextProperty("theme", theme);
        engine.rootContext()->setContextProperty("paletteModel", &model);

        // A window with Omamp's one-key shortcuts, as Main.qml has them.
        const QByteArray qml = R"(
            import QtQuick
            import QtQuick.Controls
            import QtQuick.Controls.Material
            ApplicationWindow {
                width: 760; height: 560; visible: true
                color: theme.c.background
                Material.theme: Material.Dark
                Material.accent: theme.accent
                property int windowKeys: 0
                property alias commandPalette: palette
                Shortcut { sequences: ["Up", "Down", "Return", "Space", "Q", "Z", "Escape"]
                           context: Qt.ApplicationShortcut; onActivated: windowKeys++ }
                Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; onActivated: palette.toggle() }
                Text { x: 24; y: 24; text: "Omamp"; color: theme.c.muted; font.pixelSize: 28 }
                CommandPalette { id: palette; model: paletteModel }
            }
        )";
        QQmlComponent component(&engine);
        component.setData(qml, QUrl::fromLocalFile(QString(OMAMP_APP_DIR) + "/PaletteHarness.qml"));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *popup = object->property("commandPalette").value<QObject *>();
        QVERIFY(popup);

        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(popup->property("opened").toBool());
        typeText(window, "coast");
        QCOMPARE(model.text(), QString("coast"));
        QCOMPARE(object->property("windowKeys").toInt(), 0);

        // Down twice, then Enter, chooses the third row.
        const QString third = rows(model).value(2);
        QSignalSpy chosen(popup, SIGNAL(chosen(QVariant)));
        QSignalSpy activated(&model, &Palette::activated);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Up);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        const QVariantMap item = activated[0][0].toMap();
        QCOMPARE(item["kind"].toString() + ':' + item["title"].toString(), third);
        QCOMPARE(chosen.size(), 1);
        QTRY_VERIFY(!popup->property("opened").toBool());
        QCOMPARE(object->property("windowKeys").toInt(), 0);

        // Typing keeps the window's single-letter keys from firing, and Esc closes.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(popup->property("opened").toBool());
        QCOMPARE(model.text(), QString()); // opening starts afresh
        typeText(window, "q z");
        QCOMPARE(model.text(), QString("q z"));
        QCOMPARE(object->property("windowKeys").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("opened").toBool());
        QCOMPARE(object->property("windowKeys").toInt(), 0);

        // Ctrl+K toggles it closed too; ">" and an action from the keyboard.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(popup->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(!popup->property("opened").toBool());
        QSignalSpy triggered(&model, &Palette::actionTriggered);
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(popup->property("opened").toBool());
        typeText(window, ">adv");
        QTest::keyClick(window, Qt::Key_Enter);
        QCOMPARE(triggered.size(), 1);
        QCOMPARE(triggered[0][0].toString(), QString("advanced"));
        // Enter on nothing does nothing.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(popup->property("opened").toBool());
        typeText(window, ">xyzzy");
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(popup->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("opened").toBool());

        // Screenshots for the docs: OMAMP_SCREENSHOTS=<folder>.
        const QString shots = qEnvironmentVariable("OMAMP_SCREENSHOTS");
        if (!shots.isEmpty()) {
            QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
            QTRY_VERIFY(popup->property("opened").toBool());
            typeText(window, "coast");
            QTest::keyClick(window, Qt::Key_Down);
            QTest::qWait(50);
            QVERIFY(window->grabWindow().save(shots + "/command-palette.png"));
            QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClick(window, Qt::Key_Backspace);
            QTest::qWait(50);
            QVERIFY(window->grabWindow().save(shots + "/command-palette-actions.png"));
            QTest::keyClick(window, Qt::Key_Escape);
        }
    }
};

int runPaletteTests(int argc, char **argv) {
    PaletteTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "palette_tests.moc"
