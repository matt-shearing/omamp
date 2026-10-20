#include "artprovider.h"

#include "artwork.h"
#include "library.h"
#include "nowplaying.h"
#include "remote.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QUrlQuery>
#include <atomic>
#include <QSqlQuery>
#include <QThread>
#include <map>
#include <memory>

namespace {

std::atomic_int requests{0};
std::atomic_int cancelled{0};

// "r=4" from "album/12/96?v=3&r=4", or -1.
int radiusFrom(const QString &id) {
    const int q = int(id.indexOf('?'));
    if (q < 0)
        return -1;
    const QUrlQuery query(id.mid(q + 1));
    bool ok = false;
    const int r = query.queryItemValue("r").toInt(&ok);
    return ok ? qMax(0, r) : -1;
}

// One library connection per pool thread and database, opened on first use
// and closed when the thread ends (a connection belongs to its thread).
// A track mirrored from a Navidrome / Subsonic server (or an album of
// them) has no file to read a picture from: the server's cover is fetched
// once and cached. Returns 0 when the picture should come from the files.
qint64 remoteTrackFor(Library &library, const QString &kind, qint64 number) {
    QSqlQuery q(library.db());
    if (kind == "album")
        q.prepare("SELECT t.id FROM tracks t JOIN volumes v ON v.id = t.volume_id "
                  "WHERE t.album_id = ? AND v.kind = 'subsonic' ORDER BY t.disc_no, t.track_no LIMIT 1");
    else
        q.prepare("SELECT t.id FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ? AND v.kind = 'subsonic'");
    q.addBindValue(number);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

Library *threadLibrary(const QString &path) {
    thread_local std::map<QString, std::unique_ptr<Library>> libraries;
    auto &library = libraries[path];
    if (!library) {
        library = std::make_unique<Library>(path);
        if (!library->open()) {
            library.reset();
            return nullptr;
        }
    }
    return library.get();
}

class ArtResponse : public QQuickImageResponse {
public:
    ArtResponse(const QString &id, const QSize &requested, const QString &db, const QString &dir)
        : m_id(id), m_requested(requested), m_db(db), m_dir(dir) {}

    // Runs on a pool thread. Posting finished() is the last thing it does:
    // the engine may delete the response as soon as it arrives.
    void run() {
        // Scrolled past before a thread was free: nothing to make.
        if (m_cancelled.load()) {
            ++cancelled;
            m_error = "Cancelled";
            QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
            return;
        }
        QString kind;
        qint64 number = 0;
        int size = 0;
        QString entry;
        if (ArtImageProvider::parseEntry(m_id, &entry, &size)) {
            if (size <= 0 && m_requested.isValid())
                size = qMax(m_requested.width(), m_requested.height());
            const NowPlaying::Art art = NowPlaying::resolve(entry, 0, m_db, m_dir);
            const QString path = size > 256 ? art.large : size > 96 ? art.medium : art.small;
            if (path.isEmpty() || !m_image.load(path))
                m_error = art.error.isEmpty() ? QString("No picture") : art.error;
        } else if (!ArtImageProvider::parseId(m_id, &kind, &number, &size)) {
            m_error = QString("Not a picture: %1 (use album/<id>/<size> or track/<id>/<size>)").arg(m_id);
        } else {
            if (size <= 0 && m_requested.isValid())
                size = qMax(m_requested.width(), m_requested.height());
            Library *library = threadLibrary(m_db);
            const qint64 remote = library ? remoteTrackFor(*library, kind, number) : 0;
            if (remote > 0) {
                QString error;
                const QString path = RemoteSources::coverArt(*library, remote, size > 0 ? size : 256, &error);
                if (path.isEmpty() || !m_image.load(path))
                    m_error = error.isEmpty() ? QString("No picture") : error;
                else if (m_requested.isValid() && m_requested.width() > 0 && m_requested.height() > 0 &&
                         (m_image.width() > m_requested.width() || m_image.height() > m_requested.height()))
                    m_image = m_image.scaled(m_requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            } else if (library) {
                ArtworkCache cache(*library, m_dir);
                const QString path = cache.artworkFor(
                    kind == "album" ? ArtworkCache::Kind::Album : ArtworkCache::Kind::Track, number, size);
                if (path.isEmpty() || !m_image.load(path))
                    m_error = cache.lastError().isEmpty() ? QString("No picture") : cache.lastError();
                else if (m_requested.isValid() && m_requested.width() > 0 && m_requested.height() > 0 &&
                         (m_image.width() > m_requested.width() || m_image.height() > m_requested.height()))
                    m_image = m_image.scaled(m_requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            } else {
                m_error = "The library could not be opened";
            }
        }
        const int radius = radiusFrom(m_id);
        if (radius >= 0 && !m_image.isNull()) {
            const int side = m_requested.isValid() && m_requested.width() > 0
                                 ? qMax(m_requested.width(), m_requested.height())
                                 : qMin(m_image.width(), m_image.height());
            m_image = ArtImageProvider::rounded(m_image, side, radius);
        }
        // Delivered on the thread that asked (the one the response lives in),
        // never before the caller has had a chance to connect.
        QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }
    // The view no longer wants it (the row scrolled away): a request still
    // waiting for a thread is dropped. finished() is still sent.
    void cancel() override { m_cancelled = true; }

private:
    std::atomic_bool m_cancelled{false};
    QString m_id;
    QSize m_requested;
    QString m_db, m_dir, m_error;
    QImage m_image;
};

} // namespace

ArtImageProvider::ArtImageProvider(const QString &dbPath, const QString &cacheDir)
    : m_db(dbPath.isEmpty() ? Library::defaultPath() : dbPath),
      m_dir(cacheDir.isEmpty() ? ArtworkCache::defaultCacheDir() : cacheDir) {
    m_pool.setMaxThreadCount(2);
    m_pool.setThreadPriority(QThread::LowPriority);
    // Threads stay, so their library connections do.
    m_pool.setExpiryTimeout(-1);
}

ArtImageProvider::~ArtImageProvider() { m_pool.waitForDone(); }

bool ArtImageProvider::parseId(const QString &id, QString *kind, qint64 *number, int *size) {
    const QStringList parts = id.section('?', 0, 0).split('/', Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.size() > 3 || (parts[0] != "album" && parts[0] != "track"))
        return false;
    bool ok = false;
    *number = parts[1].toLongLong(&ok);
    if (!ok || *number <= 0)
        return false;
    *kind = parts[0];
    *size = 0;
    if (parts.size() == 3) {
        *size = parts[2].toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

bool ArtImageProvider::parseEntry(const QString &id, QString *path, int *size) {
    const QStringList parts = id.section('?', 0, 0).split('/', Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.size() > 3 || parts[0] != "entry")
        return false;
    // Qt.btoa gives standard base64, whose "/" would split the id: accept
    // the URL-safe alphabet and percent-encoded slashes too.
    QByteArray encoded = QUrl::fromPercentEncoding(parts[1].toLatin1()).toLatin1();
    encoded.replace('-', '+').replace('_', '/');
    *path = QString::fromUtf8(QByteArray::fromBase64(encoded));
    if (path->isEmpty())
        return false;
    *size = 0;
    if (parts.size() == 3) {
        bool ok = false;
        *size = parts[2].toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

QImage ArtImageProvider::rounded(const QImage &image, int side, int radius) {
    if (image.isNull() || side <= 0)
        return image;
    // The middle square of the picture, scaled once to the side asked for.
    const int crop = qMin(image.width(), image.height());
    const QImage square = image.copy((image.width() - crop) / 2, (image.height() - crop) / 2, crop, crop)
                              .scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage out(side, side, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    const qreal r = qMin<qreal>(radius, side / 2.0);
    path.addRoundedRect(QRectF(0, 0, side, side), r, r);
    p.setClipPath(path);
    p.drawImage(0, 0, square);
    p.end();
    return out;
}

int ArtImageProvider::requestCount() { return requests.load(); }
int ArtImageProvider::cancelledCount() { return cancelled.load(); }

QQuickImageResponse *ArtImageProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    ++requests;
    auto *response = new ArtResponse(id, requestedSize, m_db, m_dir);
    m_pool.start([response] { response->run(); });
    return response;
}
