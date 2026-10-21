#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <functional>

class Player;

// The cover of the track that is playing, for the deck, the mini player,
// the big view and MPRIS (mpris:artUrl). QML sees it as `nowPlayingArt`.
//
// It works without the library window: the deck never opens the library,
// so the picture is found on a worker thread with its own connection to
// the library file, when there is one.
//
//   - A track in the library gets the library's picture (its own embedded
//     one, else its album's: cover.jpg beside it, or a picture chosen from
//     the Cover Art Archive), from the artwork cache.
//   - A track on a Navidrome / Subsonic server gets the server's cover.
//   - A file that isn't in the library gets the picture in its tags, else a
//     cover/folder/front picture in its folder, read on the fly.
//
// Every picture lands in the artwork cache (~/.cache/thrum/art) as
// <hash>-96.jpg, -256.jpg and -600.jpg, named by the hash of the original
// picture as the library names them, plus <hash>-blur.jpg: a small blurred
// copy that the big view stretches behind everything. The blur is made on
// the CPU, once, so the background needs no GPU effect.
class NowPlaying : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasArt READ hasArt NOTIFY artChanged)
    // file:// URLs, or empty when there is no picture.
    Q_PROPERTY(QString artUrl READ artUrl NOTIFY artChanged)     // 600 px
    Q_PROPERTY(QString coverUrl READ coverUrl NOTIFY artChanged) // 256 px
    Q_PROPERTY(QString thumbUrl READ thumbUrl NOTIFY artChanged) // 96 px
    Q_PROPERTY(QString blurUrl READ blurUrl NOTIFY artChanged)
    // Where the picture came from: library, remote, embedded, folder, or "".
    Q_PROPERTY(QString artSource READ artSource NOTIFY artChanged)
    // The playing track's library id, found even while the library window
    // is closed; 0 for a file that isn't in the library.
    Q_PROPERTY(qint64 trackId READ trackId NOTIFY artChanged)
    // True while a picture is being looked for.
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)

public:
    struct Art {
        QString large, medium, small, blur; // local paths
        QString source;
        qint64 trackId = 0;
        QString error;
    };

    // Empty paths mean the default library file and artwork cache.
    explicit NowPlaying(Player *player, const QString &dbPath = {}, const QString &cacheDir = {},
                        QObject *parent = nullptr);
    ~NowPlaying() override;

    bool hasArt() const { return !m_art.large.isEmpty(); }
    QString artUrl() const;
    QString coverUrl() const;
    QString thumbUrl() const;
    QString blurUrl() const;
    QString artPath() const { return m_art.large; }
    QString artSource() const { return m_art.source; }
    qint64 trackId() const { return m_art.trackId; }
    bool loading() const { return m_loading; }

    // Looks again for the playing track's picture (after a cover changed).
    Q_INVOKABLE void refresh();
    // The same, a moment later, so a burst of library changes looks once.
    void refreshSoon() { m_soon.start(); }
    // Looks for the picture of any queue entry, as if it were playing
    // (tests, and previews).
    void lookUp(const QString &path, qint64 trackId) { start(path, trackId); }

    // Pictures for queue entries outside the library (spotify://…): a
    // function from the entry to a local picture file, or empty. It is
    // called on the worker thread, so it must be safe there.
    static void setReferenceArt(std::function<QString(const QString &path)> lookup);

    // Finds a picture for a track; runs on any thread. path is the queue
    // entry (a file, or subsonic://…), trackId the library id if known.
    static Art resolve(const QString &path, qint64 trackId, const QString &dbPath, const QString &cacheDir);
    // Writes the standard sizes and the blur of a picture into the cache;
    // false when the bytes aren't a picture.
    static bool storePicture(const QByteArray &bytes, const QString &cacheDir, Art *art);
    // The blurred background for a picture: small, soft and saturated.
    static QImage blurred(const QImage &image);

signals:
    void artChanged();
    void loadingChanged();

private:
    void onTrackChanged();
    void start(const QString &path, qint64 trackId);

    QPointer<Player> m_player;
    QString m_db, m_dir;
    QString m_path;
    qint64 m_requestedId = 0;
    Art m_art;
    bool m_loading = false;
    int m_generation = 0;
    QThreadPool m_pool;
    QTimer m_soon;
};
