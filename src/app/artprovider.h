#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Album and track pictures for QML, from the artwork cache:
//
//   engine.addImageProvider("thrum-art", new ArtImageProvider);
//
//   Image { source: "image://thrum-art/album/" + albumId + "/256" }
//   Image { source: "image://thrum-art/track/" + trackId }   // size from sourceSize, else 256
//   Image { source: "image://thrum-art/entry/" + Qt.btoa(path) + "/96" }   // any queue entry
//
// "entry" takes a Now Playing entry's path, base64 (URL-safe or not): a
// file in or out of the library, a Subsonic track, a radio station (its
// tile) or a Spotify track (its cover), found as the deck finds the
// playing track's (NowPlaying::resolve).
//
// "?r=<pixels>" asks for a square thumbnail at the requested size with
// corners rounded by that radius, transparent outside, so a list's cells
// need no clipping effect (which the software renderer lacks).
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
    static constexpr const char *name = "thrum-art";

    // Empty paths mean the default library and cache.
    explicit ArtImageProvider(const QString &dbPath = {}, const QString &cacheDir = {});
    ~ArtImageProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Parses "album/12/256" into its parts; false when it isn't one.
    static bool parseId(const QString &id, QString *kind, qint64 *number, int *size);
    // Parses "entry/<base64 path>/96" into the path and size.
    static bool parseEntry(const QString &id, QString *path, int *size);
    // A square crop of image at side pixels with corners of radius pixels.
    static QImage rounded(const QImage &image, int side, int radius);
    // Requests made and requests dropped before they started (tests, profiling).
    static int requestCount();
    static int cancelledCount();

private:
    QString m_db, m_dir;
    QThreadPool m_pool;
};
