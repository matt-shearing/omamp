#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

#include "player.h"
#include "playlist.h"
#include "spectrum.h"
#include "theme.h"
#include "translator.h"

class OmampTests : public QObject {
    Q_OBJECT

private slots:
    void fileNamesBecomeTitles()
    {
        auto t = Playlist::trackFromFile("/m/03 - Neon Coast - Midnight_Highway.mp3");
        QCOMPARE(t.artist, QString("Neon Coast"));
        QCOMPARE(t.title, QString("Midnight Highway"));
        t = Playlist::trackFromFile("/m/07. Interlude.flac");
        QCOMPARE(t.artist, QString());
        QCOMPARE(t.title, QString("Interlude"));
    }

    void foldersAreSearchedInTrackOrder()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("album/disc");
        for (const char *name : {"album/10 b.mp3", "album/2 a.flac", "album/cover.jpg", "album/disc/1 c.ogg"})
            touch(dir.filePath(name));
        Playlist list;
        QCOMPARE(list.addPaths({dir.path()}), 3);
        QVERIFY(list.at(0).path.endsWith("2 a.flac"));
        QVERIFY(list.at(1).path.endsWith("10 b.mp3"));
        QVERIFY(list.at(2).path.endsWith("disc/1 c.ogg"));
    }

    void nextStopsAtTheEndUnlessRepeating()
    {
        QTemporaryDir dir;
        touch(dir.filePath("1.mp3"));
        touch(dir.filePath("2.mp3"));
        Player player;
        player.setMuted(true);
        player.addPaths({dir.path()});
        QCOMPARE(player.currentIndex(), 0);
        QCOMPARE(player.nextIndex(), 1);
        player.next();
        QCOMPARE(player.currentIndex(), 1);
        QCOMPARE(player.nextIndex(), -1);
        player.setRepeat(true);
        QCOMPARE(player.nextIndex(), 0);
        player.setShuffle(true);
        for (int i = 0; i < 20; ++i)
            QCOMPARE(player.nextIndex(), 0);
    }

    void removingKeepsTheCurrentTrack()
    {
        QTemporaryDir dir;
        for (const char *name : {"1.mp3", "2.mp3", "3.mp3"})
            touch(dir.filePath(name));
        Player player;
        player.setMuted(true);
        player.addPaths({dir.path()});
        player.next();
        player.next();
        QCOMPARE(player.currentIndex(), 2);
        player.remove(0);
        QCOMPARE(player.currentIndex(), 1);
        QVERIFY(player.playlist()->at(1).path.endsWith("3.mp3"));
        player.move(1, 0);
        QCOMPARE(player.currentIndex(), 0);
    }

    void stateIsRememberedBetweenRuns()
    {
        QTemporaryDir dir;
        touch(dir.filePath("1.mp3"));
        touch(dir.filePath("2.mp3"));
        const QString state = dir.filePath("config/state.json");
        {
            Player player(state);
            player.setMuted(true);
            player.addPaths({dir.filePath("1.mp3"), dir.filePath("2.mp3")});
            player.next();
            player.setVolume(42);
            player.setShuffle(true);
        }
        QVERIFY(QFile::exists(state));
        Player again(state);
        QCOMPARE(again.playlist()->count(), 2);
        QCOMPARE(again.currentIndex(), 1);
        QCOMPARE(again.volume(), 42);
        QVERIFY(again.shuffle());
    }

    void analyserFindsTheTone()
    {
        Spectrum spectrum(24);
        QList<float> tone(1024);
        for (int i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * std::sin(2 * M_PI * 1000 * i / 44100.0);
        spectrum.analyse(tone, 44100);
        const auto levels = spectrum.levels();
        const int loudest = std::max_element(levels.begin(), levels.end()) - levels.begin();
        // 1 kHz sits in the middle of a 50 Hz to 16 kHz log scale.
        QVERIFY2(loudest >= 10 && loudest <= 14, qPrintable(QString::number(loudest)));
        QVERIFY(levels.first() < levels[loudest] * 0.5);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QVERIFY(theme.colors().contains("darker_background"));
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"dark\"\naccent = \"#ff0080\"\ncyan = \"#00c8ff\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#ff0080"));
        QCOMPARE(theme.colors().value("cyan").toString(), QString("#00c8ff"));
        QCOMPARE(theme.colors().value("green").toString(), Theme::defaults().value("green").toString());
        QVERIFY(!theme.light());
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Keys").isNull());
    }

private:
    static void touch(const QString &path) { write(path, "x"); }
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(OmampTests)
#include "omamp_tests.moc"
