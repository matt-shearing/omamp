#include "albumgridmodel.h"

#include "tracklistmodel.h"

#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>

AlbumGridModel::AlbumGridModel(QObject *parent) : QAbstractListModel(parent) {}

void AlbumGridModel::setDatabase(const QSqlDatabase &db) {
    m_db = db;
    reload(true);
}

void AlbumGridModel::setTracks(TrackListModel *tracks) {
    m_tracks = tracks;
    // reloaded() also fires for a sort, which doesn't change the albums;
    // reload() skips a condition it has already read.
    connect(tracks, &TrackListModel::reloaded, this, [this] { reload(false); });
}

int AlbumGridModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_albums.size()); }

QHash<int, QByteArray> AlbumGridModel::roleNames() const {
    return {{AlbumIdRole, "albumId"}, {TitleRole, "title"}, {ArtistRole, "artist"}, {YearRole, "year"},
            {TrackCountRole, "trackCount"}, {ArtStateRole, "artState"}};
}

QVariant AlbumGridModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_albums.size())
        return {};
    const Album &a = m_albums[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return a.title;
    case AlbumIdRole: return a.id;
    case ArtistRole: return a.artist;
    case YearRole: return a.year;
    case TrackCountRole: return a.tracks;
    case ArtStateRole: return a.artState;
    }
    return {};
}

void AlbumGridModel::setActive(bool on) {
    if (on == m_active)
        return;
    m_active = on;
    emit activeChanged();
    if (on)
        reload(false);
}

void AlbumGridModel::setCurrentRow(int row) {
    row = m_albums.isEmpty() ? -1 : qBound(-1, row, int(m_albums.size()) - 1);
    if (row == m_current)
        return;
    m_current = row;
    emit currentRowChanged();
}

int AlbumGridModel::rowOfAlbum(qint64 id) const {
    for (int i = 0; i < m_albums.size(); ++i)
        if (m_albums[i].id == id)
            return i;
    return -1;
}

void AlbumGridModel::refresh() { reload(true); }

void AlbumGridModel::reload(bool force) {
    if (!m_active || !m_tracks || !m_db.isOpen())
        return;
    const QString where = m_tracks->fullWhere();
    const QVariantList params = m_tracks->fullParams();
    QString signature = where;
    for (const QVariant &p : params)
        signature += '\x1f' + p.toString();
    if (!force && signature == m_loadedFor)
        return;
    m_loadedFor = signature;

    QElapsedTimer timer;
    timer.start();
    const qint64 current = currentAlbum();
    QVector<Album> albums;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    // Library order: album artist, then title; the year breaks ties between
    // albums of the same name.
    q.prepare("SELECT al.id, al.title, coalesce(nullif(al.album_artist, ''), min(t.sort_artist)), al.year, count(*), "
              "CASE WHEN al.art_checked IS NULL THEN 0 WHEN al.artwork_id IS NULL THEN 2 ELSE 1 END "
              "FROM tracks t JOIN albums al ON al.id = t.album_id WHERE " +
              where +
              " GROUP BY t.album_id ORDER BY 3 COLLATE NOCASE, al.title COLLATE NOCASE, al.year");
    for (const QVariant &p : params)
        q.addBindValue(p);
    if (q.exec()) {
        albums.reserve(1024);
        while (q.next())
            albums.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toInt(),
                           q.value(4).toInt(), q.value(5).toInt()});
    } else {
        qWarning("Album grid: %s", qPrintable(q.lastError().text()));
    }
    // The same albums (a refresh after covers were made, or a rescan that
    // changed nothing here): update in place, so the view keeps its place.
    bool same = albums.size() == m_albums.size();
    for (int i = 0; same && i < albums.size(); ++i)
        same = albums[i].id == m_albums[i].id;
    if (same) {
        m_albums = std::move(albums);
        if (!m_albums.isEmpty())
            emit dataChanged(index(0), index(int(m_albums.size()) - 1));
    } else {
        beginResetModel();
        m_albums = std::move(albums);
        endResetModel();
    }
    m_lastQueryMs = timer.nsecsElapsed() / 1e6;
    emit countChanged();
    const int row = current ? rowOfAlbum(current) : -1;
    if (row != m_current) {
        m_current = row;
        emit currentRowChanged();
    } else if (current) {
        emit currentRowChanged();
    }
}
