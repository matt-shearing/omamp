// Phase 2: the mask language (organise and tag from file names). Pure
// string work, apart from one test that reads a track's values from a
// temporary library.

#include <QTemporaryDir>
#include <QtTest>

#include "fixture_library.h"
#include "library.h"
#include "mask.h"

namespace {

Mask::Values sample() {
    Mask::Values v;
    v.fields = {{"artist", "Neon Coast; Lena Park"},
                {"albumartist", "Neon Coast"},
                {"album", "Night Drive"},
                {"title", "Midnight Highway"},
                {"genre", "Synthwave; Pop"},
                {"composer", "Ada Moreau"},
                {"year", "1997"},
                {"originalyear", "1995"},
                {"track", "1"},
                {"tracktotal", "12"},
                {"disc", "2"},
                {"bitrate", "320"},
                {"bpm", "118"},
                {"custom1", "vinyl"},
                {"type", "music"}};
    v.path = "/music/Neon Coast/Night Drive/CD2/01 midnight.flac";
    v.rootPath = "/music";
    return v;
}

QString eval(const QString &mask, const Mask::Values &v = sample(), Mask::Context c = {}) {
    QString error;
    const QString out = Mask::evaluate(mask, v, c, &error);
    if (!error.isEmpty())
        qWarning() << mask << error;
    return out;
}

QString text(const QString &mask, const Mask::Values &v = sample()) {
    Mask::Context c;
    c.pathMode = false;
    return eval(mask, v, c);
}

}

class MaskTests : public QObject {
    Q_OBJECT

private slots:
    void fieldsAndFolders() {
        QCOMPARE(eval("<Album Artist>/<Album> (<Year>)/<Disc#>-<Track#:2> <Title>"),
                 QString("Neon Coast/Night Drive (1997)/2-01 Midnight Highway"));
        // Backslashes are separators too, as in MediaMonkey.
        QCOMPARE(eval("<Album Artist>\\<Album>\\<Track#:3>"), QString("Neon Coast/Night Drive/001"));
        QCOMPARE(eval("<Genre> <Composer> <Original Date> <Bitrate> <BPM> <Custom1> <Type> <Track Total>"),
                 QString("Synthwave; Pop Ada Moreau 1995 320 118 vinyl music 12"));
        // Field names ignore case, spaces and underscores.
        QCOMPARE(eval("<album_artist>-<ALBUM ARTIST>-<albumartist>"), QString("Neon Coast-Neon Coast-Neon Coast"));
        // Text fields with :n are cut to n characters.
        QCOMPARE(eval("<Album:5>"), QString("Night"));
        // An empty number is not padded.
        Mask::Values v = sample();
        v.fields["track"].clear();
        QCOMPARE(eval("<Track#:2> <Title>", v), QString("Midnight Highway"));
    }

    void fileAndPathParts() {
        QCOMPARE(text("<Folder:1>|<Folder:2>|<Folder>|<Folder:9>"), QString("CD2|Night Drive|CD2|"));
        QCOMPARE(text("<Path:1>|<Path:3>"), QString("Neon Coast|CD2"));
        // <Path> keeps its folders.
        QCOMPARE(eval("<Path>/<Filename>.<Extension>"), QString("Neon Coast/Night Drive/CD2/01 midnight.flac"));
        Mask::Context c;
        c.autoNumber = 7;
        QCOMPARE(eval("<Auto Number:3> <Title>", sample(), c), QString("007 Midnight Highway"));
        QCOMPARE(eval("[<Auto Number>]"), QString("[]"));
    }

    void alphabeticalGroups() {
        Mask::Values v = sample();
        QCOMPARE(eval("<Artist@1>/<Artist>", v), QString("N/Neon Coast; Lena Park"));
        QCOMPARE(eval("<Album Artist@3>"), QString("MNO"));
        v.fields["artist"] = "Ólafur Arnalds";
        QCOMPARE(eval("<Artist@1>", v), QString("O"));
        v.fields["artist"] = "Zzz";
        QCOMPARE(eval("<Artist@5>", v), QString("Z"));
        v.fields["artist"] = "50 Cent";
        QCOMPARE(eval("<Artist@1>", v), QString("#"));
        v.fields["artist"] = "坂本 龍一";
        QCOMPARE(eval("<Artist@1>", v), QString("坂"));
    }

    void functions() {
        QCOMPARE(text("$If(<Year>,(<Year>),no year)"), QString("(1997)"));
        Mask::Values v = sample();
        v.fields["year"].clear();
        QCOMPARE(text("$If(<Year>,(<Year>),no year)", v), QString("no year"));
        QCOMPARE(text("$If(<Year>,yes)", v), QString());
        // Comparisons: text without case, numbers as numbers.
        QCOMPARE(text("$If(<Album Artist>=neon coast,same,different)"), QString("same"));
        QCOMPARE(text("$If(<Artist>=<Album Artist>,,<Artist> - )<Title>"),
                 QString("Neon Coast; Lena Park - Midnight Highway"));
        QCOMPARE(text("$If(<Track#>=01,one,other)"), QString("one"));
        QCOMPARE(text("$If(<Disc#>!=1,Disc <Disc#>,)"), QString("Disc 2"));
        QCOMPARE(text("$IfEmpty(<Composer>,anon)|$IfEmpty(<Custom2>,none)"), QString("Ada Moreau|none"));
        QCOMPARE(text("$First(<Artist>)"), QString("Neon Coast"));
        QCOMPARE(text("$First(<Custom2>,<Custom1>,x)"), QString("vinyl"));
        QCOMPARE(text("$Replace(<Title>,Midnight,Noon,Highway,Road)"), QString("Noon Road"));
        // Quotes keep commas and slashes literal inside arguments.
        QCOMPARE(text("$Replace(<Genre>,\"; \",\", \")"), QString("Synthwave, Pop"));
        v = sample();
        v.fields["title"] = "AC/DC Live";
        QCOMPARE(eval("$Replace(<Title>,\"/\",-)", v), QString("AC-DC Live"));
        v.fields["artist"] = "The Beatles";
        QCOMPARE(text("$RemovePrefix(<Artist>)|$MovePrefix(<Artist>)", v), QString("Beatles|Beatles, The"));
        v.fields["artist"] = "Los Lobos";
        QCOMPARE(text("$MovePrefix(<Artist>,The,Los)", v), QString("Lobos, Los"));
        v.fields["artist"] = "Theory";
        QCOMPARE(text("$MovePrefix(<Artist>)", v), QString("Theory"));
        QCOMPARE(text("$Left(<Title>,3)|$Right(<Title>,3)|$Mid(<Title>,3,5)|$Mid(<Title>,10)"),
                 QString("Mid|way|dnigh|Highway"));
        QCOMPARE(text("[$Trim(  a   b  )]"), QString("[a b]"));
        QCOMPARE(text("$Upper(<Album>)|$Lower(<Album>)"), QString("NIGHT DRIVE|night drive"));
        v.fields["title"] = "don't STOP me now";
        QCOMPARE(text("$Title(<Title>)", v), QString("Don't Stop Me Now"));
        QCOMPARE(text("$Len(<Album>)"), QString("11"));
        QCOMPARE(text("$Pad(<Track#>,3)|$Pad(<Disc#>,4,_)"), QString("001|___2"));
        // Nested functions and literal brackets inside an argument.
        QCOMPARE(text("$Upper($Left($MovePrefix(The Cure),4))"), QString("CURE"));
        QCOMPARE(text("$$5 <Year>"), QString("$5 1997"));
    }

    void functionsMakeFolders() {
        // A separator inside a function's result still makes a folder.
        QCOMPARE(eval("<Album>/$If(<Disc#>,CD<Disc#>/,)<Title>"), QString("Night Drive/CD2/Midnight Highway"));
        // In text mode a separator is just a slash.
        QCOMPARE(text("<Album>/<Title>"), QString("Night Drive/Midnight Highway"));
    }

    void sanitising() {
        Mask::Values v = sample();
        v.fields["albumartist"] = "AC/DC";
        v.fields["album"] = "Who Made Who?: \"Live\" <1986> | *";
        v.fields["title"] = "Etc... ";
        QCOMPARE(eval("<Album Artist>/<Album>/<Title>", v), QString("AC_DC/Who Made Who__ _Live_ _1986_ _ _/Etc"));
        // Unsafe characters are replaced with the chosen text.
        Mask::Context c;
        c.sanitise.replacement = "-";
        QCOMPARE(eval("<Album Artist>", v, c), QString("AC-DC"));
        // Linux alone allows these, but trailing dots and slashes still go.
        c.sanitise.windowsSafe = false;
        QCOMPARE(eval("<Album>", v, c), QString("Who Made Who?: \"Live\" <1986> | *"));
        // A leading dot would hide the file from the scanner.
        v.fields["albumartist"] = ".38 Special";
        QCOMPARE(eval("<Album Artist>", v), QString("_38 Special"));
        v.fields["title"] = "con";
        QCOMPARE(eval("<Title>", v), QString("con_"));
        v.fields["title"] = "..";
        QCOMPARE(eval("<Title>", v), QString("_"));
        // Control characters.
        v.fields["title"] = QString("a") + QChar(0x01) + "b";
        QCOMPARE(eval("<Title>", v), QString("a_b"));
        // An empty folder is dropped; an empty file name becomes Untitled.
        v = sample();
        v.fields["albumartist"].clear();
        QCOMPARE(eval("<Album Artist>/<Album>/<Title>", v), QString("Night Drive/Midnight Highway"));
        v.fields["title"].clear();
        QCOMPARE(eval("<Album>/<Title>", v), QString("Night Drive/Untitled"));
    }

    void lengthLimits() {
        Mask::Values v = sample();
        v.fields["title"] = QString(300, QChar(0x00e9)); // é is two bytes in UTF-8
        const QString out = eval("<Title>", v);
        QVERIFY(out.toUtf8().size() <= 255);
        QCOMPARE(out.size(), 127);
        Mask::Sanitise rules;
        rules.maxComponentBytes = 10;
        QCOMPARE(Mask::sanitiseComponent("abcdefghijklmnop.flac", rules, true), QString("abcde.flac"));
        QCOMPARE(Mask::sanitiseComponent("abcdefghijklmnop.flac", rules, false), QString("abcdefghij"));
        Mask::Context c;
        c.sanitise.maxPathChars = 30;
        v.fields["title"] = QString(40, 'x');
        QCOMPARE(eval("<Album>/<Title>", v, c).size(), 30);
    }

    void parseErrors_data() {
        QTest::addColumn<QString>("mask");
        QTest::addColumn<int>("position");
        QTest::addColumn<QString>("message");
        QTest::newRow("unclosed field") << "<Album>/<Title" << 8 << "Unclosed '<'";
        QTest::newRow("unknown field") << "x <Albm>" << 2 << "Unknown field <Albm>";
        QTest::newRow("unknown function") << "ab $Nope(x)" << 3 << "Unknown function $Nope";
        QTest::newRow("no bracket") << "$Upper x" << 6 << "Expected '('";
        QTest::newRow("too few") << "-$If(<Year>)" << 1 << "$If takes 2 or 3 arguments, not 1";
        QTest::newRow("too many") << "$Upper(a,b)" << 0 << "$Upper takes 1 arguments";
        QTest::newRow("unclosed function") << "<Title> $Upper(<Album>" << 8 << "Unclosed $Upper(";
        QTest::newRow("unclosed quote") << "$Replace(<Title>,\"x,y)" << 17 << "Unclosed quote";
        QTest::newRow("skip") << "<Skip>" << 0 << "<Skip> works only";
        QTest::newRow("folder zero") << "<Folder:0>" << 0 << "counts from 1";
    }

    void parseErrors() {
        QFETCH(QString, mask);
        QFETCH(int, position);
        QFETCH(QString, message);
        const Mask::Parsed p = Mask::parse(mask);
        QVERIFY(!p.ok());
        QCOMPARE(p.position, position);
        QVERIFY2(p.error.contains(message), qPrintable(p.error));
        QString error;
        QVERIFY(Mask::evaluate(mask, sample(), {}, &error).isEmpty());
        QVERIFY(error.contains(QString("at character %1").arg(position + 1)));
    }

    void valuesFromTrack() {
        QTemporaryDir dir;
        Library library(dir.filePath("library.db"));
        QVERIFY(library.open());
        QVERIFY(Fixture::fill(library));
        QSqlQuery q(library.db());
        q.exec("INSERT INTO folders (volume_id, rel_path, is_root, added) VALUES (1, 'Summer Mix', 1, 0)");
        q.exec("SELECT id FROM tracks WHERE title = 'Tidal'");
        QVERIFY(q.next());
        const Mask::Values v = Mask::fromTrack(library, q.value(0).toLongLong());
        QCOMPARE(v.path, QString("/music/Summer Mix/03 Tidal.flac"));
        QCOMPARE(v.rootPath, QString("/music/Summer Mix"));
        // A compilation with no album artist files under Various Artists.
        QCOMPARE(v.value("albumartist"), QString("Various Artists"));
        QCOMPARE(eval("<Album Artist>/<Album>/<Track#:2> <Title>", v), QString("Various Artists/Summer Mix/03 Tidal"));
        QCOMPARE(text("<Rating> <Year> <Extension> <Filename> <Length>", v), QString("4.5 2001 flac 03 Tidal 4:20"));
        q.exec("SELECT id FROM tracks WHERE title = 'Go West'");
        QVERIFY(q.next());
        const Mask::Values w = Mask::fromTrack(library, q.value(0).toLongLong());
        QCOMPARE(w.rootPath, QString("/music")); // not under a library folder: the volume
        QCOMPARE(eval("<Path>", w), QString("Go"));
        QCOMPARE(text("<Rating>|<Album Artist>", w), QString("|Pet Dogs"));
    }

    void tagFromFilename() {
        Mask::FilenameMatch m =
            Mask::matchFilename("<Track#> - <Artist> - <Title>", "/m/x/01 - Neon Coast - Midnight Highway.flac");
        QVERIFY2(m.ok, qPrintable(m.error));
        QCOMPARE(m.values.value("track"), QString("1"));
        QCOMPARE(m.values.value("artist"), QString("Neon Coast"));
        QCOMPARE(m.values.value("title"), QString("Midnight Highway"));

        // Folders: the album from the folder above, the artist above that.
        m = Mask::matchFilename("<Artist>/<Album>/<Track#> <Title>", "/m/Neon Coast/Night Drive/07 Afterglow.mp3");
        QVERIFY(m.ok);
        QCOMPARE(m.values.value("album"), QString("Night Drive"));
        QCOMPARE(m.values.value("artist"), QString("Neon Coast"));
        QCOMPARE(m.values.value("track"), QString("7"));
        QCOMPARE(m.values.value("title"), QString("Afterglow"));

        // <Skip> keeps nothing; a dotted name keeps its dots.
        m = Mask::matchFilename("<Skip> - <Title>", "/m/whatever - Mr. Blue Sky.ogg");
        QVERIFY(m.ok);
        QCOMPARE(m.values.size(), 1);
        QCOMPARE(m.values.value("title"), QString("Mr. Blue Sky"));

        // Fixed-width numbers: "101" is disc 1, track 1.
        m = Mask::matchFilename("<Disc#:1><Track#:2> <Title>", "/m/101 Song.flac");
        QVERIFY(m.ok);
        QCOMPARE(m.values.value("disc"), QString("1"));
        QCOMPARE(m.values.value("track"), QString("1"));

        m = Mask::matchFilename("<Track#> - <Title>", "/m/Song without number.flac");
        QVERIFY(!m.ok);
        QVERIFY(m.error.contains("doesn't match"));
        m = Mask::matchFilename("<Artist>/<Album>/<Title>", "/one.flac");
        QVERIFY(!m.ok);
        m = Mask::matchFilename("$Upper(<Title>)", "/m/a.flac");
        QVERIFY(!m.ok);
        QVERIFY(m.error.contains("functions"));
        m = Mask::matchFilename("<Folder:1>", "/m/a.flac");
        QVERIFY(!m.ok);
        QCOMPARE(m.position, 0);
        m = Mask::matchFilename("<Title", "/m/a.flac");
        QVERIFY(!m.ok);
    }

    void names() {
        QVERIFY(Mask::fieldNames().contains("<Album Artist>"));
        QVERIFY(Mask::functionNames().contains("$MovePrefix"));
        QCOMPARE(Mask::canonicalField("Track#"), QString("track"));
        QCOMPARE(Mask::canonicalField("Original Date"), QString("originalyear"));
        QCOMPARE(Mask::canonicalField("nope"), QString());
        QVERIFY(Mask::isNumericField("disc"));
        QVERIFY(!Mask::isNumericField("title"));
    }
};

int runMaskTests(int argc, char **argv) {
    MaskTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "mask_tests.moc"
