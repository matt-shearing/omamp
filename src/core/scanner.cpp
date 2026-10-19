#include "scanner.h"

#include "job.h"
#include "library.h"
#include "tagreader.h"
#include "volumes.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QScopeGuard>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>
#include <atomic>
#include <cerrno>
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
                  {"moved", moved},
                  {"missing", missing},
                  {"failed", failed},
                  {"skipped_mounts", skippedMounts},
                  {"folders_listed", foldersListed},
                  {"folders_skipped", foldersSkipped},
                  {"unreadable", QJsonArray::fromStringList(unreadable)},
                  {"failures", QJsonArray::fromStringList(failures.mid(0, 100))},
                  {"moved_paths", QJsonArray::fromStringList(movedPaths)},
                  {"elapsed_ms", elapsedMs},
                  {"cancelled", cancelled}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QJsonObject LocateResult::toJson() const {
    QJsonObject o{{"ok", ok},
                  {"searched", searched},
                  {"missing_before", missingBefore},
                  {"missing_after", missingAfter},
                  {"located", QJsonArray::fromStringList(located)},
                  {"elapsed_ms", elapsedMs}};
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
    QString folder;   // its folder, relative to the volume root (shared by the folder's files)
    qint64 size;
    qint64 mtimeNs;
};

struct Walk {
    QList<FileOnDisk> files;
    QHash<QString, qint64> folders; // listed folders holding audio: rel path -> mtime_ns
    QSet<QString> skipped;          // folders not listed, their modification time unchanged
    QStringList unreadable;         // rel paths that could not be listed
    QStringList vanished;           // rel paths that no longer exist
    qint64 skippedMounts = 0, listed = 0;
    const QHash<QString, qint64> *known = nullptr; // stored folder times, in ChangedFolders mode
    Job *job = nullptr;
};

qint64 mtimeNs(const struct stat &st) { return qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec; }

QString join(const QString &dir, const QString &name) { return dir.isEmpty() ? name : dir + '/' + name; }

QString absoluteOf(const QString &volumeRoot, const QString &rel) {
    if (rel.isEmpty())
        return volumeRoot;
    return (volumeRoot == "/" ? QString() : volumeRoot) + '/' + rel;
}

QString folderOf(const QString &rel) {
    const int slash = rel.lastIndexOf('/');
    return slash < 0 ? QString("") : rel.left(slash);
}

// POSIX directory reading: one fstatat per audio file and nanosecond times,
// which QDirIterator and QFileInfo don't give. Entries are filtered by name
// and d_type before any stat, so a folder of scans and booklets costs
// nothing, and a folder skipped by its modification time only has its
// subfolders looked at.
void walk(const QByteArray &abs, const QString &rel, dev_t rootDevice, bool recursive, Walk &w) {
    if (w.job && w.job->isCancelled())
        return;
    DIR *dir = opendir(abs.constData());
    if (!dir) {
        if (errno == ENOENT || errno == ENOTDIR)
            w.vanished << rel;
        else
            w.unreadable << rel;
        return;
    }
    const int fd = dirfd(dir);
    struct stat self {};
    fstat(fd, &self);
    const qint64 folderTime = mtimeNs(self);
    bool skipFiles = false;
    if (w.known) {
        const auto it = w.known->constFind(rel);
        skipFiles = it != w.known->constEnd() && it.value() == folderTime;
    }
    if (skipFiles)
        w.skipped.insert(rel);
    else
        ++w.listed;

    bool hasAudio = false;
    QList<QPair<QByteArray, QString>> subdirs;
    while (dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        // Hidden entries are skipped: dot folders, the volume marker, and the
        // ._ files macOS leaves on shares.
        if (name[0] == '.')
            continue;
        const unsigned char type = entry->d_type;
        const bool maybeDir = recursive && (type == DT_DIR || type == DT_UNKNOWN);
        const bool maybeFile = !skipFiles && (type == DT_REG || type == DT_LNK || type == DT_UNKNOWN);
        QString fileName;
        bool audioName = false;
        if (maybeFile) {
            fileName = QFile::decodeName(name);
            audioName = TagReader::isAudio(fileName);
        }
        if (!maybeDir && !audioName)
            continue;
        struct stat st {};
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISLNK(st.st_mode)) {
            // Linked files are followed; linked folders are not, so a link
            // back up the tree can't loop the walk.
            if (!audioName || fstatat(fd, name, &st, 0) != 0 || !S_ISREG(st.st_mode))
                continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!recursive)
                continue;
            if (st.st_dev != rootDevice) {
                ++w.skippedMounts;
                continue;
            }
            subdirs.append({abs + '/' + name, join(rel, QFile::decodeName(name))});
        } else if (S_ISREG(st.st_mode) && audioName) {
            w.files.append({join(rel, fileName), rel, qint64(st.st_size), mtimeNs(st)});
            hasAudio = true;
        }
    }
    closedir(dir);

    if (hasAudio)
        w.folders.insert(rel, folderTime);
    if (w.job && (w.listed + w.skipped.size()) % 64 == 0)
        w.job->setProgress("Looking for music", w.files.size(), 0);
    for (const auto &sub : subdirs)
        walk(sub.first, sub.second, rootDevice, true, w);
}

struct Existing {
    qint64 id;
    qint64 size;
    qint64 mtimeNs;
    int tagVersion;
    QString availability;
};

struct Pending {
    FileOnDisk file;
    qint64 volumeId;
    qint64 existingId; // 0 for a new file
};

struct ReadResult {
    Pending pending;
    QString absolute;
    TrackTags tags;
};

QVariant orNull(int v) { return v ? QVariant(v) : QVariant(QMetaType(QMetaType::Int)); }
QVariant orNull(const QString &s) { return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s); }
QVariant orNull(double v) { return std::isnan(v) ? QVariant(QMetaType(QMetaType::Double)) : QVariant(v); }
QVariant negativeIsNull(int v) { return v >= 0 ? QVariant(v) : QVariant(QMetaType(QMetaType::Int)); }

bool sameText(const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

// Finds the missing track a new file used to be. Candidates are tracks
// marked missing (their volume is online and their file was not where it
// should be), with the same size or the same content hash. A candidate
// matches on
//   - the same content hash and length, or
//   - the same size, length (within a second) and title, artist and album,
//     plus the same file name when the file has none of those tags.
// When more than one matches, the one with the same file name wins; when
// that still leaves several, nothing matches and the file is added as new,
// since guessing wrong would hand one track's history to another.
class MoveMatcher {
public:
    explicit MoveMatcher(QSqlDatabase db) : m_db(db) {
        QSqlQuery count(db);
        m_enabled = count.exec("SELECT EXISTS (SELECT 1 FROM tracks WHERE availability = 'missing')") &&
                    count.next() && count.value(0).toBool();
        m_find = QSqlQuery(db);
        m_find.setForwardOnly(true);
        m_find.prepare("SELECT t.id, t.size, t.duration_ms, t.title, t.artist, t.album, t.quick_hash, t.file_name, "
                       "v.root_path, t.rel_path FROM tracks t JOIN volumes v ON v.id = t.volume_id "
                       "WHERE t.availability = 'missing' AND (t.size = ? OR t.quick_hash = ?)");
    }

    bool enabled() const { return m_enabled; }

    qint64 find(qint64 size, const TrackTags &t, const QString &fileName, QString *oldPath) {
        if (!m_enabled)
            return 0;
        const QString artist = t.artists.join("; ");
        struct Candidate {
            qint64 id;
            bool sameName, byHash;
            QString path;
        };
        QList<Candidate> matches;
        m_find.addBindValue(size);
        m_find.addBindValue(t.quickHash.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(t.quickHash));
        if (!m_find.exec())
            return 0;
        while (m_find.next()) {
            const qint64 id = m_find.value(0).toLongLong();
            if (m_claimed.contains(id))
                continue;
            const qint64 duration = m_find.value(2).toLongLong();
            const bool closeLength = qAbs(duration - t.durationMs) <= 1000;
            if (!closeLength)
                continue;
            const QString title = m_find.value(3).toString(), oldArtist = m_find.value(4).toString(),
                          album = m_find.value(5).toString(), hash = m_find.value(6).toString(),
                          oldName = m_find.value(7).toString();
            const bool sameName = oldName == fileName;
            const bool byHash = !t.quickHash.isEmpty() && hash == t.quickHash;
            bool byTags = m_find.value(1).toLongLong() == size && sameText(title, t.title) &&
                          sameText(oldArtist, artist) && sameText(album, t.album);
            if (byTags && t.title.isEmpty() && artist.isEmpty() && t.album.isEmpty())
                byTags = sameName;
            if (!byHash && !byTags)
                continue;
            const QString root = m_find.value(8).toString();
            matches.append({id, sameName, byHash, absoluteOf(root, m_find.value(9).toString())});
        }
        m_find.finish();
        if (matches.size() > 1) {
            QList<Candidate> named;
            for (const Candidate &c : std::as_const(matches))
                if (c.sameName)
                    named << c;
            matches = named;
        }
        if (matches.size() != 1)
            return 0;
        m_claimed.insert(matches[0].id);
        if (oldPath)
            *oldPath = matches[0].path;
        return matches[0].id;
    }

private:
    QSqlDatabase m_db;
    QSqlQuery m_find;
    QSet<qint64> m_claimed;
    bool m_enabled = false;
};

// Writes read results into the database, with the lookups for artists,
// genres, albums and folders cached for the whole scan. One per volume.
class Writer {
public:
    Writer(QSqlDatabase db, qint64 volumeId) : m_db(db), m_volume(volumeId) {
        m_now = QDateTime::currentSecsSinceEpoch();
        static const char *fields =
            "volume_id = :volume_id, folder_id = :folder_id, rel_path = :rel_path, file_name = :file_name, "
            "format = :format, size = :size, mtime_ns = :mtime_ns, title = :title, artist = :artist, "
            "album_artist = :album_artist, sort_artist = :sort_artist, album = :album, album_id = :album_id, "
            "composer = :composer, genre = :genre, year = :year, original_year = :original_year, "
            "track_no = :track_no, track_total = :track_total, disc_no = :disc_no, disc_total = :disc_total, "
            "compilation = :compilation, comment = :comment, grouping = :grouping, publisher = :publisher, "
            "isrc = :isrc, bpm = :bpm, mb_track_id = :mb_track_id, mb_release_id = :mb_release_id, "
            "mb_artist_id = :mb_artist_id, mb_release_group_id = :mb_release_group_id, "
            "mb_album_artist_id = :mb_album_artist_id, duration_ms = :duration_ms, bitrate = :bitrate, "
            "sample_rate = :sample_rate, channels = :channels, rg_track_gain = :rg_track_gain, "
            "rg_track_peak = :rg_track_peak, rg_album_gain = :rg_album_gain, rg_album_peak = :rg_album_peak, "
            "has_embedded_art = :has_embedded_art, has_lyrics = :has_lyrics, tag_version = :tag_version, "
            "quick_hash = :quick_hash, artwork_id = NULL, "
            "content_type = :content_type, availability = 'online', updated = :now";
        // The file's rating and play count fill the library's only while
        // those are empty, and only when the file says something new (SET
        // reads the old tag_rating), so the library's own stats always win
        // and a rating cleared in Omamp stays cleared.
        static const char *stats =
            "rating = CASE WHEN rating IS NULL AND tag_rating IS NOT :tag_rating THEN :tag_rating ELSE rating END, "
            "play_count = CASE WHEN play_count = 0 AND tag_play_count IS NOT :tag_play_count "
            "THEN coalesce(:tag_play_count, 0) ELSE play_count END, "
            "tag_rating = :tag_rating, tag_play_count = :tag_play_count";
        m_update = QSqlQuery(db);
        m_update.prepare(QString("UPDATE tracks SET %1, %2 WHERE id = :id").arg(fields, stats));
        // The insert reuses the update's column list so the two can't drift apart.
        QStringList columns, values;
        for (const QString &assignment : QString(fields).split(", ")) {
            const QStringList parts = assignment.split(" = ");
            columns << parts[0];
            values << parts[1];
        }
        columns << "rating" << "play_count" << "tag_rating" << "tag_play_count";
        values << ":tag_rating" << "coalesce(:tag_play_count, 0)" << ":tag_rating" << ":tag_play_count";
        m_insert = QSqlQuery(db);
        m_insert.prepare(QString("INSERT INTO tracks (added, %1) VALUES (:now, %2)")
                             .arg(columns.join(", "), values.join(", ")));
        m_clearArtists = QSqlQuery(db);
        m_clearArtists.prepare("DELETE FROM track_artists WHERE track_id = ?");
        m_clearGenres = QSqlQuery(db);
        m_clearGenres.prepare("DELETE FROM track_genres WHERE track_id = ?");
        m_linkArtist = QSqlQuery(db);
        m_linkArtist.prepare("INSERT OR IGNORE INTO track_artists (track_id, artist_id, role, position) VALUES (?, ?, ?, ?)");
        m_linkGenre = QSqlQuery(db);
        m_linkGenre.prepare("INSERT OR IGNORE INTO track_genres (track_id, genre_id, position) VALUES (?, ?, ?)");
        m_albumArt = QSqlQuery(db);
        m_albumArt.prepare("UPDATE albums SET art_checked = NULL WHERE id = ?");
    }

    QString lastError;

    // movedFrom is set when the file takes over a missing track's row.
    bool write(const ReadResult &r, qint64 trackId, bool moved, ScanResult &result) {
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

        QSqlQuery &q = trackId ? m_update : m_insert;
        if (trackId)
            q.bindValue(":id", trackId);
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
        q.bindValue(":mb_release_group_id", orNull(t.mbReleaseGroupId));
        q.bindValue(":mb_album_artist_id", orNull(t.mbAlbumArtistId));
        q.bindValue(":duration_ms", t.durationMs ? QVariant(t.durationMs) : QVariant(QMetaType(QMetaType::LongLong)));
        q.bindValue(":bitrate", orNull(t.bitrate));
        q.bindValue(":sample_rate", orNull(t.sampleRate));
        q.bindValue(":channels", orNull(t.channels));
        q.bindValue(":rg_track_gain", orNull(t.rgTrackGain));
        q.bindValue(":rg_track_peak", orNull(t.rgTrackPeak));
        q.bindValue(":rg_album_gain", orNull(t.rgAlbumGain));
        q.bindValue(":rg_album_peak", orNull(t.rgAlbumPeak));
        q.bindValue(":has_embedded_art", t.hasEmbeddedArt ? 1 : 0);
        q.bindValue(":has_lyrics", t.hasLyrics ? 1 : 0);
        q.bindValue(":tag_version", TagReader::version);
        q.bindValue(":quick_hash", orNull(t.quickHash));
        q.bindValue(":tag_rating", negativeIsNull(t.tagRating));
        q.bindValue(":tag_play_count", negativeIsNull(t.tagPlayCount));
        q.bindValue(":content_type", t.contentType);
        if (!q.exec()) {
            lastError = q.lastError().text();
            return false;
        }

        qint64 id = trackId;
        if (id) {
            if (moved)
                ++result.moved;
            else
                ++result.updated;
            m_clearArtists.addBindValue(id);
            m_clearArtists.exec();
            m_clearGenres.addBindValue(id);
            m_clearGenres.exec();
        } else {
            id = q.lastInsertId().toLongLong();
            ++result.added;
        }
        // The album's picture may have changed with it; look again.
        if (album && !m_artChecked.contains(album)) {
            m_artChecked.insert(album);
            m_albumArt.addBindValue(album);
            m_albumArt.exec();
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
        if (!isRoot && !mtime) {
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
        q.prepare("INSERT INTO albums (title, album_artist, artist_key, title_key, year, compilation, mb_release_id, "
                  "mb_release_group_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                  "ON CONFLICT(artist_key, title_key) DO UPDATE SET year = coalesce(year, excluded.year), "
                  "mb_release_id = coalesce(mb_release_id, excluded.mb_release_id), "
                  "mb_release_group_id = coalesce(mb_release_group_id, excluded.mb_release_group_id) RETURNING id");
        q.addBindValue(t.album);
        q.addBindValue(orNull(display));
        q.addBindValue(artistKey);
        q.addBindValue(titleKey);
        q.addBindValue(orNull(t.year));
        q.addBindValue(t.compilation ? 1 : 0);
        q.addBindValue(orNull(t.mbReleaseId));
        q.addBindValue(orNull(t.mbReleaseGroupId));
        const qint64 id = q.exec() && q.next() ? q.value(0).toLongLong() : 0;
        q.finish(); // a RETURNING statement stays open until finished, which blocks COMMIT
        m_albums.insert(cacheKey, id);
        return id;
    }

    QSqlDatabase m_db;
    qint64 m_volume, m_now;
    QSqlQuery m_update, m_insert, m_clearArtists, m_clearGenres, m_linkArtist, m_linkGenre, m_albumArt;
    QHash<QString, qint64> m_artists, m_genres, m_albums, m_folders;
    QSet<qint64> m_artChecked;
};

bool underAny(const QString &rel, const QStringList &dirs) {
    for (const QString &dir : dirs) {
        if (dir.isEmpty() || rel == dir || rel.startsWith(dir + '/'))
            return true;
    }
    return false;
}

} // namespace

// One subtree (or one folder) of a volume to compare with the database.
struct Scanner::Scope {
    qint64 volumeId = 0;
    QString volumeRoot; // where the volume is mounted now
    QString rel;        // the folder, relative to the volume root ("" for the root)
    bool recursive = true;
    bool isRoot = false; // a library root, recorded as one
    bool walk = true;    // false for a root inside another root of the same run, already covered
    dev_t device = 0;    // the library root's filesystem, never left
};

// Everything gathered by the walks, read and written afterwards. Walking
// every scope before reading anything means a file moved between two
// scanned folders is already marked missing at its old place when its new
// place is read, whichever folder comes first.
struct Scanner::Run {
    QList<Pending> toRead;
    QList<QPair<qint64, QHash<QString, qint64>>> folderTimes;
    QHash<qint64, QString> volumeRoots;
};

QStringList Scanner::roots(Library &library) {
    QStringList out;
    for (const LibraryRoot &root : libraryRoots(library))
        out << root.path;
    return out;
}

QList<LibraryRoot> Scanner::libraryRoots(Library &library) {
    QList<LibraryRoot> out;
    QSqlQuery q(library.db());
    q.exec("SELECT v.root_path, f.rel_path, v.online, v.kind, v.fs_type, v.id FROM folders f "
           "JOIN volumes v ON v.id = f.volume_id WHERE f.is_root ORDER BY v.root_path, f.rel_path");
    while (q.next()) {
        LibraryRoot root;
        root.online = q.value(2).toBool();
        if (root.online)
            root.path = absoluteOf(q.value(0).toString(), q.value(1).toString());
        root.kind = q.value(3).toString();
        root.fsType = q.value(4).toString();
        root.volumeId = q.value(5).toLongLong();
        out << root;
    }
    return out;
}

ScanResult Scanner::rescanAll(Mode mode) {
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
    return scan(online, mode);
}

qint64 Scanner::startJob(const char *kind, const QStringList &args) {
    if (m_job)
        m_job->start();
    QSqlQuery job(m_library.db());
    job.prepare("INSERT INTO jobs (kind, state, args, created, started) VALUES (?, 'running', ?, ?, ?)");
    job.addBindValue(QString(kind));
    job.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact)));
    job.addBindValue(QDateTime::currentSecsSinceEpoch());
    job.addBindValue(QDateTime::currentSecsSinceEpoch());
    job.exec();
    return job.lastInsertId().toLongLong();
}

void Scanner::finishJob(qint64 jobId, const ScanResult &result) {
    QSqlQuery done(m_library.db());
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
}

static void tidy(QSqlDatabase db) {
    // Albums, artists and genres no track uses any more.
    QSqlQuery q(db);
    q.exec("DELETE FROM albums WHERE id NOT IN (SELECT album_id FROM tracks WHERE album_id IS NOT NULL)");
    q.exec("DELETE FROM artists WHERE id NOT IN (SELECT artist_id FROM track_artists)");
    q.exec("DELETE FROM genres WHERE id NOT IN (SELECT genre_id FROM track_genres)");
}

ScanResult Scanner::scan(const QStringList &roots, Mode mode) {
    ScanResult result;
    QElapsedTimer timer;
    timer.start();
    const qint64 jobId = startJob("scan", roots);

    // Volumes that came back since the last run get their tracks back online.
    m_library.refreshVolumes(m_resolver);
    QSqlDatabase db = m_library.db();

    // A root inside another root of this scan is covered by the outer walk;
    // walking it twice would read its files twice.
    QStringList canonical;
    for (const QString &rootArg : roots)
        canonical << QFileInfo(rootArg).canonicalFilePath();
    auto covered = [&](const QString &root) {
        for (const QString &other : std::as_const(canonical))
            if (!other.isEmpty() && other != root && root.startsWith(other == "/" ? "/" : other + '/'))
                return true;
        return false;
    };

    Run run;
    QSet<QString> seen;
    for (const QString &rootArg : roots) {
        const QFileInfo rootInfo(rootArg);
        const QString root = rootInfo.canonicalFilePath();
        if (!root.isEmpty() && seen.contains(root))
            continue;
        seen.insert(root);
        if (root.isEmpty() || !rootInfo.isDir()) {
            result.ok = false;
            result.error = QString("%1 is not a folder").arg(rootArg);
            break;
        }
        QString why;
        const VolumeInfo volume = m_resolver.identify(root, true, &why);
        if (!volume.isValid()) {
            result.ok = false;
            result.error = why;
            break;
        }
        result.roots << root;

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
            result.ok = false;
            result.error = v.lastError().text();
            break;
        }
        const qint64 volumeId = v.value(0).toLongLong();
        v.finish();
        // The volume may have been offline until now.
        QSqlQuery back(db);
        back.prepare("UPDATE tracks SET availability = 'online' WHERE volume_id = ? AND availability = 'offline'");
        back.addBindValue(volumeId);
        back.exec();

        struct stat rootStat {};
        if (stat(QFile::encodeName(root).constData(), &rootStat) != 0) {
            result.ok = false;
            result.error = QString("Can't read %1").arg(root);
            break;
        }
        Scope scope;
        scope.volumeId = volumeId;
        scope.volumeRoot = volume.rootPath;
        // An empty string, not a null one: Qt SQL would bind a null QString as NULL.
        scope.rel = root == volume.rootPath ? QString("")
                                            : root.mid(volume.rootPath == "/" ? 1 : volume.rootPath.size() + 1);
        scope.recursive = true;
        scope.isRoot = true;
        scope.walk = !covered(root);
        scope.device = rootStat.st_dev;
        if (!walkScope(scope, mode, run, result)) {
            result.ok = false;
            break;
        }
        if (m_job && m_job->isCancelled()) {
            result.cancelled = true;
            break;
        }
    }

    if (result.ok && !result.cancelled && !readAndWrite(run, result))
        result.ok = false;
    if (m_job && m_job->isCancelled())
        result.cancelled = true;
    if (result.ok && (result.updated || result.missing || result.moved))
        tidy(db);
    result.elapsedMs = timer.elapsed();
    finishJob(jobId, result);
    return result;
}

ScanResult Scanner::rescanFolders(const QList<FolderScan> &folders) {
    ScanResult result;
    QElapsedTimer timer;
    timer.start();
    QStringList paths;
    for (const FolderScan &f : folders)
        paths << f.path;
    const qint64 jobId = startJob("rescan", paths);
    m_library.refreshVolumes(m_resolver);

    // Each folder belongs to the deepest library root that holds it.
    const QList<LibraryRoot> roots = libraryRoots(m_library);
    QSqlQuery volumes(m_library.db());
    QHash<qint64, QString> volumeRoots;
    volumes.exec("SELECT id, root_path FROM volumes WHERE online");
    while (volumes.next())
        volumeRoots.insert(volumes.value(0).toLongLong(), volumes.value(1).toString());

    // A recursive folder covers everything below it, so drop what it covers.
    QList<FolderScan> wanted;
    for (const FolderScan &f : folders) {
        FolderScan clean{QDir::cleanPath(f.path), f.recursive};
        bool covered = false;
        for (FolderScan &w : wanted) {
            if (w.path == clean.path) {
                w.recursive = w.recursive || clean.recursive;
                covered = true;
            } else if (w.recursive && clean.path.startsWith(w.path + '/')) {
                covered = true;
            }
        }
        if (covered)
            continue;
        if (clean.recursive)
            wanted.removeIf([&](const FolderScan &w) { return w.path.startsWith(clean.path + '/'); });
        wanted << clean;
    }

    Run run;
    for (const FolderScan &f : std::as_const(wanted)) {
        const LibraryRoot *owner = nullptr;
        for (const LibraryRoot &root : roots) {
            if (!root.online || root.path.isEmpty())
                continue;
            if ((f.path == root.path || f.path.startsWith(root.path == "/" ? "/" : root.path + '/')) &&
                (!owner || root.path.size() > owner->path.size()))
                owner = &root;
        }
        // Outside the library, or on a volume that is offline: nothing to do.
        if (!owner || !volumeRoots.contains(owner->volumeId))
            continue;
        // The root itself must be there: an unmounted share must never turn
        // into missing tracks.
        struct stat rootStat {};
        if (stat(QFile::encodeName(owner->path).constData(), &rootStat) != 0 || !S_ISDIR(rootStat.st_mode)) {
            result.unreadable << owner->path;
            continue;
        }
        const QString volumeRoot = volumeRoots.value(owner->volumeId);
        Scope scope;
        scope.volumeId = owner->volumeId;
        scope.volumeRoot = volumeRoot;
        scope.rel = f.path == volumeRoot ? QString("") : f.path.mid(volumeRoot == "/" ? 1 : volumeRoot.size() + 1);
        scope.recursive = f.recursive;
        scope.device = rootStat.st_dev;
        result.roots << f.path;
        if (!walkScope(scope, Mode::Full, run, result)) {
            // One folder that can't be listed doesn't stop the others.
            result.error.clear();
            continue;
        }
        if (m_job && m_job->isCancelled())
            break;
    }

    if (!(m_job && m_job->isCancelled()) && !readAndWrite(run, result))
        result.ok = false;
    if (m_job && m_job->isCancelled())
        result.cancelled = true;
    if (result.ok && (result.updated || result.missing || result.moved))
        tidy(m_library.db());
    result.elapsedMs = timer.elapsed();
    finishJob(jobId, result);
    return result;
}

bool Scanner::walkScope(const Scope &scope, Mode mode, Run &run, ScanResult &result) {
    QSqlDatabase db = m_library.db();
    run.volumeRoots.insert(scope.volumeId, scope.volumeRoot);
    if (!scope.walk) {
        Writer(db, scope.volumeId).folderId(scope.rel, true, 0);
        return true;
    }

    // Stored folder times, for skipping folders that haven't changed.
    QHash<QString, qint64> known;
    if (mode == Mode::ChangedFolders) {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare("SELECT rel_path, mtime_ns FROM folders WHERE volume_id = ? AND mtime_ns IS NOT NULL");
        q.addBindValue(scope.volumeId);
        q.exec();
        while (q.next())
            known.insert(q.value(0).toString(), q.value(1).toLongLong());
    }

    // 1. Walk.
    Walk w;
    w.job = m_job;
    if (mode == Mode::ChangedFolders)
        w.known = &known;
    walk(QFile::encodeName(absoluteOf(scope.volumeRoot, scope.rel)), scope.rel, scope.device, scope.recursive, w);
    if (w.unreadable.contains(scope.rel)) {
        result.error = QString("Can't list %1").arg(absoluteOf(scope.volumeRoot, scope.rel));
        result.unreadable << absoluteOf(scope.volumeRoot, scope.rel);
        return false;
    }
    const bool vanished = w.vanished.contains(scope.rel);
    if (vanished && scope.isRoot) {
        result.error = QString("%1 is not a folder").arg(absoluteOf(scope.volumeRoot, scope.rel));
        return false;
    }
    result.skippedMounts += w.skippedMounts;
    result.foldersListed += w.listed;
    result.foldersSkipped += w.skipped.size();
    for (const QString &rel : std::as_const(w.unreadable))
        result.unreadable << absoluteOf(scope.volumeRoot, rel);
    result.files += w.files.size();
    if (m_job && m_job->isCancelled())
        return true;

    // 2. Compare with the database. A folder that vanished takes everything
    // below it with it, even when only its own files were asked about.
    const bool recursive = scope.recursive || vanished;
    QHash<QString, Existing> existing;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        const QString prefix = scope.rel.isEmpty() ? QString("") : scope.rel + '/';
        q.prepare(QString("SELECT id, rel_path, size, mtime_ns, availability, tag_version FROM tracks "
                          "WHERE volume_id = ? AND substr(rel_path, 1, ?) = ? %1")
                      .arg(recursive ? "" : "AND instr(substr(rel_path, ? + 1), '/') = 0"));
        q.addBindValue(scope.volumeId);
        q.addBindValue(prefix.size());
        q.addBindValue(prefix);
        if (!recursive)
            q.addBindValue(prefix.size());
        q.exec();
        while (q.next())
            existing.insert(q.value(1).toString(), {q.value(0).toLongLong(), q.value(2).toLongLong(),
                                                     q.value(3).toLongLong(), q.value(5).toInt(),
                                                     q.value(4).toString()});
    }

    QList<qint64> cameBack;
    for (const FileOnDisk &f : std::as_const(w.files)) {
        const auto it = existing.find(f.rel);
        if (it == existing.end()) {
            run.toRead.append({f, scope.volumeId, 0});
            continue;
        }
        // Rows an older reader filled are read again once for the new fields.
        if (it->size == f.size && it->mtimeNs == f.mtimeNs && it->tagVersion >= TagReader::version) {
            ++result.unchanged;
            if (it->availability != "online")
                cameBack << it->id;
        } else {
            run.toRead.append({f, scope.volumeId, it->id});
        }
        existing.erase(it);
    }

    // Whatever is left in existing was not found on disk, unless its folder
    // was skipped (unchanged) or couldn't be listed.
    QList<qint64> gone;
    for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
        if (w.skipped.contains(folderOf(it.key()))) {
            ++result.unchanged;
            continue;
        }
        if (it->availability == "missing" || underAny(it.key(), w.unreadable))
            continue;
        gone << it->id;
    }
    existing.clear();

    Writer writer(db, scope.volumeId);
    db.transaction();
    if (scope.isRoot)
        writer.folderId(scope.rel, true, 0);
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
    // Folder times are stored only once their files are in, so a cancelled
    // or failed scan never makes a later quick rescan skip a folder.
    run.folderTimes.append({scope.volumeId, w.folders});
    return true;
}

bool Scanner::readAndWrite(Run &run, ScanResult &result) {
    QSqlDatabase db = m_library.db();
    QHash<qint64, Writer *> writers;
    auto writerFor = [&](qint64 volume) {
        Writer *&w = writers[volume];
        if (!w)
            w = new Writer(db, volume);
        return w;
    };
    const auto cleanup = qScopeGuard([&] { qDeleteAll(writers); });

    bool ok = true;
    const qint64 total = run.toRead.size();
    if (total) {
        MoveMatcher matcher(db);
        QMutex mutex;
        QWaitCondition arrived;
        QList<ReadResult> ready;
        QThreadPool pool;
        pool.setMaxThreadCount(m_workers);
        Job *jobObject = m_job;

        // The pool is fed at most m_inFlight files ahead of the writer, so
        // neither its queue nor the results waiting to be written grow with
        // the size of the library.
        qint64 submitted = 0, written = 0;
        auto feed = [&] {
            while (submitted < total && submitted - written < m_inFlight) {
                const Pending p = run.toRead.at(submitted++);
                const QString absolute = absoluteOf(run.volumeRoots.value(p.volumeId), p.file.rel);
                pool.start([&, p, absolute] {
                    ReadResult r{p, absolute, {}};
                    if (!(jobObject && jobObject->isCancelled())) {
                        r.tags = TagReader::read(absolute);
                        r.tags.quickHash = TagReader::quickHash(absolute);
                    } else {
                        r.tags.error = "cancelled";
                    }
                    QMutexLocker lock(&mutex);
                    ready.append(std::move(r));
                    arrived.wakeOne();
                });
            }
        };

        while (written < total) {
            feed();
            QList<ReadResult> batch;
            {
                QMutexLocker lock(&mutex);
                // Wait for a worthwhile batch, but not so long that progress stalls.
                while (ready.size() < 500 && written + ready.size() < submitted)
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
                    result.failures << r.absolute;
                }
                if (!ok)
                    continue;
                qint64 trackId = r.pending.existingId;
                bool moved = false;
                if (!trackId) {
                    QString oldPath;
                    const QString fileName = r.pending.file.rel.section('/', -1);
                    const int dot = fileName.lastIndexOf('.');
                    trackId = matcher.find(r.pending.file.size, r.tags, dot > 0 ? fileName.left(dot) : fileName,
                                           &oldPath);
                    if (trackId) {
                        moved = true;
                        if (result.movedPaths.size() < 100)
                            result.movedPaths << oldPath + " -> " + r.absolute;
                    }
                }
                Writer *writer = writerFor(r.pending.volumeId);
                if (!writer->write(r, trackId, moved, result)) {
                    ok = false;
                    result.error = writer->lastError;
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
    }
    run.toRead.clear();

    if (ok && !(m_job && m_job->isCancelled())) {
        db.transaction();
        for (const auto &times : std::as_const(run.folderTimes))
            writerFor(times.first)->setFolderTimes(times.second);
        db.commit();
    }
    return ok;
}

LocateResult Scanner::locate(const QString &folderArg) {
    LocateResult out;
    QElapsedTimer timer;
    timer.start();
    const QFileInfo info(folderArg);
    const QString folder = info.canonicalFilePath();
    if (folder.isEmpty() || !info.isDir()) {
        out.ok = false;
        out.error = QString("%1 is not a folder").arg(folderArg);
        return out;
    }
    m_library.refreshVolumes(m_resolver);
    QSqlDatabase db = m_library.db();
    auto missingCount = [&] {
        QSqlQuery q(db);
        return q.exec("SELECT count(*) FROM tracks WHERE availability = 'missing'") && q.next()
                   ? q.value(0).toLongLong()
                   : 0;
    };
    out.missingBefore = missingCount();
    if (!out.missingBefore) {
        out.missingAfter = 0;
        out.elapsedMs = timer.elapsed();
        return out;
    }

    // The folder's volume, recorded if it is new (a disk the library has
    // never seen can hold the files that moved). No marker is written here.
    QString why;
    const VolumeInfo volume = m_resolver.identify(folder, false, &why);
    if (!volume.isValid()) {
        out.ok = false;
        out.error = why;
        return out;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery v(db);
    v.prepare("INSERT INTO volumes (key, kind, label, fs_type, source, mount_subpath, root_path, online, last_seen, added) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?, ?) "
              "ON CONFLICT(key) DO UPDATE SET root_path = excluded.root_path, online = 1, last_seen = excluded.last_seen "
              "RETURNING id");
    for (const QVariant &value : {QVariant(volume.key), QVariant(volume.kind), QVariant(volume.label),
                                  QVariant(volume.fsType), QVariant(volume.source), QVariant(volume.mountSubpath),
                                  QVariant(volume.rootPath), QVariant(now), QVariant(now)})
        v.addBindValue(value);
    if (!v.exec() || !v.next()) {
        out.ok = false;
        out.error = v.lastError().text();
        return out;
    }
    const qint64 volumeId = v.value(0).toLongLong();
    v.finish();

    struct stat rootStat {};
    stat(QFile::encodeName(folder).constData(), &rootStat);
    Walk w;
    w.job = m_job;
    const QString rel = folder == volume.rootPath ? QString("")
                                                  : folder.mid(volume.rootPath == "/" ? 1 : volume.rootPath.size() + 1);
    walk(QFile::encodeName(folder), rel, rootStat.st_dev, true, w);

    // Sizes of the missing tracks: only files of one of those sizes are
    // opened. (The content hash covers the size too, so nothing is lost.)
    QSet<qint64> sizes;
    {
        QSqlQuery q(db);
        q.exec("SELECT DISTINCT size FROM tracks WHERE availability = 'missing'");
        while (q.next())
            sizes.insert(q.value(0).toLongLong());
    }
    QSet<QString> inLibrary;
    {
        QSqlQuery q(db);
        q.prepare("SELECT rel_path FROM tracks WHERE volume_id = ?");
        q.addBindValue(volumeId);
        q.exec();
        while (q.next())
            inLibrary.insert(q.value(0).toString());
    }

    MoveMatcher matcher(db);
    Writer writer(db, volumeId);
    ScanResult scratch;
    db.transaction();
    for (const FileOnDisk &f : std::as_const(w.files)) {
        if (inLibrary.contains(f.rel))
            continue;
        ++out.searched;
        if (!sizes.contains(f.size))
            continue;
        const QString absolute = absoluteOf(volume.rootPath, f.rel);
        ReadResult r{{f, volumeId, 0}, absolute, TagReader::read(absolute)};
        r.tags.quickHash = TagReader::quickHash(absolute);
        QString oldPath;
        const QString fileName = f.rel.section('/', -1);
        const int dot = fileName.lastIndexOf('.');
        const qint64 id = matcher.find(f.size, r.tags, dot > 0 ? fileName.left(dot) : fileName, &oldPath);
        if (!id)
            continue;
        if (writer.write(r, id, true, scratch))
            out.located << oldPath + " -> " + absolute;
    }
    db.commit();
    if (!out.located.isEmpty())
        tidy(db);
    out.missingAfter = missingCount();
    out.elapsedMs = timer.elapsed();
    return out;
}
