#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QSqlDatabase>
#include <QVector>

class TrackListModel;

// The album-art grid: the albums of whatever the track list shows (the tree
// node, the search and the column browser), one tile each, in library order.
//
// It reads one GROUP BY query, and only while active, so a hidden grid
// costs nothing. 20,000 albums are a few megabytes; the covers come from the
// image provider as tiles scroll into view.
class AlbumGridModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)
    Q_PROPERTY(qint64 currentAlbum READ currentAlbum NOTIFY currentRowChanged)
    Q_PROPERTY(double lastQueryMs READ lastQueryMs NOTIFY countChanged)

public:
    enum Roles { AlbumIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, YearRole, TrackCountRole };

    struct Album {
        qint64 id = 0;
        QString title, artist;
        int year = 0, tracks = 0;
    };

    explicit AlbumGridModel(QObject *parent = nullptr);

    void setDatabase(const QSqlDatabase &db);
    void setTracks(TrackListModel *tracks);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool active() const { return m_active; }
    void setActive(bool on);
    int count() const { return int(m_albums.size()); }
    int currentRow() const { return m_current; }
    void setCurrentRow(int row);
    qint64 currentAlbum() const { return albumId(m_current); }
    double lastQueryMs() const { return m_lastQueryMs; }

    Q_INVOKABLE qint64 albumId(int row) const { return row >= 0 && row < m_albums.size() ? m_albums[row].id : 0; }
    Q_INVOKABLE QString albumTitle(int row) const { return row >= 0 && row < m_albums.size() ? m_albums[row].title : QString(); }
    Q_INVOKABLE int rowOfAlbum(qint64 id) const;
    // Reads the albums again, keeping the current album.
    Q_INVOKABLE void refresh();

signals:
    void activeChanged();
    void countChanged();
    void currentRowChanged();

private:
    void reload(bool force);

    QSqlDatabase m_db;
    QPointer<TrackListModel> m_tracks;
    QVector<Album> m_albums;
    QString m_loadedFor; // the condition the albums were read for
    bool m_active = false;
    int m_current = -1;
    double m_lastQueryMs = 0;
};
