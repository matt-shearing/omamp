#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

class Job;
class Library;
class QThread;

// Album and track pictures, as small JPEG files on local disk.
//
// A picture comes from an image file beside the music (cover, folder or
// front, any case, as .jpg, .jpeg, .png, .webp, .bmp or .gif, in that order
// of names) or else from the tags (TagLib's PICTURE, front cover first). It
// is decoded once, on the CPU, and saved at each standard size into
// <cache>/<hash>-<size>.jpg, where the hash is of the original bytes, so an
// album whose every track embeds the same cover shares one set of files.
// The artwork table records each picture; albums.artwork_id and
// tracks.artwork_id point at it, and albums.art_checked records that an
// album was searched (an album with no picture isn't searched again until
// the scanner sees its tracks change).
//
// Lazy: nothing is made until it is asked for. ArtworkPrefetch makes the
// rest in the background at low priority.
//
// An ArtworkCache uses its Library on the calling thread, like the Library.
class ArtworkCache {
public:
    enum class Kind { Album, Track };

    // cacheDir empty means ~/.cache/omamp/art (following XDG_CACHE_HOME).
    explicit ArtworkCache(Library &library, const QString &cacheDir = {});

    static QString defaultCacheDir();
    // The sizes made, in pixels (the longer side): 96, 256 and 600.
    static const QList<int> &sizes();
    // The standard size to use for a request: the smallest that is at least
    // as big, or the largest; 0 or less means 256.
    static int snapSize(int requested);

    // A local JPEG of the picture at a standard size, made now if needed, or
    // empty when there is no picture (or its volume is offline). A track
    // without a picture of its own gets its album's.
    QString artworkFor(Kind kind, qint64 id, int size);
    QString albumArtwork(qint64 albumId, int size) { return artworkFor(Kind::Album, albumId, size); }
    QString trackArtwork(qint64 trackId, int size) { return artworkFor(Kind::Track, trackId, size); }

    // Makes the given size for every album not searched yet (or every album,
    // with all). Stops when the job is cancelled. Returns how many albums
    // now have a picture made.
    int prefetch(int size = 256, Job *job = nullptr, bool all = false);

    // Makes a picture file the album's picture (fetched from the Cover Art
    // Archive, for example): its sizes are made and the album marked as
    // searched. Gives the artwork row id, or 0 with lastError() set.
    qint64 setAlbumPicture(qint64 albumId, const QString &file, QString *thumbnail = nullptr, int size = 256);

    QString cacheDir() const { return m_dir; }
    QString lastError() const { return m_error; }

private:
    struct Picture {
        QByteArray bytes;
        QString source; // "file:<path>" or "embedded:<path>"
        QString mime;
    };
    QString albumPicture(qint64 albumId, int size);
    QString fromArtworkRow(qint64 artworkId, int size);
    Picture findAlbumPicture(qint64 albumId);
    static Picture readSource(const QString &source);
    qint64 store(const Picture &picture, int size, QString *path);
    QString thumbnailPath(const QString &hash, int size) const;

    Library &m_library;
    QString m_dir, m_error;
};

// Runs ArtworkCache::prefetch on its own low-priority thread with its own
// Library connection.
class ArtworkPrefetch : public QObject {
    Q_OBJECT
public:
    ArtworkPrefetch(const QString &dbPath = {}, const QString &cacheDir = {}, QObject *parent = nullptr);
    ~ArtworkPrefetch() override;

    void start(int size = 256);
    void cancel();
    bool isRunning() const;

signals:
    void finished(int made);

private:
    QString m_db, m_dir;
    QThread *m_thread = nullptr;
    Job *m_job = nullptr;
};
