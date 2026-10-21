// Moving Omamp's folders to Thrum's on the first start after the rename
// (src/core/renamemigration.h). Everything happens in temporary folders standing in for
// XDG_CONFIG_HOME, XDG_DATA_HOME and XDG_CACHE_HOME.

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "renamemigration.h"

namespace {

void write(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList filesUnder(const QString &dir) {
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << QDir(dir).relativeFilePath(it.next());
    out.sort();
    return out;
}

// A library.db in WAL mode as a running Omamp leaves it: the picture row is
// still only in library.db-wal. The three files are copied to folder while the
// connection is open, so the copy has a live -wal and -shm.
void makeDatabase(const QString &folder, const QString &cachePath) {
    QTemporaryDir staging;
    const QString path = staging.filePath("library.db");
    const QString name = QUuid::createUuid().toString();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("PRAGMA journal_mode=WAL"));
        QVERIFY(q.exec("PRAGMA wal_autocheckpoint=0"));
        QVERIFY(q.exec("CREATE TABLE artwork (id INTEGER PRIMARY KEY, cache_path TEXT)"));
        QVERIFY(q.exec("CREATE TABLE radio_stations (id INTEGER PRIMARY KEY, art_path TEXT)"));
        q.prepare("INSERT INTO artwork (cache_path) VALUES (?)");
        q.addBindValue(cachePath);
        QVERIFY(q.exec());
        QVERIFY(q.exec("INSERT INTO radio_stations (art_path) VALUES ('/elsewhere/omamp/x.png')"));
        QDir().mkpath(folder);
        for (const char *suffix : {"", "-wal", "-shm"})
            QVERIFY(QFile::copy(path + suffix, folder + "/library.db" + suffix));
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    QVERIFY(QFileInfo(folder + "/library.db-wal").size() > 0);
}

QStringList column(const QString &path, const QString &sql) {
    const QString name = QUuid::createUuid().toString();
    QStringList out;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            q.exec(sql);
            while (q.next())
                out << q.value(0).toString();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return out;
}

} // namespace

class RenameMigrationTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    RenameMigration::Roots roots(const QString &name) {
        const QString base = m_dir.filePath(name);
        return {base + "/config", base + "/share", base + "/cache"};
    }

private slots:
    void movesOmampFoldersWhenThrumHasNone() {
        const RenameMigration::Roots r = roots("old-only");
        write(r.config + "/omamp/state.json", "{\"volume\": 40}");
        write(r.config + "/omamp/window.conf", "[window]\n");
        write(r.config + "/omamp/spotify.conf", "[spotify]\n");
        write(r.config + "/omamp/omamp.conf", "[playback]\nengine=native\n");
        const QString oldCache = r.cache + "/omamp";
        makeDatabase(r.data + "/omamp", oldCache + "/art/ab/abcd.jpg");
        write(r.data + "/omamp/library.db.v10-20261020-062928.bak", "backup");
        write(oldCache + "/art/ab/abcd.jpg", "jpeg");

        QVERIFY(RenameMigration::pending(r));
        const RenameMigration::Result result = RenameMigration::migrate(r);
        QVERIFY2(result.moved, qPrintable(result.log.join("\n")));
        QVERIFY(!result.blocked);
        QVERIFY(!RenameMigration::pending(r));

        // Every old folder is gone and its files are under the new name.
        QVERIFY(!QFileInfo::exists(r.config + "/omamp"));
        QVERIFY(!QFileInfo::exists(r.data + "/omamp"));
        QVERIFY(!QFileInfo::exists(oldCache));
        QCOMPARE(readAll(r.config + "/thrum/state.json"), QByteArray("{\"volume\": 40}"));
        QVERIFY(QFile::exists(r.config + "/thrum/window.conf"));
        QVERIFY(QFile::exists(r.config + "/thrum/spotify.conf"));
        QCOMPARE(readAll(r.config + "/thrum/thrum.conf"), QByteArray("[playback]\nengine=native\n"));
        QVERIFY(!QFile::exists(r.config + "/thrum/omamp.conf"));
        // Opening the moved database to update its paths checkpoints the -wal
        // into it, so only the database and its backup are left.
        QCOMPARE(filesUnder(r.data + "/thrum"), QStringList({"library.db", "library.db.v10-20261020-062928.bak"}));
        QCOMPARE(readAll(r.cache + "/thrum/art/ab/abcd.jpg"), QByteArray("jpeg"));

        // The row that was only in the -wal is there, so the -wal moved with
        // the database; stored cache paths follow the cache, others stay.
        QCOMPARE(column(r.data + "/thrum/library.db", "SELECT cache_path FROM artwork"),
                 QStringList({r.cache + "/thrum/art/ab/abcd.jpg"}));
        QCOMPARE(column(r.data + "/thrum/library.db", "SELECT art_path FROM radio_stations"),
                 QStringList({"/elsewhere/omamp/x.png"}));

        // What happened is written down beside the settings.
        const QByteArray note = readAll(r.config + "/thrum/renamed-from-omamp.log");
        QVERIFY2(note.contains("library: moved"), note.constData());

        // A second start finds nothing to do.
        const RenameMigration::Result again = RenameMigration::migrate(r);
        QVERIFY(!again.moved);
        QVERIFY(again.log.isEmpty());
    }

    void keepsThrumFoldersAndLeavesOmampsAlone() {
        const RenameMigration::Roots r = roots("both");
        for (const QString &parent : {r.config, r.data, r.cache}) {
            write(parent + "/omamp/marker", "old");
            write(parent + "/thrum/marker", "new");
        }
        QVERIFY(!RenameMigration::pending(r));
        const RenameMigration::Result result = RenameMigration::migrate(r);
        QVERIFY(!result.moved);
        QCOMPARE(result.log.size(), 3); // says why each was left
        for (const QString &parent : {r.config, r.data, r.cache}) {
            QCOMPARE(readAll(parent + "/omamp/marker"), QByteArray("old"));
            QCOMPARE(readAll(parent + "/thrum/marker"), QByteArray("new"));
            QCOMPARE(filesUnder(parent + "/thrum"), QStringList({"marker"}));
        }
    }

    void movesOnlyWhatThrumLacks() {
        const RenameMigration::Roots r = roots("mixed");
        write(r.config + "/omamp/state.json", "old");
        write(r.config + "/thrum/state.json", "new");
        write(r.data + "/omamp/library.db", "db");
        const RenameMigration::Result result = RenameMigration::migrate(r);
        QVERIFY(result.moved);
        QCOMPARE(readAll(r.config + "/thrum/state.json"), QByteArray("new"));
        QCOMPARE(readAll(r.config + "/omamp/state.json"), QByteArray("old"));
        QCOMPARE(readAll(r.data + "/thrum/library.db"), QByteArray("db"));
        QVERIFY(!QFileInfo::exists(r.data + "/omamp"));
    }

    void doesNothingWithoutOmamp() {
        const RenameMigration::Roots r = roots("neither");
        QVERIFY(!RenameMigration::pending(r));
        const RenameMigration::Result result = RenameMigration::migrate(r);
        QVERIFY(!result.moved);
        QVERIFY(result.log.isEmpty());
        // Nothing is created either: not the parents, not Thrum's folders.
        QVERIFY(!QFileInfo::exists(m_dir.filePath("neither")));
    }

    void waitsWhileOmampIsRunning() {
        const RenameMigration::Roots r = roots("running");
        write(r.data + "/omamp/library.db", "db");
        const RenameMigration::Result result = RenameMigration::migrate(r, true);
        QVERIFY(result.blocked);
        QVERIFY(!result.moved);
        QVERIFY(result.blockedWhy.contains("Omamp is still running"));
        QVERIFY(QFile::exists(r.data + "/omamp/library.db"));
        QVERIFY(!QFileInfo::exists(r.data + "/thrum"));
        QVERIFY(RenameMigration::pending(r));
    }
};

int runRenameMigrationTests(int argc, char **argv) {
    RenameMigrationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "renamemigration_tests.moc"
