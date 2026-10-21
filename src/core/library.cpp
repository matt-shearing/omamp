#include "library.h"

#include "migrations.h"
#include "mounts.h"
#include "remote.h"
#include "volumes.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

Library::Library(const QString &path)
    : m_path(path.isEmpty() ? defaultPath() : path),
      m_connection("thrum-library-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

Library::~Library() { close(); }

QString Library::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/thrum/library.db";
}

int Library::latestVersion() { return migrations().last().version; }

bool Library::isOpen() const {
    return QSqlDatabase::contains(m_connection) && QSqlDatabase::database(m_connection, false).isOpen();
}

void Library::close() {
    if (!QSqlDatabase::contains(m_connection))
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen()) {
            // Folds the WAL back into the main file, so a closed library is one file.
            QSqlQuery(db).exec("PRAGMA wal_checkpoint(TRUNCATE)");
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connection);
}

QSqlDatabase Library::db() const { return QSqlDatabase::database(m_connection, false); }

static bool run(QSqlQuery &query, const QString &sql, QString *error) {
    if (query.exec(sql))
        return true;
    if (error)
        *error = QString("%1 (in: %2)").arg(query.lastError().text(), sql.simplified().left(120));
    return false;
}

// Opens the connection and sets it up, apart from open() so that every query
// and handle is gone before a failure closes the connection, and before a
// migration's VACUUM INTO (which refuses to run beside an open statement).
bool Library::connect(QString *error) {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    db.setDatabaseName(m_path);
    // REGEXP is the query language's title:/^live/; the busy timeout lets a
    // CLI scan and the window share the file without "database is locked".
    db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000;QSQLITE_ENABLE_REGEXP");
    if (!db.open()) {
        *error = db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    if (!run(q, "PRAGMA journal_mode = WAL", error) || !run(q, "PRAGMA synchronous = NORMAL", error) ||
        !run(q, "PRAGMA foreign_keys = ON", error) || !run(q, "PRAGMA temp_store = MEMORY", error) ||
        !run(q, "PRAGMA cache_size = -32000", error))
        return false;

    // Trigram search with remove_diacritics needs SQLite 3.45.
    if (q.exec("SELECT sqlite_version()") && q.next()) {
        const QString version = q.value(0).toString();
        const QStringList parts = version.split('.');
        if (parts.size() >= 2 && (parts[0].toInt() < 3 || (parts[0].toInt() == 3 && parts[1].toInt() < 45))) {
            *error = QString("SQLite %1 is too old; Thrum needs 3.45 or newer").arg(version);
            return false;
        }
    }
    return true;
}

bool Library::open(QString *error, int targetVersion) {
    if (isOpen())
        return true;
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QString("Can't create %1").arg(info.absolutePath());
        return false;
    }

    // THRUM_MOUNTINFO lets tests fake the mount table; nothing else sets it.
    if (!checkDatabaseIsLocal(m_path, VolumeResolver(qEnvironmentVariable("THRUM_MOUNTINFO", "/proc/self/mountinfo")),
                              error))
        return false;

    QString why;
    if (!connect(&why)) {
        if (error)
            *error = why;
        // connect() has returned, so no query or handle still holds the
        // connection and it can be removed cleanly.
        close();
        return false;
    }

    if (!migrate(targetVersion < 0 ? latestVersion() : targetVersion, error)) {
        close();
        return false;
    }
    return true;
}

int Library::schemaVersion() const {
    QSqlQuery q(db());
    q.exec("PRAGMA user_version");
    return q.next() ? q.value(0).toInt() : -1;
}

QString Library::sqliteVersion() const {
    QSqlQuery q(db());
    q.exec("SELECT sqlite_version()");
    return q.next() ? q.value(0).toString() : QString();
}

// A consistent copy taken with VACUUM INTO, which works while the database is
// open in WAL mode and never copies a half-written page.
bool Library::backup(int fromVersion, QString *error) {
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    QString target = QString("%1.v%2-%3.bak").arg(m_path).arg(fromVersion).arg(stamp);
    for (int n = 2; QFileInfo::exists(target); ++n)
        target = QString("%1.v%2-%3-%4.bak").arg(m_path).arg(fromVersion).arg(stamp).arg(n);
    QSqlQuery q(db());
    q.prepare("VACUUM INTO ?");
    q.addBindValue(target);
    if (!q.exec()) {
        if (error)
            *error = QString("Backup before migrating failed: %1").arg(q.lastError().text());
        return false;
    }
    m_lastBackup = target;
    return true;
}

bool Library::migrate(int target, QString *error) {
    const int current = schemaVersion();
    if (current > latestVersion()) {
        if (error)
            *error = QString("%1 is from a newer Thrum (schema %2; this one knows %3)")
                         .arg(m_path)
                         .arg(current)
                         .arg(latestVersion());
        return false;
    }
    if (current >= target)
        return true;
    // A brand new file has nothing worth backing up.
    if (current > 0 && !backup(current, error))
        return false;

    QSqlDatabase database = db();
    for (const Migration &m : migrations()) {
        if (m.version <= current || m.version > target)
            continue;
        // PRAGMA foreign_keys is a no-op inside a transaction, so it is
        // switched off before the step begins and back on after it ends,
        // whichever way it ends.
        struct ForeignKeys {
            QSqlDatabase db;
            bool off;
            ~ForeignKeys() {
                if (off)
                    QSqlQuery(db).exec("PRAGMA foreign_keys = ON");
            }
        } keys{database, m.rebuildsTables};
        if (m.rebuildsTables)
            QSqlQuery(database).exec("PRAGMA foreign_keys = OFF");
        if (!database.transaction()) {
            if (error)
                *error = database.lastError().text();
            return false;
        }
        QSqlQuery q(database);
        QString why;
        bool ok = true;
        for (const QString &statement : m.statements) {
            if (!run(q, statement, &why)) {
                ok = false;
                break;
            }
        }
        if (ok && m.rebuildsTables) {
            QSqlQuery check(database);
            if (check.exec("PRAGMA foreign_key_check") && check.next()) {
                ok = false;
                why = QString("a row in %1 lost its %2").arg(check.value(0).toString(), check.value(2).toString());
            }
        }
        if (ok)
            ok = run(q, QString("PRAGMA user_version = %1").arg(m.version), &why);
        if (!ok) {
            database.rollback();
            if (error)
                *error = QString("Migration %1 (%2) failed: %3").arg(m.version).arg(m.description, why);
            return false;
        }
        if (!database.commit()) {
            if (error)
                *error = database.lastError().text();
            return false;
        }
    }
    return true;
}

QVariant Library::setting(const QString &key, const QVariant &fallback) const {
    QSqlQuery q(db());
    q.prepare("SELECT value FROM settings WHERE key = ?");
    q.addBindValue(key);
    return q.exec() && q.next() ? q.value(0) : fallback;
}

void Library::setSetting(const QString &key, const QVariant &value) {
    QSqlQuery q(db());
    q.prepare("INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}

QList<VolumeCheck> Library::refreshVolumes(const VolumeResolver &resolver) {
    QSqlDatabase database = db();
    QList<VolumeCheck> checks;
    QList<QString> subpaths;
    QSqlQuery q(database);
    q.exec("SELECT id, key, root_path, mount_subpath, kind, coalesce(label, ''), online FROM volumes "
           "WHERE kind != 'subsonic' ORDER BY id");
    while (q.next()) {
        VolumeCheck c;
        c.id = q.value(0).toLongLong();
        c.key = q.value(1).toString();
        c.root = q.value(2).toString();
        c.kind = q.value(4).toString();
        c.label = q.value(5).toString();
        c.wasOnline = q.value(6).toBool();
        checks << c;
        subpaths << q.value(3).toString();
    }
    q.finish();

    // Look first, write after: the looking may wait on a slow share, and the
    // write transaction should not be held open meanwhile.
    for (int i = 0; i < checks.size(); ++i) {
        ProbeResult why;
        const QString where = resolver.locate(checks[i].key, checks[i].root, subpaths[i], &why);
        checks[i].online = !where.isEmpty();
        checks[i].state = checks[i].online ? QString("online") : why.name();
        if (checks[i].online)
            checks[i].root = where;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    database.transaction();
    for (VolumeCheck &c : checks) {
        QSqlQuery u(database);
        if (c.online) {
            u.prepare("UPDATE volumes SET online = 1, root_path = ?, last_seen = ? WHERE id = ?");
            u.addBindValue(c.root);
            u.addBindValue(now);
            u.addBindValue(c.id);
        } else {
            u.prepare("UPDATE volumes SET online = 0 WHERE id = ?");
            u.addBindValue(c.id);
        }
        u.exec();
        // Offline tracks come back as online; missing ones wait for a scan.
        // Run every time, not only on a change, so a crash between the two
        // updates can't leave tracks in the wrong state.
        QSqlQuery t(database);
        t.prepare(c.online ? "UPDATE tracks SET availability = 'online' WHERE volume_id = ? AND availability = 'offline'"
                           : "UPDATE tracks SET availability = 'offline' WHERE volume_id = ? AND availability = 'online'");
        t.addBindValue(c.id);
        t.exec();
        if (c.online) {
            QSqlQuery r(database);
            r.prepare("SELECT rel_path FROM folders WHERE volume_id = ? AND is_root ORDER BY rel_path");
            r.addBindValue(c.id);
            r.exec();
            while (r.next()) {
                const QString rel = r.value(0).toString();
                c.libraryRoots << (rel.isEmpty() ? c.root : (c.root == "/" ? '/' + rel : c.root + '/' + rel));
            }
        }
    }
    database.commit();
    return checks;
}

LibraryStats Library::stats() const {
    LibraryStats s;
    QSqlQuery q(db());
    q.exec("SELECT count(*), "
           "coalesce(sum(availability = 'online'), 0), coalesce(sum(availability = 'offline'), 0), "
           "coalesce(sum(availability = 'missing'), 0), coalesce(sum(duration_ms), 0), coalesce(sum(size), 0) "
           "FROM tracks");
    if (q.next()) {
        s.tracks = q.value(0).toLongLong();
        s.online = q.value(1).toLongLong();
        s.offline = q.value(2).toLongLong();
        s.missing = q.value(3).toLongLong();
        s.totalDurationMs = q.value(4).toLongLong();
        s.totalBytes = q.value(5).toLongLong();
    }
    auto count = [&](const char *sql) {
        QSqlQuery c(db());
        return c.exec(sql) && c.next() ? c.value(0).toLongLong() : 0;
    };
    s.volumes = count("SELECT count(*) FROM volumes");
    s.volumesOnline = count("SELECT count(*) FROM volumes WHERE online");
    s.folders = count("SELECT count(*) FROM folders");
    s.artists = count("SELECT count(*) FROM artists");
    s.albums = count("SELECT count(*) FROM albums");
    s.genres = count("SELECT count(*) FROM genres");
    s.playlists = count("SELECT count(*) FROM playlists");
    return s;
}

QString Library::absolutePath(qint64 trackId) const {
    QSqlQuery q(db());
    q.prepare("SELECT v.root_path, t.rel_path, v.online, v.kind FROM tracks t JOIN volumes v ON v.id = t.volume_id "
              "WHERE t.id = ?");
    q.addBindValue(trackId);
    // A remote track has no path on this machine: see playableUrl().
    if (!q.exec() || !q.next() || !q.value(2).toBool() || q.value(3).toString() == "subsonic")
        return {};
    const QString root = q.value(0).toString();
    return root == "/" ? '/' + q.value(1).toString() : root + '/' + q.value(1).toString();
}

QUrl Library::playableUrl(qint64 trackId, QString *error) const {
    QSqlQuery q(db());
    q.prepare("SELECT v.kind, v.remote_source_id, t.rel_path, t.availability FROM tracks t "
              "JOIN volumes v ON v.id = t.volume_id WHERE t.id = ?");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = QString("Track %1 is not in the library").arg(trackId);
        return {};
    }
    const QString kind = q.value(0).toString();
    const qint64 source = q.value(1).toLongLong();
    const QString rel = q.value(2).toString();
    const QString availability = q.value(3).toString();
    q.finish();
    if (availability != "online") {
        if (error)
            *error = QString("Track %1 is %2").arg(trackId).arg(availability);
        return {};
    }
    if (kind == "subsonic")
        return RemoteSources::streamUrl(*this, source, rel, error);
    const QString path = absolutePath(trackId);
    if (path.isEmpty()) {
        if (error)
            *error = QString("Track %1's volume is offline").arg(trackId);
        return {};
    }
    return QUrl::fromLocalFile(path);
}
