#include "artprovider.h"

#include "artwork.h"
#include "library.h"

#include <QImage>
#include <QThread>
#include <map>
#include <memory>

namespace {

// One library connection per pool thread and database, opened on first use
// and closed when the thread ends (a connection belongs to its thread).
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
        QString kind;
        qint64 number = 0;
        int size = 0;
        if (!ArtImageProvider::parseId(m_id, &kind, &number, &size)) {
            m_error = QString("Not a picture: %1 (use album/<id>/<size> or track/<id>/<size>)").arg(m_id);
        } else {
            if (size <= 0 && m_requested.isValid())
                size = qMax(m_requested.width(), m_requested.height());
            if (Library *library = threadLibrary(m_db)) {
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
        // Delivered on the thread that asked (the one the response lives in),
        // never before the caller has had a chance to connect.
        QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }

private:
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

QQuickImageResponse *ArtImageProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    auto *response = new ArtResponse(id, requestedSize, m_db, m_dir);
    m_pool.start([response] { response->run(); });
    return response;
}
