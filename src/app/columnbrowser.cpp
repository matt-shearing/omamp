#include "columnbrowser.h"

#include "tracklistmodel.h"

#include <QElapsedTimer>
#include <QLocale>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

// ---- FacetModel --------------------------------------------------------------

int FacetModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_entries.size()); }

QHash<int, QByteArray> FacetModel::roleNames() const {
    return {{LabelRole, "label"},         {DetailRole, "detail"}, {CountRole, "valueCount"},
            {SelectedRole, "valueSelected"}, {AllRole, "isAll"}, {KeyRole, "key"}};
}

QVariant FacetModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case LabelRole: return e.label;
    case DetailRole: return e.detail;
    case CountRole: return e.count;
    case SelectedRole: return isSelected(index.row());
    case AllRole: return index.row() == 0;
    case KeyRole: return e.key;
    }
    return {};
}

int FacetModel::rowOfKey(const QString &key) const {
    for (int i = 1; i < m_entries.size(); ++i)
        if (m_entries[i].key == key)
            return i;
    return -1;
}

bool FacetModel::isSelected(int row) const {
    if (row < 0 || row >= m_entries.size())
        return false;
    return row == 0 ? m_selected.isEmpty() : m_selected.contains(m_entries[row].key);
}

int FacetModel::firstSelectedRow() const {
    if (m_selected.isEmpty())
        return 0;
    for (int i = 1; i < m_entries.size(); ++i)
        if (m_selected.contains(m_entries[i].key))
            return i;
    return 0;
}

void FacetModel::setTitle(const QString &title) {
    if (title == m_title)
        return;
    m_title = title;
    emit titleChanged();
}

bool FacetModel::setEntries(QVector<Entry> entries) {
    QSet<QString> still;
    for (int i = 1; i < entries.size(); ++i)
        if (m_selected.contains(entries[i].key))
            still.insert(entries[i].key);
    const bool lost = still.size() != m_selected.size();
    // The same values with new counts (a refresh): update in place, so the
    // view keeps its scroll position.
    bool sameKeys = entries.size() == m_entries.size();
    for (int i = 0; sameKeys && i < entries.size(); ++i)
        sameKeys = entries[i].key == m_entries[i].key;
    if (sameKeys) {
        m_entries = std::move(entries);
        if (!m_entries.isEmpty())
            emit dataChanged(index(0), index(int(m_entries.size()) - 1), {CountRole, LabelRole, DetailRole});
    } else {
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
        emit countChanged();
        m_anchor = 0;
    }
    if (lost) {
        m_selected = still;
        if (!m_entries.isEmpty())
            emit dataChanged(index(0), index(int(m_entries.size()) - 1), {SelectedRole});
        emit selectionChanged();
    }
    return lost;
}

bool FacetModel::select(int row, int modifiers) {
    if (row < 0 || row >= m_entries.size())
        return false;
    const QSet<QString> before = m_selected;
    const bool ctrl = modifiers & Qt::ControlModifier, shift = modifiers & Qt::ShiftModifier;
    if (row == 0) {
        m_selected.clear();
        m_anchor = 0;
    } else if (shift && m_anchor > 0) {
        if (!ctrl)
            m_selected.clear();
        for (int i = qMin(m_anchor, row); i <= qMax(m_anchor, row); ++i)
            m_selected.insert(m_entries[i].key);
    } else if (ctrl) {
        const QString &k = m_entries[row].key;
        if (!m_selected.remove(k))
            m_selected.insert(k);
        m_anchor = row;
    } else {
        m_selected = {m_entries[row].key};
        m_anchor = row;
    }
    if (m_selected == before)
        return false;
    emit dataChanged(index(0), index(int(m_entries.size()) - 1), {SelectedRole});
    emit selectionChanged();
    return true;
}

bool FacetModel::setSelectedKeys(const QSet<QString> &keys) {
    if (keys == m_selected)
        return false;
    m_selected = keys;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(int(m_entries.size()) - 1), {SelectedRole});
    emit selectionChanged();
    return true;
}

// ---- ColumnBrowser -----------------------------------------------------------

namespace {

QString marks(int n) {
    QStringList m;
    for (int i = 0; i < n; ++i)
        m << "?";
    return m.join(", ");
}

// A condition with no parameters made only of 1s selects every track, so a
// pane can count from its own table without joining the tracks.
bool selectsEverything(const QString &where, const QVariantList &params) {
    if (!params.isEmpty())
        return false;
    QString s = where;
    s.remove('(').remove(')').remove(' ').replace("AND", "");
    return !s.isEmpty() && s.count('1') == s.size();
}

} // namespace

ColumnBrowser::ColumnBrowser(QObject *parent) : QObject(parent) {
    for (FacetModel &p : m_panes)
        p.setParent(this); // so QML never takes them over
    m_panes[GenrePane].setTitle(tr("Genre"));
    m_panes[ArtistPane].setTitle(tr("Album Artist"));
    m_panes[AlbumPane].setTitle(tr("Album"));
}

void ColumnBrowser::setDatabase(const QSqlDatabase &db) {
    m_db = db;
    if (m_enabled)
        update(0, true);
}

void ColumnBrowser::setTracks(TrackListModel *tracks) {
    m_tracks = tracks;
    // A new node or search: recount before the list reloads, and keep what
    // is chosen where it is still there. The list reloads once, with the
    // browser's condition already in place.
    connect(tracks, &TrackListModel::baseChanged, this, [this] {
        if (m_enabled)
            update(0, false);
    });
}

void ColumnBrowser::setEnabled(bool on) {
    if (on == m_enabled)
        return;
    m_enabled = on;
    emit enabledChanged();
    if (on) {
        update(0, true);
    } else {
        // Hidden panes must not hide tracks.
        for (FacetModel &p : m_panes)
            p.setSelectedKeys({});
        if (m_tracks)
            m_tracks->setBaseOverride({});
        applyCondition(true);
    }
}

void ColumnBrowser::setArtistMode(const QString &mode) {
    const QString m = mode == "artist" ? mode : QStringLiteral("albumartist");
    if (m == m_artistMode)
        return;
    m_artistMode = m;
    m_panes[ArtistPane].setTitle(m == "artist" ? tr("Artist") : tr("Album Artist"));
    // The keys mean something else now.
    m_panes[ArtistPane].setSelectedKeys({});
    emit artistModeChanged();
    if (m_enabled)
        update(ArtistPane, true);
}

bool ColumnBrowser::filtering() const {
    for (const FacetModel &p : m_panes)
        if (p.selectedCount() > 0)
            return true;
    return false;
}

void ColumnBrowser::select(int pane, int row, int modifiers) {
    if (pane < 0 || pane >= PaneCount || !m_panes[pane].select(row, modifiers))
        return;
    update(pane + 1, true);
}

void ColumnBrowser::selectKeys(int pane, const QStringList &keys) {
    if (pane < 0 || pane >= PaneCount)
        return;
    if (!m_enabled)
        setEnabled(true);
    if (m_panes[pane].setSelectedKeys(QSet<QString>(keys.begin(), keys.end())))
        update(pane + 1, true);
}

void ColumnBrowser::clear() {
    bool changed = false;
    for (FacetModel &p : m_panes)
        changed |= p.setSelectedKeys({});
    if (changed && m_enabled)
        update(1, true);
}

void ColumnBrowser::recount() {
    if (m_enabled)
        update(0, false);
}

QString ColumnBrowser::condition(int pane, QVariantList *params) const {
    const QSet<QString> &keys = m_panes[pane].selected();
    if (keys.isEmpty())
        return {};
    QStringList sorted(keys.begin(), keys.end());
    sorted.sort();
    switch (pane) {
    case GenrePane:
        for (const QString &k : sorted)
            *params << k.toLongLong();
        return QString("t.id IN (SELECT track_id FROM track_genres WHERE genre_id IN (%1))").arg(marks(sorted.size()));
    case ArtistPane:
        if (m_artistMode == "artist") {
            for (const QString &k : sorted)
                *params << k.toLongLong();
            return QString("t.id IN (SELECT track_id FROM track_artists WHERE role = 'artist' AND artist_id IN (%1))")
                .arg(marks(sorted.size()));
        } else {
            QStringList parts;
            QStringList names;
            for (const QString &k : sorted) {
                if (k.isEmpty())
                    parts << "coalesce(t.sort_artist, '') = ''";
                else
                    names << k;
            }
            if (!names.isEmpty()) {
                parts.prepend(QString("t.sort_artist COLLATE NOCASE IN (%1)").arg(marks(names.size())));
                for (const QString &n : names)
                    *params << n;
            }
            return parts.size() == 1 ? parts.first() : "(" + parts.join(" OR ") + ")";
        }
    case AlbumPane: {
        QStringList parts;
        QStringList ids;
        for (const QString &k : sorted) {
            if (k == "0")
                parts << "t.album_id IS NULL";
            else
                ids << k;
        }
        if (!ids.isEmpty()) {
            parts.prepend(QString("t.album_id IN (%1)").arg(marks(ids.size())));
            for (const QString &id : ids)
                *params << id.toLongLong();
        }
        return parts.size() == 1 ? parts.first() : "(" + parts.join(" OR ") + ")";
    }
    }
    return {};
}

QVector<FacetModel::Entry> ColumnBrowser::countPane(int pane, const QString &where, const QVariantList &params) const {
    QVector<FacetModel::Entry> out;
    if (!m_db.isOpen())
        return out;
    const bool everything = selectsEverything(where, params);
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    auto run = [&](const QString &sql, bool bind) {
        q.prepare(sql);
        if (bind)
            for (const QVariant &p : params)
                q.addBindValue(p);
        if (!q.exec()) {
            qWarning("Column browser: %s", qPrintable(q.lastError().text()));
            return false;
        }
        return true;
    };

    FacetModel::Entry all;
    if (run("SELECT count(*) FROM tracks t WHERE " + where, true) && q.next())
        all.count = q.value(0).toLongLong();
    out << all;

    bool ok = false;
    switch (pane) {
    case GenrePane:
        ok = everything ? run("SELECT g.id, g.name, count(*) FROM track_genres tg JOIN genres g ON g.id = tg.genre_id "
                              "GROUP BY g.id ORDER BY g.name COLLATE NOCASE",
                              false)
                        : run("SELECT g.id, g.name, count(*) FROM tracks t JOIN track_genres tg ON tg.track_id = t.id "
                              "JOIN genres g ON g.id = tg.genre_id WHERE " +
                                  where + " GROUP BY g.id ORDER BY g.name COLLATE NOCASE",
                              true);
        while (ok && q.next())
            out << FacetModel::Entry{q.value(0).toString(), q.value(1).toString(), {}, q.value(2).toLongLong()};
        break;
    case ArtistPane:
        if (m_artistMode == "artist") {
            ok = everything ? run("SELECT a.id, a.name, count(*) FROM track_artists ta JOIN artists a ON a.id = "
                                  "ta.artist_id WHERE ta.role = 'artist' GROUP BY a.id ORDER BY a.name COLLATE NOCASE",
                                  false)
                            : run("SELECT a.id, a.name, count(*) FROM tracks t JOIN track_artists ta ON ta.track_id = "
                                  "t.id AND ta.role = 'artist' JOIN artists a ON a.id = ta.artist_id WHERE " +
                                      where + " GROUP BY a.id ORDER BY a.name COLLATE NOCASE",
                                  true);
            while (ok && q.next())
                out << FacetModel::Entry{q.value(0).toString(), q.value(1).toString(), {}, q.value(2).toLongLong()};
        } else {
            // One value per track, read off the library-order index.
            ok = run("SELECT coalesce(t.sort_artist, ''), count(*) FROM tracks t WHERE " + where +
                         " GROUP BY t.sort_artist COLLATE NOCASE ORDER BY t.sort_artist COLLATE NOCASE",
                     true);
            FacetModel::Entry unknown;
            while (ok && q.next()) {
                const QString name = q.value(0).toString();
                if (name.isEmpty()) {
                    unknown.count += q.value(1).toLongLong();
                    continue;
                }
                out << FacetModel::Entry{name.toLower(), name, {}, q.value(1).toLongLong()};
            }
            if (unknown.count > 0) {
                unknown.label = tr("Unknown artist");
                out << unknown; // key "" is the unknown artist; All is row 0
            }
        }
        break;
    case AlbumPane: {
        ok = run("SELECT coalesce(t.album_id, 0), al.title, al.album_artist, count(*) FROM tracks t "
                 "LEFT JOIN albums al ON al.id = t.album_id WHERE " +
                     where + " GROUP BY t.album_id ORDER BY al.title COLLATE NOCASE, al.album_artist COLLATE NOCASE",
                 true);
        FacetModel::Entry unknown;
        while (ok && q.next()) {
            if (q.value(0).toLongLong() == 0) {
                unknown = {QStringLiteral("0"), tr("Unknown album"), {}, q.value(3).toLongLong()};
                continue;
            }
            const QString title = q.value(1).toString();
            out << FacetModel::Entry{q.value(0).toString(), title.isEmpty() ? tr("Unknown album") : title,
                                     q.value(2).toString(), q.value(3).toLongLong()};
        }
        if (unknown.count > 0)
            out << unknown;
        break;
    }
    }
    out[0].label = tr("All (%1)").arg(QLocale().toString(all.count));
    out[0].detail = QString::number(qMax(0, int(out.size()) - 1));
    return out;
}

void ColumnBrowser::update(int from, bool reloadList) {
    if (!m_tracks)
        return;
    QElapsedTimer timer;
    timer.start();
    // Each pane counts within the node, the search, and the panes to its left.
    QString where = m_tracks->baseWhere();
    QVariantList params = m_tracks->baseParams();
    if (selectsEverything(where, params)) {
        m_tracks->setBaseOverride({});
    } else {
        // The node and search once, into a temporary table (on this
        // connection only, never in the library file): every pane and the
        // track list then read it by id. A search that can't use the index
        // (two letters) scans the library once instead of five times.
        if (from == 0 || !m_baseReady) {
            QSqlQuery q(m_db);
            q.exec("CREATE TEMP TABLE IF NOT EXISTS thrum_browse_base (id INTEGER PRIMARY KEY)");
            q.exec("DELETE FROM temp.thrum_browse_base");
            q.prepare("INSERT INTO temp.thrum_browse_base SELECT t.id FROM tracks t WHERE " + where);
            for (const QVariant &p : params)
                q.addBindValue(p);
            m_baseReady = q.exec();
            if (!m_baseReady)
                qWarning("Column browser: %s", qPrintable(q.lastError().text()));
        }
        if (m_baseReady) {
            where = QStringLiteral("t.id IN (SELECT id FROM temp.thrum_browse_base)");
            params.clear();
            m_tracks->setBaseOverride(where);
        }
    }
    for (int p = 0; p < PaneCount; ++p) {
        if (p >= from)
            m_panes[p].setEntries(countPane(p, where, params));
        QVariantList more;
        const QString c = condition(p, &more);
        if (!c.isEmpty()) {
            where = QString("(%1) AND %2").arg(where, c);
            params += more;
        }
    }
    m_lastQueryMs = timer.nsecsElapsed() / 1e6;
    emit counted();
    applyCondition(reloadList);
}

void ColumnBrowser::applyCondition(bool reloadList) {
    QStringList parts;
    QVariantList params;
    if (m_enabled) {
        for (int p = 0; p < PaneCount; ++p) {
            const QString c = condition(p, &params);
            if (!c.isEmpty())
                parts << c;
        }
    }
    if (m_tracks)
        m_tracks->setBrowse(parts.isEmpty() ? QStringLiteral("1") : parts.join(" AND "), params, reloadList);
    const bool now = filtering();
    if (now != m_wasFiltering) {
        m_wasFiltering = now;
        emit filteringChanged();
    }
}
