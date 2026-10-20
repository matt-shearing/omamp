#include "playlistitemsmodel.h"

#include "library.h"

#include <algorithm>

PlaylistItemsModel::PlaylistItemsModel(QObject *parent) : QAbstractListModel(parent) {}

int PlaylistItemsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_items.size(); }

QHash<int, QByteArray> PlaylistItemsModel::roleNames() const {
    return {{ItemIdRole, "itemId"},     {TrackIdRole, "trackId"},           {NumberRole, "number"},
            {TitleRole, "title"},       {ArtistRole, "artist"},             {AlbumRole, "album"},
            {DurationRole, "duration"}, {AvailabilityRole, "availability"}, {DuplicateRole, "duplicate"},
            {SelectedRole, "rowSelected"}, {CurrentRole, "rowCurrent"},     {PathRole, "path"},
            {SpotifyRole, "fromSpotify"}};
}

QVariant PlaylistItemsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const Playlists::Item &it = m_items[index.row()];
    switch (role) {
    case ItemIdRole: return it.id;
    case TrackIdRole: return it.trackId;
    case NumberRole: return index.row() + 1;
    case Qt::DisplayRole:
    case TitleRole: {
        if (!it.title.isEmpty())
            return it.title;
        const int slash = it.path.lastIndexOf('/');
        return slash >= 0 ? it.path.mid(slash + 1) : it.path;
    }
    case ArtistRole: return it.artist;
    case AlbumRole: return it.album;
    case DurationRole: return it.durationMs;
    case AvailabilityRole: return it.availability;
    case DuplicateRole:
        return it.trackId > 0 ? m_trackUses.value(it.trackId) > 1 : it.isSpotify() && m_spotifyUses.value(it.path) > 1;
    case SpotifyRole: return it.isSpotify();
    case SelectedRole: return m_selected.contains(index.row());
    case CurrentRole: return index.row() == m_current;
    case PathRole: return it.path;
    }
    return {};
}

void PlaylistItemsModel::setPlaylist(qint64 id) {
    m_id = id;
    m_selected.clear();
    m_current = m_anchor = -1;
    load();
    emit playlistChanged();
    emit selectionChanged();
    emit currentRowChanged();
}

void PlaylistItemsModel::setLocked(bool locked) {
    if (locked == m_locked)
        return;
    m_locked = locked;
    emit playlistChanged();
}

void PlaylistItemsModel::reload() {
    const int current = m_current;
    load();
    m_current = m_items.isEmpty() || current < 0 ? -1 : qMin(current, int(m_items.size()) - 1);
    QSet<int> kept;
    for (int row : std::as_const(m_selected))
        if (row < m_items.size())
            kept.insert(row);
    m_selected = kept;
    emit playlistChanged();
    emit currentRowChanged();
    emit selectionChanged();
}

void PlaylistItemsModel::load() {
    beginResetModel();
    m_items.clear();
    m_trackUses.clear();
    m_spotifyUses.clear();
    m_totalMs = 0;
    m_unmatched = m_duplicates = m_spotify = 0;
    m_error.clear();
    m_playlist = Playlists::Playlist();
    if (m_library && m_id > 0 && Playlists::get(*m_library, m_id, &m_playlist) && m_playlist.kind != "folder")
        m_items = Playlists::items(*m_library, m_id, &m_error);
    for (const Playlists::Item &it : std::as_const(m_items)) {
        m_totalMs += qMax<qint64>(0, it.durationMs);
        if (it.trackId > 0) {
            ++m_trackUses[it.trackId];
        } else if (it.isSpotify()) {
            ++m_spotify;
            ++m_spotifyUses[it.path];
        } else {
            ++m_unmatched;
        }
    }
    for (auto it = m_trackUses.cbegin(); it != m_trackUses.cend(); ++it)
        if (it.value() > 1)
            m_duplicates += it.value() - 1;
    for (auto it = m_spotifyUses.cbegin(); it != m_spotifyUses.cend(); ++it)
        if (it.value() > 1)
            m_duplicates += it.value() - 1;
    endResetModel();
    emit countChanged();
}

void PlaylistItemsModel::selectRows(const QList<int> &rows, int current) {
    m_selected = QSet<int>(rows.begin(), rows.end());
    m_current = current;
    if (!m_items.isEmpty())
        emit dataChanged(index(0), index(m_items.size() - 1), {SelectedRole, CurrentRole});
    emit selectionChanged();
    emit currentRowChanged();
}

void PlaylistItemsModel::setCurrentRow(int row) {
    if (row == m_current || row < -1 || row >= m_items.size())
        return;
    const int old = m_current;
    m_current = row;
    if (old >= 0)
        emit dataChanged(index(old), index(old), {CurrentRole});
    if (row >= 0)
        emit dataChanged(index(row), index(row), {CurrentRole});
    emit currentRowChanged();
}

void PlaylistItemsModel::select(int row, int modifiers) {
    if (row < 0 || row >= m_items.size())
        return;
    QSet<int> next;
    if (modifiers & Qt::ShiftModifier) {
        const int from = m_anchor >= 0 ? m_anchor : row;
        if (modifiers & Qt::ControlModifier)
            next = m_selected;
        for (int r = qMin(from, row); r <= qMax(from, row); ++r)
            next.insert(r);
    } else if (modifiers & Qt::ControlModifier) {
        next = m_selected;
        if (next.contains(row))
            next.remove(row);
        else
            next.insert(row);
        m_anchor = row;
    } else {
        next.insert(row);
        m_anchor = row;
    }
    selectRows(QList<int>(next.begin(), next.end()), row);
}

void PlaylistItemsModel::moveCurrent(int delta, int modifiers) {
    if (m_items.isEmpty())
        return;
    const int row = qBound(0, (m_current < 0 ? -1 : m_current) + delta, int(m_items.size()) - 1);
    if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier))
        select(row, modifiers & Qt::ShiftModifier ? Qt::ShiftModifier : 0);
    else
        select(row, 0);
}

void PlaylistItemsModel::selectAll() {
    QList<int> rows;
    for (int i = 0; i < m_items.size(); ++i)
        rows << i;
    selectRows(rows, m_current < 0 && !rows.isEmpty() ? 0 : m_current);
}

void PlaylistItemsModel::clearSelection() { selectRows({}, m_current); }

QList<int> PlaylistItemsModel::selectedRows() const {
    QList<int> rows(m_selected.begin(), m_selected.end());
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty() && m_current >= 0 && m_current < m_items.size())
        rows << m_current;
    return rows;
}

QVariantList PlaylistItemsModel::selectedTrackIds() const {
    QVariantList ids;
    for (int row : selectedRows())
        if (m_items[row].trackId > 0)
            ids << m_items[row].trackId;
    return ids;
}

qint64 PlaylistItemsModel::trackId(int row) const {
    return row >= 0 && row < m_items.size() ? m_items[row].trackId : 0;
}

bool PlaylistItemsModel::moveSelection(int delta) {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty() || delta == 0)
        return false;
    const int to = qBound(0, rows.first() + delta, int(m_items.size()) - int(rows.size()));
    if (to == rows.first())
        return false;
    return moveSelectionTo(to > rows.first() ? to + int(rows.size()) : to);
}

// row is where the drag ended, in the list as it is now (before the move):
// the entries go just before that row, or at the end for row == count.
bool PlaylistItemsModel::moveSelectionTo(int row) {
    if (!editable() || !m_library) {
        if (!editable() && m_playlist.kind == QLatin1String("auto"))
            emit failed(tr("An auto-playlist's order comes from its query"));
        return false;
    }
    const QList<int> rows = selectedRows();
    if (rows.isEmpty())
        return false;
    row = qBound(0, row, int(m_items.size()));
    // Where the block starts once the moved rows are taken out.
    int before = 0;
    for (int r : rows)
        if (r < row)
            ++before;
    const int to = row - before;
    QList<qint64> itemIds;
    for (int r : rows)
        itemIds << m_items[r].id;
    QString error;
    if (!Playlists::moveItems(*m_library, m_id, itemIds, to, &error)) {
        emit failed(error);
        return false;
    }
    load();
    QList<int> moved;
    for (int i = 0; i < rows.size(); ++i)
        moved << to + i;
    m_anchor = to;
    selectRows(moved, to);
    emit edited();
    return true;
}

int PlaylistItemsModel::removeSelection() {
    if (!editable() || !m_library)
        return 0;
    const QList<int> rows = selectedRows();
    QList<qint64> itemIds;
    for (int r : rows)
        itemIds << m_items[r].id;
    if (itemIds.isEmpty())
        return 0;
    QString error;
    const int removed = Playlists::removeItems(*m_library, m_id, itemIds, &error);
    if (!error.isEmpty())
        emit failed(error);
    const int first = rows.first();
    load();
    m_anchor = -1;
    selectRows({}, m_items.isEmpty() ? -1 : qMin(first, int(m_items.size()) - 1));
    emit edited();
    return removed;
}
