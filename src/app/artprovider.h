#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Album and track pictures for QML, from the artwork cache:
//
//   engine.addImageProvider("omamp-art", new ArtImageProvider);
//
//   Image { source: "image://omamp-art/album/" + albumId + "/256" }
//   Image { source: "image://omamp-art/track/" + trackId }   // size from sourceSize, else 256
//
// The size snaps to the cache's 96, 256 or 600 pixels. Anything after a "?"
// is ignored, so the window can add "?v=2" to reload a picture that changed.
// A missing picture fails the request (Image.status is Image.Error), and
// the window shows its own placeholder.
//
// Pictures are made on two low-priority threads, each with its own library
// connection, so scrolling a wall of covers never blocks the window.
class ArtImageProvider : public QQuickAsyncImageProvider {
public:
    static constexpr const char *name = "omamp-art";

    // Empty paths mean the default library and cache.
    explicit ArtImageProvider(const QString &dbPath = {}, const QString &cacheDir = {});
    ~ArtImageProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Parses "album/12/256" into its parts; false when it isn't one.
    static bool parseId(const QString &id, QString *kind, qint64 *number, int *size);

private:
    QString m_db, m_dir;
    QThreadPool m_pool;
};
