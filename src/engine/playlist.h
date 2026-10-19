#pragma once

#include <QAbstractListModel>
#include <QStringList>

// The tracks in play order. Titles start as a tidy version of the file name
// ("Artist - Title.flac") and are replaced by the real tags once a track plays.
class Playlist : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(qint64 totalKnownMs READ totalKnownMs NOTIFY countChanged)

public:
    enum Roles { PathRole = Qt::UserRole + 1, TitleRole, ArtistRole, DurationRole, NumberRole };

    struct Track {
        QString path;
        QString title;
        QString artist;
        qint64 durationMs = 0;
        // The library's id for the track, when known (0 otherwise). A
        // track on a Subsonic server has path subsonic://<source>/<song>,
        // which the Player's resolver turns into a stream URL.
        qint64 trackId = 0;
    };

    // Whether a path names something other than a local file: a library
    // reference (subsonic://…) or a URL.
    static bool isReference(const QString &path);

    explicit Playlist(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_tracks.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_tracks.size(); }
    qint64 totalKnownMs() const;
    const Track &at(int row) const { return m_tracks.at(row); }
    QStringList paths() const;

    // Files are added as they are; folders are searched for audio, sorted by
    // path so albums stay in track order. Returns how many tracks were added.
    int addPaths(const QStringList &paths);
    // Tracks whose tags are already known (from the library), inserted at
    // row without touching the disk. Returns how many were inserted.
    int insertTracks(int row, const QList<Track> &tracks);
    void replaceTracks(const QList<Track> &tracks);
    void remove(int row);
    void clear();
    void move(int from, int to);
    void updateTags(int row, const QString &title, const QString &artist, qint64 durationMs);

    static bool isAudio(const QString &path);
    static Track trackFromFile(const QString &path);

signals:
    void countChanged();

private:
    QList<Track> m_tracks;
};
