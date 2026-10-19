#include "tracklistmodel.h"

#include "dates.h"
#include "query.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <algorithm>

TrackListModel::TrackListModel(QObject *parent) : QAbstractTableModel(parent) {}

void TrackListModel::setDatabase(const QSqlDatabase &db) {
    m_db = db;
    reload(false);
}

int TrackListModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_ids.size(); }

int TrackListModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : ColumnCount; }

QHash<int, QByteArray> TrackListModel::roleNames() const {
    QHash<int, QByteArray> names = QAbstractTableModel::roleNames();
    names[RatingRole] = "rating";
    names[AvailableRole] = "available";
    names[RemoteRole] = "remote";
    names[OfflineNoteRole] = "offlineNote";
    names[SelectedRole] = "rowSelected";
    names[CurrentRole] = "rowCurrent";
    names[PlayingRole] = "playing";
    names[TrackIdRole] = "trackId";
    names[NumericRole] = "numeric";
    return names;
}

QString TrackListModel::columnName(int column) const {
    switch (column) {
    case Number: return tr("#");
    case Title: return tr("Title");
    case Artist: return tr("Artist");
    case Album: return tr("Album");
    case Year: return tr("Year");
    case Genre: return tr("Genre");
    case Length: return tr("Length");
    case Rating: return tr("Rating");
    case Plays: return tr("Plays");
    case Added: return tr("Added");
    case Format: return tr("Format");
    }
    return {};
}

QVariant TrackListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        QString name = columnName(section);
        if (section == m_sortColumn)
            name += m_sortDescending ? QStringLiteral(" ▼") : QStringLiteral(" ▲");
        return name;
    }
    if (orientation == Qt::Vertical && role == Qt::DisplayRole)
        return section + 1;
    return {};
}

QString TrackListModel::clock(qint64 ms) {
    if (ms <= 0)
        return {};
    const qint64 total = ms / 1000;
    const qint64 h = total / 3600, m = total % 3600 / 60, s = total % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QVariant TrackListModel::display(const Row &r, int column) const {
    switch (column) {
    case Number:
        if (r.trackNo <= 0)
            return QString();
        return r.discNo > 1 ? QString("%1.%2").arg(r.discNo).arg(r.trackNo, 2, 10, QChar('0'))
                            : QString::number(r.trackNo);
    case Title: return r.title.isEmpty() ? r.fileName : r.title;
    case Artist: return r.artist;
    case Album: return r.album;
    case Year: return r.year > 0 ? QString::number(r.year) : QString();
    case Genre: return r.genre;
    case Length: return clock(r.durationMs);
    case Rating: return QString();
    case Plays: return r.plays > 0 ? QLocale().toString(r.plays) : QString();
    case Added:
        return r.added > 0 ? Dates::text(QDateTime::fromSecsSinceEpoch(r.added).date()) : QString();
    case Format: {
        const QString format = r.format.toUpper();
        return r.bitrate > 0 ? QString("%1 %2 kbps").arg(format).arg(r.bitrate) : format;
    }
    }
    return {};
}

QVariant TrackListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_ids.size())
        return {};
    const int row = index.row();
    // Roles that need nothing from the database.
    switch (role) {
    case SelectedRole: return m_selected.contains(m_ids[row]);
    case CurrentRole: return row == m_currentRow;
    case TrackIdRole: return m_ids[row];
    case NumericRole: {
        const int c = index.column();
        return c == Number || c == Year || c == Length || c == Plays;
    }
    default: break;
    }
    const Row *r = rowAt(row);
    if (!r)
        return {};
    switch (role) {
    case Qt::DisplayRole: return display(*r, index.column());
    case RatingRole: return r->rating;
    case AvailableRole: return r->availability == QLatin1String("online") && !r->path.isEmpty();
    case PlayingRole: return !m_playingPath.isEmpty() && r->path == m_playingPath;
    case RemoteRole: return r->volumeKind == QLatin1String("subsonic");
    case OfflineNoteRole:
        if (r->availability == QLatin1String("missing"))
            return tr("The file is missing; a rescan found it gone");
        if (r->availability != QLatin1String("online") || r->path.isEmpty())
            return r->volumeKind == QLatin1String("subsonic") ? tr("On %1, which isn't answering").arg(r->volumeName)
                                                              : tr("On %1, which isn't connected").arg(r->volumeName);
        return QString();
    }
    return {};
}

const TrackListModel::Row *TrackListModel::rowAt(int row) const {
    const int pageNo = row / pageSize;
    auto it = m_pages.find(pageNo);
    if (it == m_pages.end()) {
        // Too many pages kept: drop the least recently used quarter.
        if (m_pages.size() >= maxPages) {
            QList<QPair<quint64, int>> byUse;
            for (auto p = m_pages.cbegin(); p != m_pages.cend(); ++p)
                byUse.append({p.value()->used, p.key()});
            std::sort(byUse.begin(), byUse.end());
            for (int i = 0; i < maxPages / 4; ++i)
                m_pages.remove(byUse[i].second);
        }
        const int first = pageNo * pageSize;
        const int last = qMin(int(m_ids.size()), first + pageSize);
        auto page = QSharedPointer<Page>::create();
        page->rows = fetch(m_ids.mid(first, last - first));
        it = m_pages.insert(pageNo, page);
    }
    Page &page = *it.value();
    page.used = ++m_useCounter;
    const int offset = row - pageNo * pageSize;
    return offset < page.rows.size() ? &page.rows[offset] : nullptr;
}

// Reads the tracks with these ids, returned in the same order. The ids go
// into the statement as numbers: they come from the database, never from
// the user, and one statement beats binding hundreds of values.
QVector<TrackListModel::Row> TrackListModel::fetch(const QVector<qint64> &ids) const {
    QVector<Row> out(ids.size());
    if (ids.isEmpty() || !m_db.isOpen())
        return out;
    QHash<qint64, int> position;
    position.reserve(ids.size());
    QStringList list;
    list.reserve(ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        position.insert(ids[i], i);
        list << QString::number(ids[i]);
    }
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    if (!q.exec("SELECT t.id, v.root_path, v.online, t.rel_path, t.title, t.artist, t.album, t.genre, t.format, "
                "t.availability, t.file_name, t.year, t.track_no, t.disc_no, t.bitrate, t.play_count, t.rating, "
                "t.duration_ms, t.added, v.kind, coalesce(nullif(v.label, ''), v.root_path) "
                "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id IN (" +
                list.join(',') + ')')) {
        qWarning("Track list: %s", qPrintable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        const int at = position.value(id, -1);
        if (at < 0)
            continue;
        Row &r = out[at];
        r.id = id;
        if (q.value(2).toBool()) {
            const QString root = q.value(1).toString();
            r.path = (root == "/" ? QString() : root) + '/' + q.value(3).toString();
        }
        r.title = q.value(4).toString();
        r.artist = q.value(5).toString();
        r.album = q.value(6).toString();
        r.genre = q.value(7).toString();
        r.format = q.value(8).toString();
        r.availability = q.value(9).toString();
        r.fileName = q.value(10).toString();
        r.year = q.value(11).toInt();
        r.trackNo = q.value(12).toInt();
        r.discNo = q.value(13).toInt();
        r.bitrate = q.value(14).toInt();
        r.plays = q.value(15).toInt();
        r.rating = q.value(16).isNull() ? -1 : q.value(16).toInt();
        r.durationMs = q.value(17).toLongLong();
        r.added = q.value(18).toLongLong();
        r.volumeKind = q.value(19).toString();
        r.volumeName = q.value(20).toString();
    }
    return out;
}

QVector<TrackListModel::Row> TrackListModel::rows(const QList<int> &rowList) const {
    QVector<Row> out;
    out.reserve(rowList.size());
    QVector<qint64> batch;
    for (int i = 0; i < rowList.size(); ++i) {
        const int row = rowList[i];
        if (row >= 0 && row < m_ids.size())
            batch << m_ids[row];
        if (batch.size() == 500 || (i == rowList.size() - 1 && !batch.isEmpty())) {
            out += fetch(batch);
            batch.clear();
        }
    }
    return out;
}

// Every order ends with the library order and the id, so equal values keep
// albums together and the list never shuffles between refreshes.
//
// The text keys match the tracks_sort_* indexes (schema 5) exactly, tie-break
// included, so SQLite walks an index instead of sorting: 10 ms rather than
// 200 ms for 200,000 tracks. Descending keeps the tie-break ascending (tracks
// of an album stay in order), which costs a partial sort of each run of
// equal keys, about 50 ms.
QString TrackListModel::orderBy() const {
    static const QString library =
        "t.sort_artist COLLATE NOCASE, t.album COLLATE NOCASE, t.disc_no, t.track_no, t.id";
    static const QString libraryWithoutAlbum = "t.sort_artist COLLATE NOCASE, t.disc_no, t.track_no, t.id";
    const QString dir = m_sortDescending ? " DESC" : "";
    QString key;
    switch (m_sortColumn) {
    case Number: key = QString("t.disc_no%1, t.track_no%1").arg(dir); break;
    case Title: key = "coalesce(nullif(t.title, ''), t.file_name) COLLATE NOCASE" + dir; break;
    case Artist: key = "t.artist COLLATE NOCASE" + dir; break;
    case Album: return "t.album COLLATE NOCASE" + dir + ", " + libraryWithoutAlbum;
    case Year: key = "t.year" + dir; break;
    case Genre: key = "t.genre COLLATE NOCASE" + dir; break;
    case Length: key = "t.duration_ms" + dir; break;
    case Rating: key = "t.rating" + dir; break;
    case Plays: key = "t.play_count" + dir; break;
    case Added: key = "t.added" + dir; break;
    case Format: key = QString("t.format%1, t.bitrate%1").arg(dir); break;
    default: return library;
    }
    return key + ", " + library;
}

void TrackListModel::reload(bool keepSelection) {
    QElapsedTimer timer;
    timer.start();
    const qint64 currentId = m_currentRow >= 0 && m_currentRow < m_ids.size() ? m_ids[m_currentRow] : 0;
    const qint64 anchorId = m_anchorRow >= 0 && m_anchorRow < m_ids.size() ? m_ids[m_anchorRow] : 0;

    QVector<qint64> ids;
    if (m_db.isOpen()) {
        QSqlQuery q(m_db);
        q.setForwardOnly(true);
        q.prepare(QString("SELECT t.id FROM tracks t WHERE %1 ORDER BY %2").arg(fullWhere(), orderBy()));
        for (const QVariant &p : fullParams())
            q.addBindValue(p);
        if (q.exec()) {
            ids.reserve(1024);
            while (q.next())
                ids.append(q.value(0).toLongLong());
        } else {
            qWarning("Track list: %s", qPrintable(q.lastError().text()));
        }
    }

    beginResetModel();
    m_ids = std::move(ids);
    m_pages.clear();
    const int oldSelected = m_selected.size();
    if (keepSelection) {
        // Only tracks still in the list stay selected.
        if (!m_selected.isEmpty()) {
            QSet<qint64> still;
            for (qint64 id : std::as_const(m_ids))
                if (m_selected.contains(id))
                    still.insert(id);
            m_selected = still;
        }
        m_currentRow = currentId ? rowOfTrack(currentId) : -1;
        m_anchorRow = anchorId ? rowOfTrack(anchorId) : -1;
    } else {
        m_selected.clear();
        m_currentRow = -1;
        m_anchorRow = -1;
    }
    endResetModel();
    m_lastQueryMs = timer.nsecsElapsed() / 1e6;
    emit countChanged();
    emit currentRowChanged();
    if (oldSelected || !m_selected.isEmpty())
        emit selectionChanged();
    emit reloaded();
}

void TrackListModel::refresh() {
    emit aboutToRefresh();
    reload(true);
}

void TrackListModel::setScope(const QString &where, const QVariantList &params) {
    m_scopeWhere = where.isEmpty() ? QStringLiteral("1") : where;
    m_scopeParams = params;
    emit baseChanged();
    reload(false);
}

void TrackListModel::setBrowse(const QString &where, const QVariantList &params, bool reloadNow) {
    const QString w = where.isEmpty() ? QStringLiteral("1") : where;
    if (w == m_browseWhere && params == m_browseParams)
        return;
    m_browseWhere = w;
    m_browseParams = params;
    if (reloadNow)
        reload(false);
}

void TrackListModel::setFilter(const QString &text) {
    if (text == m_filter)
        return;
    m_filter = text;
    emit filterChanged();
    QString error;
    if (text.trimmed().isEmpty()) {
        m_filterWhere = "1";
        m_filterParams.clear();
    } else {
        const Query::Compiled c = Query::compile(text);
        if (!c.ok()) {
            // Half-typed queries ("artist:") are normal while typing: keep
            // showing the last good result and say what is wrong.
            error = c.error;
        } else {
            m_filterWhere = c.where;
            m_filterParams = c.params;
        }
    }
    if (error != m_filterError) {
        m_filterError = error;
        emit filterErrorChanged();
    }
    if (error.isEmpty()) {
        emit baseChanged();
        reload(false);
    }
}

void TrackListModel::sortBy(int column) {
    if (column < 0 || column >= ColumnCount)
        return;
    if (column != m_sortColumn)
        setSort(column, false);
    else if (!m_sortDescending)
        setSort(column, true);
    else
        setSort(-1, false);
}

void TrackListModel::setSort(int column, bool descending) {
    if (column == m_sortColumn && descending == m_sortDescending)
        return;
    m_sortColumn = column;
    m_sortDescending = descending;
    emit sortChanged();
    emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    reload(true);
}

void TrackListModel::setPlayingPath(const QString &path) {
    if (path == m_playingPath)
        return;
    m_playingPath = path;
    emit playingPathChanged();
    visibleDataChanged(PlayingRole);
}

void TrackListModel::setVisibleRows(int first, int last) {
    m_visibleFirst = qMax(0, first);
    m_visibleLast = qMax(m_visibleFirst, last);
}

void TrackListModel::visibleDataChanged(int role) {
    if (m_ids.isEmpty())
        return;
    // A margin for rows half in view and rows the view loads ahead.
    const int first = qBound(0, m_visibleFirst - 8, int(m_ids.size()) - 1);
    const int last = qBound(first, m_visibleLast + 8, int(m_ids.size()) - 1);
    emit dataChanged(index(first, 0), index(last, ColumnCount - 1), {role});
}

void TrackListModel::setCurrentRow(int row) {
    row = m_ids.isEmpty() ? -1 : qBound(-1, row, int(m_ids.size()) - 1);
    if (row == m_currentRow)
        return;
    const int old = m_currentRow;
    m_currentRow = row;
    if (old >= 0 && old < m_ids.size())
        emit dataChanged(index(old, 0), index(old, ColumnCount - 1), {CurrentRole});
    if (row >= 0)
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {CurrentRole});
    emit currentRowChanged();
}

void TrackListModel::selectionDataChanged() {
    visibleDataChanged(SelectedRole);
    emit selectionChanged();
}

void TrackListModel::select(int row, int modifiers) {
    if (row < 0 || row >= m_ids.size())
        return;
    const bool shift = modifiers & Qt::ShiftModifier;
    const bool ctrl = modifiers & Qt::ControlModifier;
    if (shift && m_anchorRow >= 0 && m_anchorRow < m_ids.size()) {
        if (!ctrl)
            m_selected.clear();
        const int a = qMin(m_anchorRow, row), b = qMax(m_anchorRow, row);
        for (int i = a; i <= b; ++i)
            m_selected.insert(m_ids[i]);
    } else if (ctrl) {
        const qint64 id = m_ids[row];
        if (!m_selected.remove(id))
            m_selected.insert(id);
        m_anchorRow = row;
    } else {
        m_selected.clear();
        m_selected.insert(m_ids[row]);
        m_anchorRow = row;
    }
    setCurrentRow(row);
    selectionDataChanged();
}

void TrackListModel::moveCurrent(int delta, int modifiers) {
    if (m_ids.isEmpty())
        return;
    const int row = qBound(0, (m_currentRow < 0 ? (delta > 0 ? -1 : int(m_ids.size())) : m_currentRow) + delta,
                           int(m_ids.size()) - 1);
    // Ctrl moves the cursor without changing the selection, so Ctrl+Space
    // (or a Ctrl+click) can pick rows that are far apart.
    if ((modifiers & Qt::ControlModifier) && !(modifiers & Qt::ShiftModifier))
        setCurrentRow(row);
    else
        select(row, modifiers & Qt::ShiftModifier);
}

void TrackListModel::selectAll() {
    m_selected = QSet<qint64>(m_ids.cbegin(), m_ids.cend());
    selectionDataChanged();
}

void TrackListModel::clearSelection() {
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    selectionDataChanged();
}

bool TrackListModel::isSelected(int row) const {
    return row >= 0 && row < m_ids.size() && m_selected.contains(m_ids[row]);
}

QList<int> TrackListModel::selectedRows() const {
    QList<int> out;
    if (!m_selected.isEmpty()) {
        for (int i = 0; i < m_ids.size(); ++i)
            if (m_selected.contains(m_ids[i]))
                out << i;
    } else if (m_currentRow >= 0) {
        out << m_currentRow;
    }
    return out;
}

qint64 TrackListModel::trackId(int row) const { return row >= 0 && row < m_ids.size() ? m_ids[row] : 0; }

QString TrackListModel::path(int row) const {
    const Row *r = row >= 0 && row < m_ids.size() ? rowAt(row) : nullptr;
    return r ? r->path : QString();
}

int TrackListModel::rowOfTrack(qint64 id) const { return m_ids.indexOf(id); }
