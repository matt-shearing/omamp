#include "renamemigration.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace RenameMigration {

namespace {

const char *const oldName = "omamp";
const char *const newName = "thrum";

bool present(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

// A copy for when the old folder is on another filesystem than its parent
// (a bind mount or a symlinked folder), where rename() can't move it.
bool copyTree(const QString &from, const QString &to) {
    if (!QDir().mkpath(to))
        return false;
    QDirIterator it(from, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QFileInfo info = it.fileInfo();
        const QString target = to + '/' + QDir(from).relativeFilePath(source);
        if (info.isSymLink()) {
            if (!QFile::link(info.symLinkTarget(), target))
                return false;
        } else if (info.isDir()) {
            if (!QDir().mkpath(target))
                return false;
        } else if (!QFile::copy(source, target)) {
            return false;
        }
    }
    return true;
}

enum class Moved { Nothing, Kept, Renamed, Copied, Failed };

Moved moveFolder(const QString &what, const QString &parent, QStringList &log) {
    const QString from = parent + '/' + oldName;
    const QString to = parent + '/' + newName;
    if (!present(from))
        return Moved::Nothing;
    if (present(to)) {
        log << QString("%1: kept %2 and left %3 as it is").arg(what, to, from);
        return Moved::Kept;
    }
    const QByteArray a = QFile::encodeName(from), b = QFile::encodeName(to);
    if (std::rename(a.constData(), b.constData()) == 0) {
        log << QString("%1: moved %2 to %3").arg(what, from, to);
        return Moved::Renamed;
    }
    const int error = errno;
    if (error == EXDEV) {
        // Copied beside the target and renamed into place, so a half copy
        // never looks like a finished one. The old folder stays.
        const QString temp = to + ".migrating-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        if (copyTree(from, temp) && std::rename(QFile::encodeName(temp).constData(), b.constData()) == 0) {
            log << QString("%1: copied %2 to %3 (another filesystem; the old folder is still there)")
                       .arg(what, from, to);
            return Moved::Copied;
        }
        QDir(temp).removeRecursively();
    }
    log << QString("%1: could not move %2 to %3: %4").arg(what, from, to, QString::fromLocal8Bit(std::strerror(error)));
    return Moved::Failed;
}

// Absolute paths into the old cache folder, stored by the library, now point
// into the new one.
void rewriteCachePaths(const QString &dbPath, const QString &oldCache, const QString &newCache, QStringList &log) {
    const QString connection = "thrum-rename-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            log << QString("library: could not open %1 to update cache paths").arg(dbPath);
        } else {
            const QString prefix = oldCache + '/';
            int changed = 0;
            for (const auto &[table, column] : {std::pair{"artwork", "cache_path"}, std::pair{"radio_stations", "art_path"}}) {
                QSqlQuery q(db);
                q.prepare(QString("UPDATE %1 SET %2 = ? || substr(%2, ?) WHERE substr(%2, 1, ?) = ?").arg(table, column));
                q.addBindValue(newCache + '/');
                q.addBindValue(prefix.size() + 1);
                q.addBindValue(prefix.size());
                q.addBindValue(prefix);
                if (q.exec()) // a table from a newer schema may not exist yet
                    changed += qMax(0, q.numRowsAffected());
            }
            if (changed > 0)
                log << QString("library: %1 cached picture paths now point into %2").arg(changed).arg(newCache);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
}

} // namespace

Roots defaultRoots() {
    return {QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)};
}

bool pending(const Roots &roots) {
    for (const QString &parent : {roots.config, roots.data, roots.cache}) {
        if (present(parent + '/' + oldName) && !present(parent + '/' + newName))
            return true;
    }
    return false;
}

qint64 runningOmampPid() {
    const QStringList pids = QDir("/proc").entryList({"[0-9]*"}, QDir::Dirs);
    for (const QString &pid : pids) {
        QFile comm("/proc/" + pid + "/comm");
        if (comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == oldName)
            return pid.toLongLong();
    }
    return 0;
}

Result migrate(const Roots &roots, bool omampRunning) {
    Result r;
    if (!pending(roots)) {
        // Still say when both exist, so the log explains why nothing moved.
        for (const auto &[what, parent] : {std::pair{"settings", roots.config}, std::pair{"library", roots.data},
                                           std::pair{"cache", roots.cache}}) {
            if (present(parent + '/' + oldName) && present(parent + '/' + newName))
                r.log << QString("%1: kept %2/%3 and left %2/%4 as it is").arg(what, parent, newName, oldName);
        }
        return r;
    }
    if (omampRunning) {
        r.blocked = true;
        r.blockedWhy = "Omamp is still running. Quit it, then start Thrum again: on its first start Thrum moves "
                       "Omamp's library, settings and cache over.";
        r.log << r.blockedWhy;
        return r;
    }

    const Moved config = moveFolder("settings", roots.config, r.log);
    if (config == Moved::Renamed || config == Moved::Copied) {
        const QString dir = roots.config + '/' + newName;
        const QString oldConf = dir + "/omamp.conf", newConf = dir + "/thrum.conf";
        if (QFile::exists(oldConf) && !QFile::exists(newConf) && QFile::rename(oldConf, newConf))
            r.log << QString("settings: renamed %1 to %2").arg(oldConf, newConf);
    }
    // The database moves with its whole folder in one rename, so library.db,
    // library.db-wal and library.db-shm can never be split up.
    const Moved data = moveFolder("library", roots.data, r.log);
    const Moved cache = moveFolder("cache", roots.cache, r.log);

    const auto movedOne = [](Moved m) { return m == Moved::Renamed || m == Moved::Copied; };
    r.moved = movedOne(config) || movedOne(data) || movedOne(cache);

    const QString oldCache = roots.cache + '/' + oldName, newCache = roots.cache + '/' + newName;
    const QString db = roots.data + '/' + newName + "/library.db";
    if ((movedOne(data) || movedOne(cache)) && QFile::exists(db) && present(newCache) && !present(oldCache))
        rewriteCachePaths(db, oldCache, newCache, r.log);

    if (r.moved) {
        // A note of what happened, beside the settings.
        QFile note(roots.config + '/' + newName + "/renamed-from-omamp.log");
        if (QDir().mkpath(roots.config + '/' + newName) && note.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&note);
            out << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
            for (const QString &line : r.log)
                out << "  " << line << '\n';
        }
    }
    return r;
}

bool runAtStartup(QString *message) {
    const Roots roots = defaultRoots();
    if (!pending(roots))
        return true;
    const qint64 pid = runningOmampPid();
    const Result r = migrate(roots, pid > 0);
    for (const QString &line : r.log)
        std::fprintf(stderr, "thrum: %s\n", qPrintable(line));
    if (r.blocked) {
        if (message)
            *message = r.blockedWhy + QString(" (Omamp's process id is %1.)").arg(pid);
        return false;
    }
    return true;
}

} // namespace RenameMigration
