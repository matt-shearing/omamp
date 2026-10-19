#include "nowplaying.h"

#include "artwork.h"
#include "library.h"
#include "player.h"
#include "playlist.h"
#include "remote.h"
#include "tagreader.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <map>
#include <memory>

namespace {

constexpr qint64 maxPictureBytes = 40 * 1024 * 1024;

// One library connection per worker thread and file, kept while the thread
// lives (a connection belongs to the thread that opened it). The library
// is never created from here: without a library file there is nothing to
// look up, and the deck alone must not make one.
Library *workerLibrary(const QString &path) {
    thread_local std::map<QString, std::unique_ptr<Library>> libraries;
    auto &library = libraries[path];
    if (!library) {
        if (!QFileInfo::exists(path))
            return nullptr;
        library = std::make_unique<Library>(path);
        if (!library->open()) {
            library.reset();
            return nullptr;
        }
    }
    return library.get();
}

// The library's id for a file, by the volume whose folder holds it.
qint64 trackForPath(Library &library, const QString &path) {
    QSqlQuery q(library.db());
    q.setForwardOnly(true);
    q.exec("SELECT id, root_path FROM volumes WHERE kind <> 'subsonic'");
    QList<QPair<qint64, QString>> volumes;
    while (q.next())
        volumes.append({q.value(0).toLongLong(), q.value(1).toString()});
    q.finish();
    for (const auto &v : volumes) {
        const QString root = v.second == "/" ? QString() : v.second;
        if (!path.startsWith(root + '/'))
            continue;
        QSqlQuery t(library.db());
        t.prepare("SELECT id FROM tracks WHERE volume_id = ? AND rel_path = ?");
        t.addBindValue(v.first);
        t.addBindValue(path.mid(root.size() + 1));
        if (t.exec() && t.next())
            return t.value(0).toLongLong();
    }
    return 0;
}

bool isRemote(Library &library, qint64 trackId) {
    QSqlQuery q(library.db());
    q.prepare("SELECT 1 FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ? AND v.kind = 'subsonic'");
    q.addBindValue(trackId);
    return q.exec() && q.next();
}

bool saveJpeg(const QImage &image, const QString &path) {
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    QImageWriter writer(&out, "jpg");
    writer.setQuality(88);
    return writer.write(image.convertToFormat(QImage::Format_RGB888)) && out.commit();
}

// The blur beside a set of cached sizes (<base>-600.jpg and the rest),
// made from the small one when it isn't there yet.
QString blurBeside(const QString &large) {
    const QString base = large.left(large.lastIndexOf('-'));
    const QString blur = base + "-blur.jpg";
    if (QFileInfo::exists(blur))
        return blur;
    QImage source(base + "-96.jpg");
    if (source.isNull())
        source.load(large);
    if (source.isNull() || !saveJpeg(NowPlaying::blurred(source), blur))
        return {};
    return blur;
}

void fillFromLarge(const QString &large, NowPlaying::Art *art) {
    const QString base = large.left(large.lastIndexOf('-'));
    art->large = large;
    art->medium = QFileInfo::exists(base + "-256.jpg") ? base + "-256.jpg" : large;
    art->small = QFileInfo::exists(base + "-96.jpg") ? base + "-96.jpg" : art->medium;
    art->blur = blurBeside(large);
}

QByteArray readPicture(const QString &file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly) || f.size() > maxPictureBytes)
        return {};
    return f.readAll();
}

} // namespace

NowPlaying::NowPlaying(Player *player, const QString &dbPath, const QString &cacheDir, QObject *parent)
    : QObject(parent), m_player(player), m_db(dbPath.isEmpty() ? Library::defaultPath() : dbPath),
      m_dir(cacheDir.isEmpty() ? ArtworkCache::defaultCacheDir() : cacheDir) {
    // One low-priority thread keeps its library connection between tracks.
    m_pool.setMaxThreadCount(1);
    m_pool.setThreadPriority(QThread::LowPriority);
    m_pool.setExpiryTimeout(-1);
    m_soon.setSingleShot(true);
    m_soon.setInterval(400);
    connect(&m_soon, &QTimer::timeout, this, &NowPlaying::refresh);
    if (m_player) {
        connect(m_player, &Player::trackChanged, this, &NowPlaying::onTrackChanged);
        onTrackChanged();
    }
}

NowPlaying::~NowPlaying() {
    ++m_generation;
    m_pool.waitForDone();
}

static QString urlOf(const QString &path) { return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString(); }
QString NowPlaying::artUrl() const { return urlOf(m_art.large); }
QString NowPlaying::coverUrl() const { return urlOf(m_art.medium); }
QString NowPlaying::thumbUrl() const { return urlOf(m_art.small); }
QString NowPlaying::blurUrl() const { return urlOf(m_art.blur); }

void NowPlaying::onTrackChanged() {
    const int row = m_player ? m_player->currentIndex() : -1;
    QString path;
    qint64 id = 0;
    if (row >= 0 && row < m_player->playlist()->count()) {
        path = m_player->playlist()->at(row).path;
        id = m_player->playlist()->at(row).trackId;
    }
    // trackChanged also comes when the length or the tags arrive: only a
    // different entry needs a new picture.
    if (path == m_path && id == m_requestedId)
        return;
    start(path, id);
}

void NowPlaying::refresh() { start(m_path, m_requestedId); }

void NowPlaying::start(const QString &path, qint64 trackId) {
    m_path = path;
    m_requestedId = trackId;
    const int generation = ++m_generation;
    if (path.isEmpty()) {
        if (hasArt() || m_art.trackId) {
            m_art = Art();
            emit artChanged();
        }
        if (m_loading) {
            m_loading = false;
            emit loadingChanged();
        }
        return;
    }
    if (!m_loading) {
        m_loading = true;
        emit loadingChanged();
    }
    const QString db = m_db, dir = m_dir;
    m_pool.start([this, generation, path, trackId, db, dir] {
        const Art art = resolve(path, trackId, db, dir);
        // Delivered on the window's thread; a newer track wins. The pool is
        // waited for in the destructor, so this object is still here.
        QMetaObject::invokeMethod(
            this,
            [this, generation, art] {
                if (generation != m_generation)
                    return;
                const bool changed = art.large != m_art.large || art.blur != m_art.blur ||
                                     art.trackId != m_art.trackId || art.source != m_art.source;
                m_art = art;
                m_loading = false;
                emit loadingChanged();
                if (changed)
                    emit artChanged();
            },
            Qt::QueuedConnection);
    });
}

NowPlaying::Art NowPlaying::resolve(const QString &path, qint64 trackId, const QString &dbPath,
                                    const QString &cacheDir) {
    Art art;
    if (path.isEmpty())
        return art;
    const bool reference = Playlist::isReference(path);
    Library *library = workerLibrary(dbPath);
    if (library && trackId <= 0 && !reference)
        trackId = trackForPath(*library, path);
    if (library && trackId > 0) {
        art.trackId = trackId;
        if (isRemote(*library, trackId)) {
            QString error;
            const QString file = RemoteSources::coverArt(*library, trackId, 600, &error);
            if (!file.isEmpty() && storePicture(readPicture(file), cacheDir, &art)) {
                art.source = "remote";
                return art;
            }
            art.error = error;
            return art;
        }
        ArtworkCache cache(*library, cacheDir);
        const QString large = cache.trackArtwork(trackId, 600);
        if (!large.isEmpty()) {
            fillFromLarge(large, &art);
            art.source = "library";
            return art;
        }
        art.error = cache.lastError();
        // The library looked and found nothing; the file may have gained a
        // picture since the last scan, so look at it as well.
    }
    if (reference)
        return art;
    // A file outside the library: its own picture first, as the library
    // prefers a track's embedded cover, then the folder's.
    QString mime;
    if (storePicture(TagReader::embeddedPicture(path, &mime), cacheDir, &art)) {
        art.source = "embedded";
        return art;
    }
    const QString beside = ArtworkCache::pictureBeside(QFileInfo(path).path());
    if (!beside.isEmpty() && storePicture(readPicture(beside), cacheDir, &art)) {
        art.source = "folder";
        return art;
    }
    return art;
}

bool NowPlaying::storePicture(const QByteArray &bytes, const QString &cacheDir, Art *art) {
    if (bytes.isEmpty())
        return false;
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(32));
    const QString base = cacheDir + '/' + hash;
    const QList<int> sizes = ArtworkCache::sizes();
    bool missing = false;
    for (int s : sizes)
        missing = missing || !QFileInfo::exists(QString("%1-%2.jpg").arg(base).arg(s));
    if (missing) {
        if (!QDir().mkpath(cacheDir))
            return false;
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize original = reader.size();
        if (original.isValid() && (original.width() > 1200 || original.height() > 1200))
            reader.setScaledSize(original.scaled(1200, 1200, Qt::KeepAspectRatio));
        QImage image = reader.read();
        if (image.isNull())
            return false;
        if (image.hasAlphaChannel()) {
            QImage flat(image.size(), QImage::Format_RGB32);
            flat.fill(Qt::white);
            QPainter painter(&flat);
            painter.drawImage(0, 0, image);
            painter.end();
            image = flat;
        }
        for (int s : sizes) {
            const QString file = QString("%1-%2.jpg").arg(base).arg(s);
            if (QFileInfo::exists(file))
                continue;
            const QImage scaled = image.width() > s || image.height() > s
                                      ? image.scaled(s, s, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                      : image;
            if (!saveJpeg(scaled, file))
                return false;
        }
    }
    fillFromLarge(QString("%1-%2.jpg").arg(base).arg(sizes.last()), art);
    return true;
}

// A box blur run three times (close to a Gaussian) over a 64-pixel copy,
// with the colours lifted a little so a dim cover still tints the stage.
// Stretched to the window with smooth scaling it reads as a soft glow.
QImage NowPlaying::blurred(const QImage &source) {
    const int n = 64;
    QImage image = source.scaled(n, n, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_RGB32);
    const int radius = 5;
    QList<QRgb> line(n);
    auto pass = [&](bool horizontal) {
        for (int a = 0; a < n; ++a) {
            for (int b = 0; b < n; ++b)
                line[b] = horizontal ? image.pixel(b, a) : image.pixel(a, b);
            for (int b = 0; b < n; ++b) {
                int r = 0, g = 0, bl = 0, count = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int i = qBound(0, b + k, n - 1);
                    r += qRed(line[i]);
                    g += qGreen(line[i]);
                    bl += qBlue(line[i]);
                    ++count;
                }
                const QRgb v = qRgb(r / count, g / count, bl / count);
                if (horizontal)
                    image.setPixel(b, a, v);
                else
                    image.setPixel(a, b, v);
            }
        }
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            QColor c = QColor::fromRgb(image.pixel(x, y)).toHsv();
            c.setHsv(c.hue(), qMin(255, int(c.saturation() * 1.25)), c.value());
            image.setPixel(x, y, c.rgb());
        }
    return image;
}
