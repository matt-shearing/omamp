#include "scanner.h"

#include "job.h"
#include "library.h"
#include "tagreader.h"
#include "volumes.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>
#include <atomic>
#include <cmath>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

QJsonObject ScanResult::toJson() const {
    QJsonObject o{{"ok", ok},
                  {"roots", QJsonArray::fromStringList(roots)},
                  {"files", files},
                  {"unchanged", unchanged},
                  {"added", added},
                  {"updated", updated},
                  {"missing", missing},
                  {"failed", failed},
                  {"skipped_mounts", skippedMounts},
                  {"unreadable", QJsonArray::fromStringList(unreadable)},
                  {"failures", QJsonArray::fromStringList(failures.mid(0, 100))},
                  {"elapsed_ms", elapsedMs},
                  {"cancelled", cancelled}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

Scanner::Scanner(Library &library, const VolumeResolver &resolver, Job *job)
    : m_library(library), m_resolver(resolver), m_job(job), m_workers(defaultWorkers()) {}

int Scanner::defaultWorkers() { return qBound(4, QThread::idealThreadCount() / 2, 8); }

namespace {

struct FileOnDisk {
    QString rel;      // relative to the volume root
    QString folder;   // its folder, relative to the volume root
    qint64 size;
    qint64 mtimeNs;
};

struct Walk {
    QList<FileOnDisk> files;
    QHash<QString, qint64> folders; // folder rel path -> mtime_ns, folders holding audio only
    QStringList unreadable;         // rel paths
    qint64 skippedMounts = 0;
    Job *job = nullptr;
};

qint64 mtimeNs(const struct stat &st) { return qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec; }

QString join(const QString &dir, const QString &name) { return dir.isEmpty() ? name : dir + '/' + name; }

// POSIX directory reading: one fstatat per entry and nanosecond times, which
// QDirIterator and QFileInfo don't give.
void walk(const QByteArray &abs, const QString &rel, dev_t rootDevice, Walk &w) {
    if (w.job && w.job->isCancelled())
        return;
    DIR *dir = opendir(abs.constData());
    if (!dir) {
        w.unreadable << rel;
        return;
    }
    const int fd = dirfd(dir);
    struct stat self {};
    fstat(fd, &self);
    bool hasAudio = false;
    QList<QPair<QByteArray, QString>> subdirs;

    while (dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        // Hidden entries are skipped: dot folders, the volume marker, and the
        // ._ files macOS leaves on shares.
        if (name[0] == '.')
            continue;
        struct stat st {};
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISLNK(st.st_mode)) {
            // Linked files are followed; linked folders are not, so a link
            // back up the tree can't loop the walk.
            if (fstatat(fd, name, &st, 0) != 0 || !S_ISREG(st.st_mode))
                continue;
        }
        const QString fileName = QFile::decodeName(name);
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != rootDevice) {
                ++w.skippedMounts;
                continue;
            }
            subdirs.append({abs + '/' + name, join(rel, fileName)});
        } else if (S_ISREG(st.st_mode) && TagReader::isAudio(fileName)) {
            w.files.append({join(rel, fileName), rel, qint64(st.st_size), mtimeNs(st)});
            hasAudio = true;
        }
    }
    closedir(dir);

    if (hasAudio)
        w.folders.insert(rel, mtimeNs(self));
    if (w.job && w.files.size() % 1000 < 50)
        w.job->setProgress("Looking for music", w.files.size(), 0);
    for (const auto &sub : subdirs)
        walk(sub.first, sub.second, rootDevice, w);
}

struct Existing {
    qint64 id;
    qint64 size;
    qint64 mtimeNs;
    QString availability;
};

struct Pending {
    FileOnDisk file;
    QString absolute;
    qint64 existingId; // 0 for a new file
};

struct ReadResult {
    Pending pending;
    TrackTags tags;
};

QVariant orNull(int v) { return v ? QVariant(v) : QVariant(QMetaType(QMetaType::Int)); }
QVariant orNull(const QString &s) { return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s); }
QVariant orNull(double v) { return std::isnan(v) ? QVariant(QMetaType(QMetaType::Double)) : QVariant(v); }

// Writes read results into the database, with the lookups for artists,
// genres, albums and folders cached for the whole scan.
class Writer {
public:
    Writer(QSqlDatabase db, qint64 volumeId) : m_db(db), m_volume(volumeId) {
        m_now = QDateTime::currentSecsSinceEpoch();
        static const char *fields =
            "folder_id = :folder_id, rel_path = :rel_path, file_name = :file_name, format = :format, "
            "size = :size, mtime_ns = :mtime_ns, title = :title, artist = :artist, album_artist = :album_artist, "
            "sort_artist = :sort_artist, album = :album, album_id = :album_id, composer = :composer, "
            "genre = :genre, year = :year, original_year = :original_year, track_no = :track_no, "
            "track_total = :track_total, disc_no = :disc_no, disc_total = :disc_total, "
            "compilation = :compilation, comment = :comment, grouping = :grouping, publisher = :publisher, "
            "isrc = :isrc, bpm = :bpm, mb_track_id = :mb_track_id, mb_release_id = :mb_release_id, "
            "mb_artist_id = :mb_artist_id, duration_ms = :duration_ms, bitrate = :bitrate, "
            "sample_rate = :sample_rate, channels = :channels, rg_track_gain = :rg_track_gain, "
            "rg_track_peak = :rg_track_peak, rg_album_gain = :rg_album_gain, rg_album_peak = :rg_album_peak, "
            "content_type = :content_type, availability = 'online', updated = :now";
        m_update = QSqlQuery(db);
        m_update.prepare(QString("UPDATE tracks SET %1 WHERE id = :id").arg(fields));
        // The insert reuses the update's column list so the two can't drift apart.
        QStringList columns, values;
        for (const QString &assignment : QString(fields).split(", ")) {
            const QStringList parts = assignment.split(" = ");
            columns << parts[0];
            values << parts[1];
        }
        m_insert = QSqlQuery(db);
        m_insert.prepare(QString("INSERT INTO tracks (volume_id, added, %1) VALUES (:volume_id, :now, %2)")
                             .arg(columns.join(", "), values.join(", ")));
        m_clearArtists = QSqlQuery(db);
        m_clearArtists.prepare("DELETE FROM track_artists WHERE track_id = ?");
        m_clearGenres = QSqlQuery(db);
        m_clearGenres.prepare("DELETE FROM track_genres WHERE track_id = ?");
        m_linkArtist = QSqlQuery(db);
        m_linkArtist.prepare("INSERT OR IGNORE INTO track_artists (track_id, artist_id, role, position) VALUES (?, ?, ?, ?)");
        m_linkGenre = QSqlQuery(db);
        m_linkGenre.prepare("INSERT OR IGNORE INTO track_genres (track_id, genre_id, position) VALUES (?, ?, ?)");
    }

    QString lastError;

    bool write(const ReadResult &r, ScanResult &result) {
        const TrackTags &t = r.tags;
        const FileOnDisk &f = r.pending.file;
        const qint64 folder = folderId(f.folder);
        const QString artist = t.artists.join("; ");
        const QString albumArtist = t.albumArtists.join("; ");
        QString sortArtist = !albumArtist.isEmpty() ? albumArtist : artist;
        if (albumArtist.isEmpty() && t.compilation)
            sortArtist = "Various Artists";
        const QString fileName = f.rel.section('/', -1);
        const int dot = fileName.lastIndexOf('.');

        QSqlQuery &q = r.pending.existingId ? m_update : m_insert;
        if (r.pending.existingId)
            q.bindValue(":id", r.pending.existingId);
        else
            q.bindValue(":volume_id", m_volume);
        q.bindValue(":now", m_now);
        q.bindValue(":folder_id", folder ? QVariant(folder) : QVariant(QMetaType(QMetaType::LongLong)));
        q.bindValue(":rel_path", f.rel);
        q.bindValue(":file_name", dot > 0 ? fileName.left(dot) : fileName);
        q.bindValue(":format", dot > 0 ? fileName.mid(dot + 1).toLower() : QString());
        q.bindValue(":size", f.size);
        q.bindValue(":mtime_ns", f.mtimeNs);
        q.bindValue(":title", orNull(t.title));
        q.bindValue(":artist", orNull(artist));
        q.bindValue(":album_artist", orNull(albumArtist));
        q.bindValue(":sort_artist", orNull(sortArtist));
        q.bindValue(":album", orNull(t.album));
        const qint64 album = albumId(t, albumArtist, artist, folder);
        q.bindValue(":album_id", album ? QVariant(album) : QVariant(QMetaType(QMetaType::LongLong)));
        q.bindValue(":composer", orNull(t.composers.join("; ")));
        q.bindValue(":genre", orNull(t.genres.join("; ")));
        q.bindValue(":year", orNull(t.year));
        q.bindValue(":original_year", orNull(t.originalYear));
        q.bindValue(":track_no", orNull(t.trackNo));
        q.bindValue(":track_total", orNull(t.trackTotal));
        q.bindValue(":disc_no", orNull(t.discNo));
        q.bindValue(":disc_total", orNull(t.discTotal));
        q.bindValue(":compilation", t.compilation ? 1 : 0);
        q.bindValue(":comment", orNull(t.comment));
        q.bindValue(":grouping", orNull(t.grouping));
        q.bindValue(":publisher", orNull(t.publisher));
        q.bindValue(":isrc", orNull(t.isrc));
        q.bindValue(":bpm", orNull(t.bpm));
        q.bindValue(":mb_track_id", orNull(t.mbTrackId));
        q.bindValue(":mb_release_id", orNull(t.mbReleaseId));
        q.bindValue(":mb_artist_id", orNull(t.mbArtistId));
        q.bindValue(":duration_ms", t.durationMs ? QVariant(t.durationMs) : QVariant(QMetaType(QMetaType::LongLong)));
        q.bindValue(":bitrate", orNull(t.bitrate));
        q.bindValue(":sample_rate", orNull(t.sampleRate));
        q.bindValue(":channels", orNull(t.channels));
        q.bindValue(":rg_track_gain", orNull(t.rgTrackGain));
        q.bindValue(":rg_track_peak", orNull(t.rgTrackPeak));
        q.bindValue(":rg_album_gain", orNull(t.rgAlbumGain));
        q.bindValue(":rg_album_peak", orNull(t.rgAlbumPeak));
        q.bindValue(":content_type", t.contentType);
        if (!q.exec()) {
            lastError = q.lastError().text();
            return false;
        }

        qint64 id = r.pending.existingId;
        if (id) {
            ++result.updated;
            m_clearArtists.addBindValue(id);
            m_clearArtists.exec();
            m_clearGenres.addBindValue(id);
            m_clearGenres.exec();
        } else {
            id = q.lastInsertId().toLongLong();
            ++result.added;
        }
        link(id, t.artists, "artist");
        link(id, t.albumArtists, "album_artist");
        link(id, t.composers, "composer");
        for (int i = 0; i < t.genres.size(); ++i) {
            m_linkGenre.addBindValue(id);
            m_linkGenre.addBindValue(named(m_genres, "genres", t.genres[i]));
            m_linkGenre.addBindValue(i);
            m_linkGenre.exec();
        }
        return true;
    }

    qint64 folderId(const QString &rel, bool isRoot = false, qint64 mtime = 0) {
        if (!isRoot) {
            const auto it = m_folders.constFind(rel);
            if (it != m_folders.constEnd())
                return it.value();
        }
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO folders (volume_id, rel_path, mtime_ns, is_root, added) VALUES (?, ?, ?, ?, ?) "
                  "ON CONFLICT(volume_id, rel_path) DO UPDATE SET "
                  "mtime_ns = coalesce(excluded.mtime_ns, mtime_ns), is_root = max(is_root, excluded.is_root) "
                  "RETURNING id");
        q.addBindValue(m_volume);
        q.addBindValue(rel);
        q.addBindValue(mtime ? QVariant(mtime) : QVariant(QMetaType(QMetaType::LongLong)));
        q.addBindValue(isRoot ? 1 : 0);
        q.addBindValue(m_now);
        const qint64 id = q.exec() && q.next() ? q.value(0).toLongLong() : 0;
        q.finish(); // a RETURNING statement stays open until finished, which blocks COMMIT
        m_folders.insert(rel, id);
        return id;
    }

    void setFolderTimes(const QHash<QString, qint64> &folders) {
        for (auto it = folders.constBegin(); it != folders.constEnd(); ++it)
            folderId(it.key(), false, it.value());
    }

private:
    void link(qint64 track, const QStringList &names, const char *role) {
        for (int i = 0; i < names.size(); ++i) {
            m_linkArtist.addBindValue(track);
            m_linkArtist.addBindValue(named(m_artists, "artists", names[i]));
            m_linkArtist.addBindValue(QString(role));
            m_linkArtist.addBindValue(i);
            m_linkArtist.exec();
        }
    }

    // Finds or creates a row by name in artists or genres. Names compare
    // without case, as the column's NOCASE collation does.
    qint64 named(QHash<QString, qint64> &cache, const char *table, const QString &name) {
        const QString key = name.toLower();
        const auto it = cache.constFind(key);
        if (it != cache.constEnd())
            return it.value();
        QSqlQuery q(m_db);
        q.prepare(QString("INSERT INTO %1 (name) VALUES (?) ON CONFLICT(name) DO UPDATE SET name = name RETURNING id")
                      .arg(table));
        q.addBindValue(name);
        const qint64 id = q.exec() && q.next() ? q.value(0).toLongLong() : 0;
        q.finish(); // a RETURNING statement stays open until finished, which blocks COMMIT
        cache.insert(key, id);
        return id;
    }

    // An album is its title plus its album artist; a compilation's album
    // artist is "Various Artists". Without an album artist the folder stands
    // in, so an album whose tracks each name different guests stays whole.
    qint64 albumId(const TrackTags &t, const QString &albumArtist, const QString &artist, qint64 folder) {
        if (t.album.isEmpty())
            return 0;
        QString display = albumArtist;
        QString artistKey = albumArtist.toLower();
        if (albumArtist.isEmpty()) {
            if (t.compilation) {
                display = "Various Artists";
                artistKey = "various artists";
            } else {
                display = artist;
                artistKey = QString("folder:%1").arg(folder);
            }
        }
        const QString titleKey = t.album.toLower();
        const QString cacheKey = artistKey + '\x1f' + titleKey;
        const auto it = m_albums.constFind(cacheKey);
        if (it != m_albums.constEnd())
            return it.value();
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO albums (title, album_artist, artist_key, title_key, year, compilation, mb_release_id) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?) "
                  "ON CONFLICT(artist_key, title_key) DO UPDATE SET year = coalesce(year, excluded.year) RETURNING id");
        q.addBindValue(t.album);
        q.addBindValue(orNull(display));
        q.addBindValue(artistKey);
        q.addBindValue(titleKey);
        q.addBindValue(orNull(t.year));
        q.addBindValue(t.compilation ? 1 : 0);
        q.addBindValue(orNull(t.mbReleaseId));
        const qint64 id = q.exec() && q.next() ? q.value(0).toLongLong() : 0;
        q.finish(); // a RETURNING statement stays open until finished, which blocks COMMIT
        m_albums.insert(cacheKey, id);
        return id;
    }

    QSqlDatabase m_db;
    qint64 m_volume, m_now;
    QSqlQuery m_update, m_insert, m_clearArtists, m_clearGenres, m_linkArtist, m_linkGenre;
    QHash<QString, qint64> m_artists, m_genres, m_albums, m_folders;
};

bool underAny(const QString &rel, const QStringList &dirs) {
    for (const QString &dir : dirs) {
        if (dir.isEmpty() || rel.startsWith(dir + '/'))
            return true;
    }
    return false;
}

} // namespace

QStringList Scanner::roots(Library &library) {
    QStringList out;
    QSqlQuery q(library.db());
    q.exec("SELECT v.root_path, f.rel_path, v.online FROM folders f JOIN volumes v ON v.id = f.volume_id "
           "WHERE f.is_root ORDER BY v.root_path, f.rel_path");
    while (q.next()) {
        const QString root = q.value(0).toString(), rel = q.value(1).toString();
        if (!q.value(2).toBool())
            out << QString();
        else
            out << (rel.isEmpty() ? root : (root == "/" ? '/' + rel : root + '/' + rel));
    }
    return out;
}

ScanResult Scanner::rescanAll() {
    m_library.refreshVolumes(m_resolver);
    QStringList online;
    for (const QString &root : roots(m_library))
        if (!root.isEmpty())
            online << root;
    if (online.isEmpty()) {
        ScanResult result;
        result.ok = false;
        result.error = "The library has no folders that are online. Add one with: omamp scan <folder>";
        return result;
    }
    return scan(online);
}

ScanResult Scanner::scan(const QStringList &roots) {
    ScanResult result;
    QElapsedTimer timer;
    timer.start();
    if (m_job)
        m_job->start();

    QSqlDatabase db = m_library.db();
    QSqlQuery job(db);
    job.prepare("INSERT INTO jobs (kind, state, args, created, started) VALUES ('scan', 'running', ?, ?, ?)");
    job.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(roots)).toJson(QJsonDocument::Compact)));
    job.addBindValue(QDateTime::currentSecsSinceEpoch());
    job.addBindValue(QDateTime::currentSecsSinceEpoch());
    job.exec();
    const qint64 jobId = job.lastInsertId().toLongLong();

    // Volumes that came back since the last run get their tracks back online.
    m_library.refreshVolumes(m_resolver);

    for (const QString &root : roots) {
        if (!scanRoot(root, result)) {
            result.ok = false;
            break;
        }
        if (m_job && m_job->isCancelled()) {
            result.cancelled = true;
            break;
        }
    }

    // Albums, artists and genres no track uses any more.
    if (result.ok && (result.updated || result.missing)) {
        QSqlQuery tidy(db);
        tidy.exec("DELETE FROM albums WHERE id NOT IN (SELECT album_id FROM tracks WHERE album_id IS NOT NULL)");
        tidy.exec("DELETE FROM artists WHERE id NOT IN (SELECT artist_id FROM track_artists)");
        tidy.exec("DELETE FROM genres WHERE id NOT IN (SELECT genre_id FROM track_genres)");
    }

    result.elapsedMs = timer.elapsed();
    QSqlQuery done(db);
    done.prepare("UPDATE jobs SET state = ?, result = ?, error = ?, progress = ?, total = ?, finished = ? WHERE id = ?");
    done.addBindValue(result.cancelled ? "cancelled" : (result.ok ? "done" : "failed"));
    done.addBindValue(QString::fromUtf8(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact)));
    done.addBindValue(orNull(result.error));
    done.addBindValue(result.files);
    done.addBindValue(result.files);
    done.addBindValue(QDateTime::currentSecsSinceEpoch());
    done.addBindValue(jobId);
    done.exec();

    if (m_job) {
        if (result.ok)
            m_job->finish();
        else
            m_job->fail(result.error);
    }
    return result;
}

bool Scanner::scanRoot(const QString &rootArg, ScanResult &result) {
    const QFileInfo rootInfo(rootArg);
    const QString root = rootInfo.canonicalFilePath();
    if (root.isEmpty() || !rootInfo.isDir()) {
        result.error = QString("%1 is not a folder").arg(rootArg);
        return false;
    }
    QString why;
    const VolumeInfo volume = m_resolver.identify(root, true, &why);
    if (!volume.isValid()) {
        result.error = why;
        return false;
    }
    result.roots << root;

    QSqlDatabase db = m_library.db();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery v(db);
    v.prepare("INSERT INTO volumes (key, kind, label, fs_type, source, mount_subpath, root_path, online, last_seen, added) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?, ?) "
              "ON CONFLICT(key) DO UPDATE SET root_path = excluded.root_path, mount_subpath = excluded.mount_subpath, "
              "fs_type = excluded.fs_type, source = excluded.source, online = 1, last_seen = excluded.last_seen "
              "RETURNING id");
    for (const QVariant &value : {QVariant(volume.key), QVariant(volume.kind), QVariant(volume.label),
                                  QVariant(volume.fsType), QVariant(volume.source), QVariant(volume.mountSubpath),
                                  QVariant(volume.rootPath), QVariant(now), QVariant(now)})
        v.addBindValue(value);
    if (!v.exec() || !v.next()) {
        result.error = v.lastError().text();
        return false;
    }
    const qint64 volumeId = v.value(0).toLongLong();
    v.finish();
    // The volume may have been offline until now.
    QSqlQuery back(db);
    back.prepare("UPDATE tracks SET availability = 'online' WHERE volume_id = ? AND availability = 'offline'");
    back.addBindValue(volumeId);
    back.exec();

    // An empty string, not a null one: Qt SQL would bind a null QString as NULL.
    const QString rootRel = root == volume.rootPath ? QString("")
                                                    : root.mid(volume.rootPath == "/" ? 1 : volume.rootPath.size() + 1);

    // 1. Walk.
    struct stat rootStat {};
    const QByteArray rootBytes = QFile::encodeName(root);
    if (stat(rootBytes.constData(), &rootStat) != 0) {
        result.error = QString("Can't read %1").arg(root);
        return false;
    }
    Walk w;
    w.job = m_job;
    walk(rootBytes, rootRel, rootStat.st_dev, w);
    if (w.unreadable.contains(rootRel)) {
        result.error = QString("Can't list %1").arg(root);
        return false;
    }
    result.skippedMounts += w.skippedMounts;
    for (const QString &rel : std::as_const(w.unreadable))
        result.unreadable << (volume.rootPath == "/" ? '/' + rel : volume.rootPath + '/' + rel);
    result.files += w.files.size();
    if (m_job && m_job->isCancelled())
        return true;

    // 2. Compare with the database.
    QHash<QString, Existing> existing;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare("SELECT id, rel_path, size, mtime_ns, availability FROM tracks "
                  "WHERE volume_id = ? AND (? = '' OR substr(rel_path, 1, ?) = ?)");
        q.addBindValue(volumeId);
        q.addBindValue(rootRel);
        q.addBindValue(rootRel.size() + 1);
        q.addBindValue(rootRel + '/');
        q.exec();
        while (q.next())
            existing.insert(q.value(1).toString(), {q.value(0).toLongLong(), q.value(2).toLongLong(),
                                                     q.value(3).toLongLong(), q.value(4).toString()});
    }

    QList<Pending> toRead;
    QList<qint64> cameBack;
    for (const FileOnDisk &f : std::as_const(w.files)) {
        const auto it = existing.find(f.rel);
        if (it == existing.end()) {
            toRead.append({f, (volume.rootPath == "/" ? QString() : volume.rootPath) + '/' + f.rel, 0});
            continue;
        }
        if (it->size == f.size && it->mtimeNs == f.mtimeNs) {
            ++result.unchanged;
            if (it->availability != "online")
                cameBack << it->id;
        } else {
            toRead.append({f, (volume.rootPath == "/" ? QString() : volume.rootPath) + '/' + f.rel, it->id});
        }
        existing.erase(it);
    }

    // Whatever is left in existing was not found on disk.
    QList<qint64> gone;
    for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
        if (it->availability == "missing" || underAny(it.key(), w.unreadable))
            continue;
        gone << it->id;
    }

    Writer writer(db, volumeId);
    db.transaction();
    writer.folderId(rootRel, true, 0);
    writer.setFolderTimes(w.folders);
    QSqlQuery mark(db);
    mark.prepare("UPDATE tracks SET availability = ? WHERE id = ?");
    for (qint64 id : std::as_const(cameBack)) {
        mark.addBindValue("online");
        mark.addBindValue(id);
        mark.exec();
    }
    if (!(m_job && m_job->isCancelled())) {
        for (qint64 id : std::as_const(gone)) {
            mark.addBindValue("missing");
            mark.addBindValue(id);
            mark.exec();
        }
        result.missing += gone.size();
    }
    if (!db.commit()) {
        result.error = db.lastError().text();
        return false;
    }

    // 3. Read tags on the pool and write them in batches as they arrive.
    if (toRead.isEmpty())
        return true;
    QMutex mutex;
    QWaitCondition arrived;
    QList<ReadResult> ready;
    QThreadPool pool;
    pool.setMaxThreadCount(m_workers);
    Job *jobObject = m_job;
    for (const Pending &p : std::as_const(toRead)) {
        pool.start([&, p] {
            ReadResult r{p, {}};
            if (!(jobObject && jobObject->isCancelled()))
                r.tags = TagReader::read(p.absolute);
            else
                r.tags.error = "cancelled";
            QMutexLocker lock(&mutex);
            ready.append(std::move(r));
            arrived.wakeOne();
        });
    }

    const qint64 total = toRead.size();
    qint64 written = 0;
    bool ok = true;
    while (written < total) {
        QList<ReadResult> batch;
        {
            QMutexLocker lock(&mutex);
            // Wait for a worthwhile batch, but not so long that progress stalls.
            while (ready.size() < 500 && written + ready.size() < total)
                if (!arrived.wait(&mutex, 200) && !ready.isEmpty())
                    break;
            batch.swap(ready);
        }
        db.transaction();
        for (const ReadResult &r : std::as_const(batch)) {
            if (r.tags.error == "cancelled")
                continue;
            if (!r.tags.ok) {
                ++result.failed;
                result.failures << r.pending.absolute;
            }
            if (ok && !writer.write(r, result)) {
                ok = false;
                result.error = writer.lastError;
            }
        }
        if (!db.commit() && ok) {
            ok = false;
            result.error = db.lastError().text();
        }
        written += batch.size();
        if (m_job)
            m_job->setProgress("Reading tags", written, total);
    }
    pool.waitForDone();
    if (m_job && m_job->isCancelled())
        result.cancelled = true;
    return ok;
}
