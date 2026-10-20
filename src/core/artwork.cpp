#include "artwork.h"

#include "job.h"
#include "library.h"
#include "remote.h"
#include "tagreader.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <atomic>
#include <climits>
#include <memory>

namespace {

// Image files beside the music, best first. MediaMonkey, foobar2000 and
// most players look for the same names.
const QStringList pictureNames = {"cover", "folder", "front"};
const QStringList pictureExtensions = {"jpg", "jpeg", "png", "webp", "bmp", "gif"};
constexpr qint64 maxPictureBytes = 40 * 1024 * 1024;

QString absoluteOf(const QString &root, const QString &rel) { return (root == "/" ? QString() : root) + '/' + rel; }

// The best picture file in a folder, or empty.
QString pictureIn(const QString &folder) {
    QString best;
    int bestRank = INT_MAX;
    const QStringList entries = QDir(folder).entryList(QDir::Files | QDir::Readable);
    for (const QString &entry : entries) {
        const int dot = entry.lastIndexOf('.');
        if (dot <= 0)
            continue;
        const int name = pictureNames.indexOf(entry.left(dot).toLower());
        const int ext = pictureExtensions.indexOf(entry.mid(dot + 1).toLower());
        if (name < 0 || ext < 0)
            continue;
        const int rank = name * 10 + ext;
        if (rank < bestRank) {
            bestRank = rank;
            best = folder + '/' + entry;
        }
    }
    return best;
}

QVariant nullInt() { return QVariant(QMetaType(QMetaType::LongLong)); }

} // namespace

ArtworkCache::ArtworkCache(Library &library, const QString &cacheDir)
    : m_library(library), m_dir(cacheDir.isEmpty() ? defaultCacheDir() : cacheDir) {}

QString ArtworkCache::defaultCacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omamp/art";
}

const QList<int> &ArtworkCache::sizes() {
    static const QList<int> list = {96, 256, 600};
    return list;
}

int ArtworkCache::snapSize(int requested) {
    if (requested <= 0)
        return 256;
    for (int size : sizes())
        if (size >= requested)
            return size;
    return sizes().last();
}

QString ArtworkCache::pictureBeside(const QString &folder) {
    const QString here = pictureIn(folder);
    if (!here.isEmpty())
        return here;
    static const QRegularExpression discFolder("^(cd|disc|disk)\\s*\\d+$", QRegularExpression::CaseInsensitiveOption);
    if (discFolder.match(QFileInfo(folder).fileName()).hasMatch())
        return pictureIn(QFileInfo(folder).path());
    return {};
}

QString ArtworkCache::thumbnailPath(const QString &hash, int size) const {
    return QString("%1/%2-%3.jpg").arg(m_dir, hash).arg(size);
}

QString ArtworkCache::artworkFor(Kind kind, qint64 id, int size) {
    m_error.clear();
    size = snapSize(size);
    if (kind == Kind::Album)
        return albumPicture(id, size);

    QSqlQuery q(m_library.db());
    q.prepare("SELECT t.album_id, t.artwork_id, t.has_embedded_art, v.root_path, t.rel_path, v.online, t.availability "
              "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ?");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) {
        m_error = "No such track";
        return {};
    }
    const qint64 album = q.value(0).toLongLong();
    const qint64 artwork = q.value(1).toLongLong();
    const bool embedded = q.value(2).toBool();
    const bool online = q.value(5).toBool() && q.value(6).toString() == "online";
    const QString path = absoluteOf(q.value(3).toString(), q.value(4).toString());
    q.finish();

    if (artwork) {
        const QString made = fromArtworkRow(artwork, size);
        if (!made.isEmpty())
            return made;
    }
    if (embedded && online) {
        Picture picture;
        picture.bytes = TagReader::embeddedPicture(path, &picture.mime);
        picture.source = "embedded:" + path;
        QString made;
        const qint64 stored = picture.bytes.isEmpty() ? 0 : store(picture, size, &made);
        if (stored) {
            QSqlQuery u(m_library.db());
            u.prepare("UPDATE tracks SET artwork_id = ? WHERE id = ?");
            u.addBindValue(stored);
            u.addBindValue(id);
            u.exec();
            return made;
        }
    }
    return album ? albumPicture(album, size) : QString();
}

QString ArtworkCache::fromArtworkRow(qint64 artworkId, int size) {
    QSqlQuery q(m_library.db());
    q.prepare("SELECT hash, source FROM artwork WHERE id = ?");
    q.addBindValue(artworkId);
    if (!q.exec() || !q.next())
        return {};
    const QString hash = q.value(0).toString(), source = q.value(1).toString();
    q.finish();
    const QString path = thumbnailPath(hash, size);
    if (QFileInfo::exists(path))
        return path;
    // The cache was cleared: make it again from where the picture came from.
    const Picture picture = readSource(source);
    if (picture.bytes.isEmpty())
        return {};
    QString made;
    return store(picture, size, &made) ? made : QString();
}

// Whether any of an album's tracks come from a Navidrome (Subsonic) server.
bool ArtworkCache::hasServerTracks(qint64 albumId) {
    QSqlQuery q(m_library.db());
    q.prepare("SELECT 1 FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.album_id = ? "
              "AND v.kind = 'subsonic' LIMIT 1");
    q.addBindValue(albumId);
    return q.exec() && q.next();
}

QString ArtworkCache::albumPicture(qint64 albumId, int size) {
    QSqlDatabase db = m_library.db();
    QSqlQuery q(db);
    q.prepare("SELECT artwork_id, art_checked FROM albums WHERE id = ?");
    q.addBindValue(albumId);
    if (!q.exec() || !q.next()) {
        m_error = "No such album";
        return {};
    }
    const qint64 artwork = q.value(0).toLongLong();
    const bool checked = !q.value(1).isNull();
    q.finish();
    if (checked) {
        if (!artwork && !hasServerTracks(albumId))
            return {}; // searched before and nothing found
        const QString made = fromArtworkRow(artwork, size);
        if (!made.isEmpty())
            return made;
        // Its source has gone; search again.
    }

    const Picture picture = findAlbumPicture(albumId);
    if (picture.source.isEmpty())
        return {}; // every track is offline: try again another time
    QString made;
    const qint64 stored = picture.bytes.isEmpty() ? 0 : store(picture, size, &made);
    QSqlQuery u(db);
    u.prepare("UPDATE albums SET artwork_id = ?, art_checked = ? WHERE id = ?");
    u.addBindValue(stored ? QVariant(stored) : nullInt());
    u.addBindValue(QDateTime::currentSecsSinceEpoch());
    u.addBindValue(albumId);
    u.exec();
    return made;
}

// Looks beside the album's tracks for a picture file, then in their tags.
// An empty source means none of the tracks is online, so nothing could be
// looked at; empty bytes with a source means nothing was found.
ArtworkCache::Picture ArtworkCache::findAlbumPicture(qint64 albumId) {
    QSqlQuery q(m_library.db());
    q.prepare("SELECT v.root_path, t.rel_path, t.has_embedded_art FROM tracks t JOIN volumes v ON v.id = t.volume_id "
              "WHERE t.album_id = ? AND v.online AND t.availability = 'online' AND v.kind != 'subsonic' "
              "ORDER BY t.disc_no, t.track_no, t.rel_path");
    q.addBindValue(albumId);
    q.exec();
    QStringList folders, embedded;
    while (q.next()) {
        const QString path = absoluteOf(q.value(0).toString(), q.value(1).toString());
        const QString folder = QFileInfo(path).path();
        if (!folders.contains(folder))
            folders << folder;
        if (q.value(2).toBool())
            embedded << path;
    }
    q.finish();
    Picture picture;
    if (folders.isEmpty()) {
        // An album from a Navidrome (Subsonic) server has no folder on disk:
        // the server has the cover, so ask it for one of the album's tracks.
        QSqlQuery r(m_library.db());
        r.prepare("SELECT t.id FROM tracks t JOIN volumes v ON v.id = t.volume_id "
                  "WHERE t.album_id = ? AND v.kind = 'subsonic' ORDER BY t.disc_no, t.track_no LIMIT 1");
        r.addBindValue(albumId);
        if (r.exec() && r.next()) {
            const qint64 track = r.value(0).toLongLong();
            r.finish();
            return readSource("subsonic:" + QString::number(track));
        }
        return picture;
    }
    picture.source = "none";

    // A disc folder ("CD1", "Disc 2") often has the cover one level up.
    static const QRegularExpression discFolder("^(cd|disc|disk)\\s*\\d+$", QRegularExpression::CaseInsensitiveOption);
    QStringList candidates;
    for (const QString &folder : std::as_const(folders)) {
        candidates << folder;
        if (discFolder.match(QFileInfo(folder).fileName()).hasMatch())
            candidates << QFileInfo(folder).path();
    }
    for (const QString &folder : std::as_const(candidates)) {
        const QString file = pictureIn(folder);
        if (file.isEmpty())
            continue;
        Picture found = readSource("file:" + file);
        if (!found.bytes.isEmpty())
            return found;
    }
    for (const QString &path : std::as_const(embedded)) {
        Picture found = readSource("embedded:" + path);
        if (!found.bytes.isEmpty())
            return found;
    }
    return picture;
}

ArtworkCache::Picture ArtworkCache::readSource(const QString &source) {
    Picture picture;
    picture.source = source;
    if (source.startsWith("file:")) {
        QFile file(source.mid(5));
        if (file.open(QIODevice::ReadOnly) && file.size() <= maxPictureBytes)
            picture.bytes = file.readAll();
    } else if (source.startsWith("embedded:")) {
        picture.bytes = TagReader::embeddedPicture(source.mid(9), &picture.mime);
    } else if (source.startsWith("subsonic:")) {
        // The server's cover, fetched once into its own cache, then read
        // like any picture file. An unreachable server gives no bytes, so
        // the album is looked at again another time.
        QString error;
        const QString file = RemoteSources::coverArt(m_library, source.mid(9).toLongLong(), 600, &error);
        QFile f(file);
        if (!file.isEmpty() && f.open(QIODevice::ReadOnly) && f.size() <= maxPictureBytes)
            picture.bytes = f.readAll();
        else
            picture.source.clear();
    }
    return picture;
}

// Saves the picture at every standard size not made yet, records it, and
// gives the path at the size asked for. The image is decoded once, scaled
// down already by the decoder where the format allows (JPEG does), and
// resized on the CPU.
qint64 ArtworkCache::store(const Picture &picture, int size, QString *path) {
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(picture.bytes, QCryptographicHash::Sha1).toHex().left(32));
    QList<int> missing;
    for (int s : sizes())
        if (!QFileInfo::exists(thumbnailPath(hash, s)))
            missing << s;

    QVariant width = nullInt(), height = nullInt();
    QString format;
    if (!missing.isEmpty()) {
        if (!QDir().mkpath(m_dir)) {
            m_error = QString("Can't create %1").arg(m_dir);
            return 0;
        }
        QBuffer buffer;
        buffer.setData(picture.bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize original = reader.size();
        format = QString::fromLatin1(reader.format());
        if (original.isValid()) {
            width = original.width();
            height = original.height();
            const int largest = missing.last();
            if (original.width() > largest * 2 || original.height() > largest * 2)
                reader.setScaledSize(original.scaled(largest * 2, largest * 2, Qt::KeepAspectRatio));
        }
        QImage image = reader.read();
        if (image.isNull()) {
            m_error = QString("Can't decode the picture from %1: %2").arg(picture.source, reader.errorString());
            return 0;
        }
        if (!original.isValid()) {
            width = image.width();
            height = image.height();
        }
        // JPEG has no transparency: flatten onto white rather than black.
        if (image.hasAlphaChannel()) {
            QImage flat(image.size(), QImage::Format_RGB32);
            flat.fill(Qt::white);
            QPainter painter(&flat);
            painter.drawImage(0, 0, image);
            painter.end();
            image = flat;
        }
        for (int s : std::as_const(missing)) {
            const QImage scaled = image.width() > s || image.height() > s
                                      ? image.scaled(s, s, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                      : image;
            QSaveFile out(thumbnailPath(hash, s));
            if (!out.open(QIODevice::WriteOnly)) {
                m_error = out.errorString();
                return 0;
            }
            QImageWriter writer(&out, "jpg");
            writer.setQuality(88);
            writer.setOptimizedWrite(true);
            if (!writer.write(scaled.convertToFormat(QImage::Format_RGB888)) || !out.commit()) {
                m_error = writer.errorString();
                return 0;
            }
        }
    }

    QString mime = picture.mime;
    if (mime.isEmpty() && !format.isEmpty())
        mime = "image/" + (format == "jpg" ? QString("jpeg") : format);
    QSqlQuery q(m_library.db());
    q.prepare("INSERT INTO artwork (hash, mime, width, height, source, cache_path, added) VALUES (?, ?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(hash) DO UPDATE SET source = excluded.source, cache_path = excluded.cache_path, "
              "mime = coalesce(excluded.mime, mime), width = coalesce(excluded.width, width), "
              "height = coalesce(excluded.height, height) RETURNING id");
    q.addBindValue(hash);
    q.addBindValue(mime.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(mime));
    q.addBindValue(width);
    q.addBindValue(height);
    q.addBindValue(picture.source);
    q.addBindValue(m_dir + '/' + hash);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!q.exec() || !q.next()) {
        m_error = q.lastError().text();
        return 0;
    }
    const qint64 id = q.value(0).toLongLong();
    q.finish(); // a RETURNING statement stays open until finished
    if (path)
        *path = thumbnailPath(hash, snapSize(size));
    return id;
}

qint64 ArtworkCache::setAlbumPicture(qint64 albumId, const QString &file, QString *thumbnail, int size) {
    m_error.clear();
    const Picture picture = readSource("file:" + file);
    if (picture.bytes.isEmpty()) {
        m_error = QString("Can't read the picture %1").arg(file);
        return 0;
    }
    QString made;
    const qint64 stored = store(picture, snapSize(size), &made);
    if (!stored)
        return 0;
    QSqlQuery u(m_library.db());
    u.prepare("UPDATE albums SET artwork_id = ?, art_checked = ? WHERE id = ?");
    u.addBindValue(stored);
    u.addBindValue(QDateTime::currentSecsSinceEpoch());
    u.addBindValue(albumId);
    if (!u.exec()) {
        m_error = u.lastError().text();
        return 0;
    }
    if (thumbnail)
        *thumbnail = made;
    return stored;
}

int ArtworkCache::prefetch(int size, Job *job, bool all) {
    QList<qint64> albums;
    {
        QSqlQuery q(m_library.db());
        q.setForwardOnly(true);
        q.exec(all ? "SELECT id FROM albums ORDER BY id" : "SELECT id FROM albums WHERE art_checked IS NULL ORDER BY id");
        while (q.next())
            albums << q.value(0).toLongLong();
    }
    int made = 0;
    for (int i = 0; i < albums.size(); ++i) {
        if (job && job->isCancelled())
            break;
        if (!albumPicture(albums[i], size).isEmpty())
            ++made;
        if (job && (i % 16 == 0 || i + 1 == albums.size()))
            job->setProgress("Making album pictures", i + 1, albums.size());
    }
    return made;
}

ArtworkPrefetch::ArtworkPrefetch(const QString &dbPath, const QString &cacheDir, QObject *parent)
    : QObject(parent), m_db(dbPath), m_dir(cacheDir) {}

ArtworkPrefetch::~ArtworkPrefetch() {
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    delete m_job;
}

bool ArtworkPrefetch::isRunning() const { return m_thread && m_thread->isRunning(); }

void ArtworkPrefetch::cancel() {
    if (m_job)
        m_job->cancel();
}

void ArtworkPrefetch::start(int size) {
    if (isRunning())
        return;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    delete m_job;
    m_job = new Job("artwork");
    auto made = std::make_shared<std::atomic_int>(0);
    const QString db = m_db, dir = m_dir;
    Job *job = m_job;
    m_thread = QThread::create([db, dir, size, job, made] {
        Library library(db);
        if (!library.open())
            return;
        ArtworkCache cache(library, dir);
        *made = cache.prefetch(size, job);
    });
    m_thread->setObjectName("omamp-artwork");
    connect(m_thread, &QThread::finished, this, [this, made] { emit finished(*made); });
    // Pictures are a nicety: they never compete with playback or the window.
    m_thread->start(QThread::LowestPriority);
}
