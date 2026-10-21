#include "organise.h"

#include "job.h"
#include "library.h"
#include "ownwrites.h"
#include "tagreader.h"
#include "tagwriter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QJsonArray companionsJson(const QList<CompanionMove> &list) {
    QJsonArray out;
    for (const CompanionMove &c : list)
        out << c.toJson();
    return out;
}

QString folderOf(const QString &path) { return QFileInfo(path).absolutePath(); }

// The new path with " (n)" before its extension.
QString numbered(const QString &path, int n) {
    const QFileInfo info(path);
    const QString suffix = info.suffix();
    const QString stem = suffix.isEmpty() ? path : path.left(path.size() - suffix.size() - 1);
    return QString("%1 (%2)%3").arg(stem).arg(n).arg(suffix.isEmpty() ? QString() : '.' + suffix);
}

bool exists(const QString &path) {
    struct stat st {};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0;
}

dev_t deviceOf(const QString &path) {
    // The nearest folder that exists: the target folder may not exist yet.
    QString p = path;
    struct stat st {};
    while (!p.isEmpty()) {
        if (::stat(QFile::encodeName(p).constData(), &st) == 0)
            return st.st_dev;
        const QString up = QFileInfo(p).absolutePath();
        if (up == p)
            break;
        p = up;
    }
    return 0;
}

QByteArray sha1Of(const QString &path, bool *ok) {
    QFile f(path);
    *ok = f.open(QIODevice::ReadOnly);
    if (!*ok)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha1);
    *ok = hash.addData(&f);
    return hash.result();
}

void syncPath(const QString &path, bool directory) {
    const int fd = ::open(QFile::encodeName(path).constData(), directory ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

// Gives a file a new name only if nothing has that name. Returns 0, or the
// errno; EEXIST means the target is taken.
int renameNoReplace(const QString &from, const QString &to) {
    const QByteArray a = QFile::encodeName(from), b = QFile::encodeName(to);
    if (::renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
        return errno;
    // The filesystem can't refuse a replace by itself (some network
    // filesystems): a hard link fails if the name is taken.
    if (::link(a.constData(), b.constData()) == 0) {
        ::unlink(a.constData());
        return 0;
    }
    if (errno == EEXIST || errno == EXDEV)
        return errno;
    // No hard links either: check, then rename. A file appearing between the
    // two is the one case this can't rule out.
    if (exists(to))
        return EEXIST;
    return ::rename(a.constData(), b.constData()) == 0 ? 0 : errno;
}

struct VolumeRow {
    qint64 id;
    QString root;
};

QList<VolumeRow> onlineVolumes(Library &library) {
    QList<VolumeRow> out;
    QSqlQuery q(library.db());
    q.exec("SELECT id, root_path FROM volumes WHERE online = 1");
    while (q.next())
        out << VolumeRow{q.value(0).toLongLong(), q.value(1).toString()};
    return out;
}

// The volume holding an absolute path, and the path relative to it.
bool volumeFor(const QList<VolumeRow> &volumes, const QString &path, qint64 *id, QString *rel) {
    int best = -1;
    for (int k = 0; k < volumes.size(); ++k) {
        const QString &root = volumes[k].root;
        const bool inside = root == "/" || path.startsWith(root + '/');
        if (inside && (best < 0 || root.size() > volumes[best].root.size()))
            best = k;
    }
    if (best < 0)
        return false;
    *id = volumes[best].id;
    const QString &root = volumes[best].root;
    *rel = root == "/" ? path.mid(1) : path.mid(root.size() + 1);
    return true;
}

QString pathOf(Library &library, const QString &key) {
    // "volume:rel" from the journal, to an absolute path where the volume is now.
    const int colon = key.indexOf(':');
    QSqlQuery q(library.db());
    q.prepare("SELECT root_path, online FROM volumes WHERE id = ?");
    q.addBindValue(key.left(colon).toLongLong());
    if (colon < 0 || !q.exec() || !q.next() || !q.value(1).toBool())
        return {};
    const QString root = q.value(0).toString();
    return (root == "/" ? QString() : root) + '/' + key.mid(colon + 1);
}

QSet<QString> libraryRootFolders(Library &library) {
    QSet<QString> out;
    QSqlQuery q(library.db());
    q.exec("SELECT v.root_path, f.rel_path FROM folders f JOIN volumes v ON v.id = f.volume_id WHERE f.is_root = 1");
    while (q.next()) {
        const QString root = q.value(0).toString(), rel = q.value(1).toString();
        out.insert(rel.isEmpty() ? root : (root == "/" ? QString() : root) + '/' + rel);
    }
    return out;
}

void journal(Library &library, const QString &batch, qint64 trackId, const QString &field, const QString &oldValue,
             const QString &newValue, const QString &source) {
    QSqlQuery q(library.db());
    q.prepare("INSERT INTO tag_journal (batch, track_id, field, old_value, new_value, source, changed_at) "
              "VALUES (?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(batch);
    q.addBindValue(trackId ? QVariant(trackId) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(field);
    q.addBindValue(oldValue);
    q.addBindValue(newValue);
    q.addBindValue(source);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.exec();
}

} // namespace

QJsonObject CompanionMove::toJson() const {
    QJsonObject o{{"from", from}, {"to", to}, {"skip", skip}};
    if (trackId)
        o["track_id"] = trackId;
    if (!note.isEmpty())
        o["note"] = note;
    return o;
}

QJsonObject OrganiseItem::toJson() const {
    QJsonObject o{{"track_id", trackId}, {"old_path", oldPath}, {"new_path", newPath}, {"status", status}};
    if (!note.isEmpty())
        o["note"] = note;
    if (!companions.isEmpty())
        o["companions"] = companionsJson(companions);
    return o;
}

QJsonObject OrganisePlan::toJson() const {
    QJsonArray list;
    for (const OrganiseItem &i : items)
        list << i.toJson();
    QJsonObject o{{"ok", ok},
                  {"mask", mask},
                  {"destination", destination.isEmpty() ? QJsonValue() : QJsonValue(destination)},
                  {"moves", moves},
                  {"renamed", renamed},
                  {"unchanged", unchanged},
                  {"problems", problems},
                  {"items", list},
                  {"folder_companions", companionsJson(folderCompanions)}};
    if (!error.isEmpty()) {
        o["error"] = error;
        o["error_position"] = errorPosition;
    }
    return o;
}

QJsonObject OrganiseResult::toJson() const {
    QJsonArray list;
    for (const OrganiseItem &i : items)
        list << i.toJson();
    QJsonObject o{{"ok", ok},
                  {"batch", batch.isEmpty() ? QJsonValue() : QJsonValue(batch)},
                  {"cancelled", cancelled},
                  {"moved", moved},
                  {"copied", copied},
                  {"unchanged", unchanged},
                  {"failed", failed},
                  {"companions_moved", companionsMoved},
                  {"folders_removed", foldersRemoved},
                  {"elapsed_ms", elapsedMs},
                  {"items", list}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

Organiser::Organiser(Library &library, Job *job) : m_library(library), m_job(job) {}

OrganisePlan Organiser::plan(const QList<qint64> &trackIds, const OrganiseOptions &options) {
    OrganisePlan plan;
    plan.mask = options.mask;
    plan.destination = options.destination.isEmpty() ? QString() : QDir::cleanPath(options.destination);
    const Mask::Parsed mask = Mask::parse(options.mask);
    if (options.mask.trimmed().isEmpty()) {
        plan.ok = false;
        plan.error = "The mask is empty";
        return plan;
    }
    if (!mask.ok()) {
        plan.ok = false;
        plan.error = mask.error;
        plan.errorPosition = mask.position;
        return plan;
    }
    if (!plan.destination.isEmpty() && !QDir::isAbsolutePath(plan.destination)) {
        plan.ok = false;
        plan.error = "The destination must be an absolute folder";
        return plan;
    }

    const QList<VolumeRow> volumes = onlineVolumes(m_library);
    QSqlQuery availability(m_library.db());
    availability.prepare("SELECT availability FROM tracks WHERE id = ?");
    Mask::Context context;
    context.sanitise = options.sanitise;
    for (int k = 0; k < trackIds.size(); ++k) {
        OrganiseItem item;
        item.trackId = trackIds[k];
        availability.addBindValue(item.trackId);
        const QString state = availability.exec() && availability.next() ? availability.value(0).toString() : QString();
        availability.finish();
        const Mask::Values values = Mask::fromTrack(m_library, item.trackId);
        item.oldPath = m_library.absolutePath(item.trackId);
        if (state.isEmpty()) {
            item.status = "error";
            item.note = "Not in the library";
        } else if (item.oldPath.isEmpty() || state == "offline") {
            item.status = "offline";
            item.note = "Its volume is offline";
            item.oldPath = values.path;
        } else if (state == "missing" || !exists(item.oldPath)) {
            item.status = "missing";
            item.note = "The file is missing";
        } else {
            context.autoNumber = k + 1;
            const QString rel = Mask::evaluate(mask, values, context);
            const QString root = plan.destination.isEmpty() ? values.rootPath : plan.destination;
            const QString suffix = QFileInfo(item.oldPath).suffix();
            QString name = rel.section('/', -1);
            const QString folders = rel.contains('/') ? rel.section('/', 0, -2) : QString();
            name = Mask::sanitiseComponent(suffix.isEmpty() ? name : name + '.' + suffix, options.sanitise, true);
            item.newPath = QDir::cleanPath(root + '/' + (folders.isEmpty() ? QString() : folders + '/') + name);
            item.status = item.newPath == item.oldPath ? "unchanged" : "move";
            qint64 volume = 0;
            QString volumeRel;
            if (plan.destination.isEmpty() && !values.inLibraryFolder) {
                // Never file a track at the root of a whole disk.
                item.status = "error";
                item.note = "Not inside a library folder; give a destination";
            } else if (!volumeFor(volumes, item.newPath, &volume, &volumeRel)) {
                item.status = "error";
                item.note = "The destination is not on a library volume; add it with thrum scan first";
            } else if (!Journal::insideSandbox(m_sandbox, item.newPath) ||
                       !Journal::insideSandbox(m_sandbox, item.oldPath)) {
                item.status = "error";
                item.note = "Outside the allowed folder";
            }
        }
        plan.items << item;
    }

    // Names that clash get " (2)", " (3)": first the files that stay where
    // they are claim their names, then the moves in order.
    QSet<QString> claimed;
    for (const OrganiseItem &item : std::as_const(plan.items))
        if (item.status == "unchanged")
            claimed.insert(item.newPath);
    for (OrganiseItem &item : plan.items) {
        if (item.status != "move")
            continue;
        QString candidate = item.newPath;
        for (int n = 2; claimed.contains(candidate) || (candidate != item.oldPath && exists(candidate)); ++n)
            candidate = numbered(item.newPath, n);
        if (candidate != item.newPath) {
            item.note = QString("%1 is taken").arg(QFileInfo(item.newPath).fileName());
            item.newPath = candidate;
            item.status = candidate == item.oldPath ? "unchanged" : "renamed";
        }
        claimed.insert(item.newPath);
    }

    // A track's own lyrics file follows it.
    for (OrganiseItem &item : plan.items) {
        if (!item.moves())
            continue;
        const QFileInfo from(item.oldPath), to(item.newPath);
        const QString lrc = from.absolutePath() + '/' + from.completeBaseName() + ".lrc";
        if (exists(lrc)) {
            CompanionMove c{lrc, to.absolutePath() + '/' + to.completeBaseName() + ".lrc", item.trackId, false, {}};
            if (exists(c.to)) {
                c.skip = true;
                c.note = "A file with that name is already there";
            }
            item.companions << c;
        }
    }

    // A folder whose every audio file moves to one new folder takes its
    // pictures, cue sheets and logs along.
    if (options.moveCompanions) {
        QHash<QString, QSet<QString>> targets; // old folder -> new folders
        QHash<QString, QSet<QString>> movingNames;
        QSet<QString> blocked;
        for (const OrganiseItem &item : std::as_const(plan.items)) {
            const QString folder = folderOf(item.oldPath.isEmpty() ? item.newPath : item.oldPath);
            if (!item.moves()) {
                if (item.status == "unchanged" || !item.oldPath.isEmpty())
                    blocked.insert(folder);
                continue;
            }
            targets[folder].insert(folderOf(item.newPath));
            movingNames[folder].insert(QFileInfo(item.oldPath).fileName());
        }
        for (auto it = targets.constBegin(); it != targets.constEnd(); ++it) {
            if (blocked.contains(it.key()) || it.value().size() != 1)
                continue;
            const QString target = *it.value().constBegin();
            if (target == it.key())
                continue;
            const QDir dir(it.key());
            bool whole = true;
            const QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QString &f : files)
                if (TagReader::isAudio(f) && !movingNames[it.key()].contains(f))
                    whole = false;
            if (!whole)
                continue;
            QSet<QString> lrcTaken;
            for (const OrganiseItem &item : std::as_const(plan.items))
                for (const CompanionMove &c : item.companions)
                    lrcTaken.insert(c.from);
            for (const QString &f : files) {
                const QString from = it.key() + '/' + f;
                if (TagReader::isAudio(f) || lrcTaken.contains(from) ||
                    !options.companionExtensions.contains(QFileInfo(f).suffix().toLower()))
                    continue;
                CompanionMove c{from, target + '/' + f, 0, false, {}};
                if (exists(c.to) || claimed.contains(c.to)) {
                    c.skip = true;
                    c.note = "A file with that name is already there";
                }
                if (!Journal::insideSandbox(m_sandbox, c.to))
                    c.skip = true;
                plan.folderCompanions << c;
            }
        }
    }

    for (const OrganiseItem &item : std::as_const(plan.items)) {
        if (item.status == "move")
            ++plan.moves;
        else if (item.status == "renamed") {
            ++plan.moves;
            ++plan.renamed;
        } else if (item.status == "unchanged")
            ++plan.unchanged;
        else
            ++plan.problems;
    }
    return plan;
}

bool Organiser::moveFile(const QString &from, const QString &to, bool *copied, QString *error) {
    *copied = false;
    if (!Journal::insideSandbox(m_sandbox, from) || !Journal::insideSandbox(m_sandbox, to)) {
        *error = "Outside the allowed folder";
        return false;
    }
    if (exists(to)) {
        *error = QString("%1 already exists; it was not replaced").arg(to);
        return false;
    }
    const QString toFolder = folderOf(to);
    // Every path this move touches is our own change: the folder watcher
    // must not rescan for it (the library row is updated by the caller).
    OwnWrites::note(from);
    OwnWrites::note(to);
    OwnWrites::noteFolder(toFolder);
    auto noteAgain = qScopeGuard([&] {
        OwnWrites::note(from);
        OwnWrites::note(to);
    });
    if (!QDir().mkpath(toFolder)) {
        *error = QString("Can't make the folder %1").arg(toFolder);
        return false;
    }
    bool cross = m_crossDevice ? m_crossDevice(from, toFolder) : deviceOf(from) != deviceOf(toFolder);
    if (!cross) {
        const int e = renameNoReplace(from, to);
        if (e == 0) {
            syncPath(toFolder, true);
            return true;
        }
        if (e != EXDEV) {
            *error = e == EEXIST ? QString("%1 already exists; it was not replaced").arg(to)
                                 : QString("Can't move the file: %1").arg(QString::fromLocal8Bit(strerror(e)));
            return false;
        }
        cross = true;
    }

    // Across filesystems: copy beside the target, check, name it, then
    // delete the original.
    const QString temporary =
        toFolder + '/' + QLatin1String(OwnWrites::temporaryPrefix) +
        QString("%1-").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')) + QFileInfo(to).fileName();
    auto cleanup = qScopeGuard([&] { QFile::remove(temporary); });
    if (!QFile::copy(from, temporary)) {
        *error = "Can't copy the file to the other filesystem";
        return false;
    }
    syncPath(temporary, false);
    bool okA = false, okB = false;
    const QByteArray a = sha1Of(from, &okA), b = sha1Of(temporary, &okB);
    if (!okA || !okB || a != b || QFileInfo(from).size() != QFileInfo(temporary).size()) {
        *error = "The copy did not match the original; the original was kept";
        return false;
    }
    // Keep the original's modification time, as a rename would.
    struct stat st {};
    if (::stat(QFile::encodeName(from).constData(), &st) == 0) {
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        ::utimensat(AT_FDCWD, QFile::encodeName(temporary).constData(), times, 0);
    }
    const int e = renameNoReplace(temporary, to);
    if (e != 0) {
        *error = e == EEXIST ? QString("%1 already exists; it was not replaced").arg(to)
                             : QString("Can't name the copy: %1").arg(QString::fromLocal8Bit(strerror(e)));
        return false;
    }
    cleanup.dismiss();
    syncPath(toFolder, true);
    *copied = true;
    if (!QFile::remove(from)) {
        *error = QString("Copied, but the original could not be deleted: %1").arg(from);
        // The copy is the file now; the caller records the move.
    }
    return true;
}

bool Organiser::updatePath(qint64 trackId, const QString &newPath, QString *oldKey, QString *newKey, QString *error) {
    qint64 volume = 0;
    QString rel;
    if (!volumeFor(onlineVolumes(m_library), newPath, &volume, &rel)) {
        *error = QString("%1 is not on a library volume; add its folder with thrum scan first").arg(newPath);
        return false;
    }
    QSqlDatabase db = m_library.db();
    QSqlQuery old(db);
    old.prepare("SELECT volume_id, rel_path FROM tracks WHERE id = ?");
    old.addBindValue(trackId);
    if (!old.exec() || !old.next()) {
        *error = "The track is no longer in the library";
        return false;
    }
    *oldKey = QString("%1:%2").arg(old.value(0).toLongLong()).arg(old.value(1).toString());
    *newKey = QString("%1:%2").arg(volume).arg(rel);
    old.finish();

    const QString folderRel = rel.contains('/') ? rel.section('/', 0, -2) : QString("");
    QSqlQuery folder(db);
    folder.prepare("INSERT INTO folders (volume_id, rel_path, is_root, added) VALUES (?, ?, 0, ?) "
                   "ON CONFLICT(volume_id, rel_path) DO UPDATE SET rel_path = rel_path RETURNING id");
    folder.addBindValue(volume);
    folder.addBindValue(folderRel);
    folder.addBindValue(QDateTime::currentSecsSinceEpoch());
    const qint64 folderId = folder.exec() && folder.next() ? folder.value(0).toLongLong() : 0;
    folder.finish();

    struct stat st {};
    ::stat(QFile::encodeName(newPath).constData(), &st);
    const QString fileName = rel.section('/', -1);
    const int dot = fileName.lastIndexOf('.');
    QSqlQuery u(db);
    u.prepare("UPDATE tracks SET volume_id = ?, folder_id = ?, rel_path = ?, file_name = ?, format = ?, size = ?, "
              "mtime_ns = ?, availability = 'online', updated = ? WHERE id = ?");
    u.addBindValue(volume);
    u.addBindValue(folderId ? QVariant(folderId) : QVariant(QMetaType(QMetaType::LongLong)));
    u.addBindValue(rel);
    u.addBindValue(dot > 0 ? fileName.left(dot) : fileName);
    u.addBindValue(dot > 0 ? fileName.mid(dot + 1).toLower() : QString());
    u.addBindValue(qint64(st.st_size));
    u.addBindValue(qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec);
    u.addBindValue(QDateTime::currentSecsSinceEpoch());
    u.addBindValue(trackId);
    if (!u.exec()) {
        *error = u.lastError().text();
        return false;
    }
    return true;
}

bool Organiser::removeEmptyFolders(const QStringList &folders, const QString &batch, const QString &source,
                                   int *removed) {
    const QSet<QString> roots = libraryRootFolders(m_library);
    QStringList queue = folders;
    // Deepest first, so a parent is looked at after its children.
    std::sort(queue.begin(), queue.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    QSet<QString> seen;
    for (int k = 0; k < queue.size(); ++k) {
        const QString folder = QDir::cleanPath(queue[k]);
        if (seen.contains(folder) || roots.contains(folder) || folder == "/" || !Journal::insideSandbox(m_sandbox, folder))
            continue;
        seen.insert(folder);
        QDir dir(folder);
        if (!dir.exists() || !dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            continue;
        // Never above a library folder: the parent must still be inside one.
        bool insideRoot = false;
        for (const QString &root : roots)
            if (folder.startsWith(root + '/'))
                insideRoot = true;
        if (!insideRoot)
            continue;
        OwnWrites::note(folder);
        if (!QDir().rmdir(folder))
            continue;
        ++*removed;
        journal(m_library, batch, 0, "folder_removed", folder, QString(), source);
        queue << folderOf(folder);
    }
    return true;
}

OrganiseResult Organiser::apply(const OrganisePlan &plan, const OrganiseOptions &options) {
    OrganiseResult result;
    QElapsedTimer timer;
    timer.start();
    if (!plan.ok) {
        result.ok = false;
        result.error = plan.error;
        return result;
    }
    const QString batch = TagWriter::newBatchId("organise");
    const qint64 jobRow = Journal::startJobRow(m_library, "organise", {batch, plan.mask});
    if (m_job)
        m_job->start();

    QSet<QString> failedFolders; // old folders with a track that didn't move
    QStringList emptied;
    bool journalled = false;
    for (int k = 0; k < plan.items.size(); ++k) {
        OrganiseItem item = plan.items[k];
        if (m_job && m_job->isCancelled()) {
            result.cancelled = true;
            item.status = "skipped";
            result.items << item;
            failedFolders.insert(folderOf(item.oldPath));
            continue;
        }
        if (m_job)
            m_job->setProgress("Moving files", k, plan.items.size());
        if (item.status == "unchanged") {
            ++result.unchanged;
            result.items << item;
            continue;
        }
        if (!item.moves()) {
            // Offline, missing or refused at planning: reported, not tried.
            item.note = item.note.isEmpty() ? item.status : item.note;
            item.status = "skipped";
            result.items << item;
            failedFolders.insert(folderOf(item.oldPath));
            continue;
        }
        // The window may have edited the new path; check it again.
        if (m_library.absolutePath(item.trackId) != item.oldPath) {
            item.status = "failed";
            item.note = "The track moved since the plan was made";
            ++result.failed;
            result.items << item;
            continue;
        }
        bool copied = false;
        QString error;
        if (!moveFile(item.oldPath, item.newPath, &copied, &error)) {
            item.status = "failed";
            item.note = error;
            ++result.failed;
            result.items << item;
            failedFolders.insert(folderOf(item.oldPath));
            continue;
        }
        QSqlDatabase db = m_library.db();
        db.transaction();
        QString oldKey, newKey, why;
        bool ok = updatePath(item.trackId, item.newPath, &oldKey, &newKey, &why);
        if (ok) {
            journal(m_library, batch, item.trackId, "path", oldKey, newKey, "organise");
            ok = db.commit();
            if (!ok)
                why = db.lastError().text();
        }
        if (!ok) {
            db.rollback();
            // Put the file back, so the library and the disk still agree.
            bool back = false;
            QString ignored;
            moveFile(item.newPath, item.oldPath, &back, &ignored);
            item.status = "failed";
            item.note = why;
            ++result.failed;
            result.items << item;
            failedFolders.insert(folderOf(item.oldPath));
            continue;
        }
        journalled = true;
        item.status = "moved";
        if (!error.isEmpty())
            item.note = error;
        ++result.moved;
        if (copied)
            ++result.copied;
        emptied << folderOf(item.oldPath);
        for (CompanionMove &c : item.companions) {
            if (c.skip)
                continue;
            bool companionCopied = false;
            QString e;
            if (moveFile(c.from, c.to, &companionCopied, &e)) {
                journal(m_library, batch, item.trackId, "companion", c.from, c.to, "organise");
                ++result.companionsMoved;
            } else {
                c.skip = true;
                c.note = e;
            }
        }
        result.items << item;
    }

    for (const CompanionMove &c : plan.folderCompanions) {
        if (c.skip || failedFolders.contains(folderOf(c.from)) || result.cancelled)
            continue;
        bool copied = false;
        QString e;
        if (moveFile(c.from, c.to, &copied, &e)) {
            journal(m_library, batch, 0, "companion", c.from, c.to, "organise");
            ++result.companionsMoved;
            journalled = true;
        }
    }
    if (options.removeEmptyFolders && journalled)
        removeEmptyFolders(emptied, batch, "organise", &result.foldersRemoved);

    result.ok = result.failed == 0 && !result.cancelled;
    if (journalled)
        result.batch = batch;
    result.elapsedMs = timer.elapsed();
    Journal::finishJobRow(m_library, jobRow, result.ok, result.cancelled, result.toJson(),
                          result.failed ? QString("%1 files failed").arg(result.failed) : QString());
    if (m_job) {
        m_job->setProgress("Moving files", plan.items.size(), plan.items.size());
        if (result.failed)
            m_job->fail(QString("%1 files could not be moved").arg(result.failed));
        else
            m_job->finish();
    }
    return result;
}

OrganiseResult Organiser::undo(const QString &batch) {
    OrganiseResult result;
    QElapsedTimer timer;
    timer.start();
    QList<Journal::Entry> entries = Journal::entries(m_library, batch);
    std::reverse(entries.begin(), entries.end());
    bool any = false;
    for (const Journal::Entry &e : std::as_const(entries))
        any = any || !e.undone;
    if (!any) {
        result.ok = false;
        result.error = entries.isEmpty() ? QString("No batch called %1 in the journal").arg(batch)
                                         : QString("Batch %1 has nothing left to undo").arg(batch);
        return result;
    }
    const QString undoBatch = TagWriter::newBatchId("undo");
    const QString source = "undo:" + batch;
    const qint64 jobRow = Journal::startJobRow(m_library, "organise", {undoBatch, "undo", batch});
    QStringList emptied;
    QSqlQuery mark(m_library.db());
    mark.prepare("UPDATE tag_journal SET undone = 1 WHERE id = ?");
    int done = 0;
    for (const Journal::Entry &e : std::as_const(entries)) {
        if (m_job && m_job->isCancelled()) {
            result.cancelled = true;
            break;
        }
        if (m_job)
            m_job->setProgress("Moving files back", done++, entries.size());
        if (e.undone)
            continue;
        bool ok = false;
        if (e.field == "folder_removed") {
            if (Journal::insideSandbox(m_sandbox, e.oldValue))
                OwnWrites::noteFolder(e.oldValue);
            ok = Journal::insideSandbox(m_sandbox, e.oldValue) && QDir().mkpath(e.oldValue);
            if (ok)
                journal(m_library, undoBatch, 0, "folder_created", QString(), e.oldValue, source);
        } else if (e.field == "companion") {
            bool copied = false;
            QString error;
            ok = exists(e.newValue) && moveFile(e.newValue, e.oldValue, &copied, &error);
            if (ok) {
                journal(m_library, undoBatch, e.trackId, "companion", e.newValue, e.oldValue, source);
                emptied << folderOf(e.newValue);
                ++result.companionsMoved;
            }
        } else if (e.field == "path") {
            OrganiseItem item;
            item.trackId = e.trackId;
            item.oldPath = pathOf(m_library, e.newValue);
            item.newPath = pathOf(m_library, e.oldValue);
            bool copied = false;
            QString error;
            if (!e.trackId || item.oldPath.isEmpty() || item.newPath.isEmpty()) {
                error = "The track or its volume is gone";
            } else if (m_library.absolutePath(e.trackId) != item.oldPath) {
                error = "The track has moved again since";
            } else if (moveFile(item.oldPath, item.newPath, &copied, &error)) {
                QSqlDatabase db = m_library.db();
                db.transaction();
                QString oldKey, newKey;
                ok = updatePath(e.trackId, item.newPath, &oldKey, &newKey, &error);
                if (ok)
                    journal(m_library, undoBatch, e.trackId, "path", oldKey, newKey, source);
                ok = ok && db.commit();
                if (!ok) {
                    db.rollback();
                    bool back = false;
                    QString ignored;
                    moveFile(item.newPath, item.oldPath, &back, &ignored);
                }
            }
            item.status = ok ? "moved" : "failed";
            item.note = ok ? QString() : error;
            if (ok) {
                ++result.moved;
                if (copied)
                    ++result.copied;
                emptied << folderOf(item.oldPath);
            } else {
                ++result.failed;
            }
            result.items << item;
        } else {
            continue;
        }
        if (ok) {
            mark.addBindValue(e.id);
            mark.exec();
        }
    }
    removeEmptyFolders(emptied, undoBatch, source, &result.foldersRemoved);
    result.batch = undoBatch;
    result.ok = result.failed == 0 && !result.cancelled;
    result.elapsedMs = timer.elapsed();
    Journal::finishJobRow(m_library, jobRow, result.ok, result.cancelled, result.toJson(), {});
    return result;
}

QJsonObject undoBatch(Library &library, const QString &batch, Job *job) {
    QJsonObject o;
    if (Journal::isOrganise(library, batch)) {
        o = Organiser(library, job).undo(batch).toJson();
        o["kind"] = "organise";
    } else {
        o = TagWriter(library, job).undo(batch).toJson();
        o["kind"] = "tag";
    }
    o["undid"] = batch;
    return o;
}
