#include "librarytree.h"

#include "query.h"

#include <QCollator>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>

namespace {

bool exec(QSqlQuery &q, const QVariantList &params = {}) {
    for (const QVariant &p : params)
        q.addBindValue(p);
    if (q.exec())
        return true;
    qWarning("Library tree: %s", qPrintable(q.lastError().text()));
    return false;
}

QString stars(int halfStars) {
    if (halfStars <= 0)
        return LibraryTree::tr("No stars");
    return QString(halfStars / 2, QChar(0x2605)) + (halfStars % 2 ? QStringLiteral("½") : QString());
}

QString placeholders(int n) {
    QStringList marks;
    for (int i = 0; i < n; ++i)
        marks << "?";
    return marks.join(", ");
}

} // namespace

QString LibraryTree::letterOf(const QString &title) {
    const QString t = title.trimmed();
    if (t.isEmpty() || !t[0].isLetter())
        return QStringLiteral("#");
    return t.left(1).toUpper();
}

int LibraryTree::reveal(const QStringList &keys) {
    int row = -1;
    for (int i = 0; i < keys.size(); ++i) {
        row = rowOfKey(keys[i]);
        if (row < 0)
            return -1;
        if (i + 1 < keys.size())
            expand(row);
    }
    return row;
}

LibraryTree::LibraryTree(QObject *parent) : QAbstractListModel(parent) {}

void LibraryTree::setDatabase(const QSqlDatabase &db) {
    m_db = db;
    beginResetModel();
    m_nodes = topLevel();
    endResetModel();
    emit countChanged();
}

int LibraryTree::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_nodes.size(); }

QHash<int, QByteArray> LibraryTree::roleNames() const {
    return {{LabelRole, "label"},           {DepthRole, "depth"}, {ExpandableRole, "expandable"},
            {ExpandedRole, "expanded"},     {TrackCountRole, "trackCount"}, {DimRole, "dim"},
            {KindRole, "kind"},             {KeyRole, "key"}};
}

QVariant LibraryTree::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_nodes.size())
        return {};
    const Node &n = m_nodes[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case LabelRole: return n.label;
    case DepthRole: return n.depth;
    case ExpandableRole: return n.expandable;
    case ExpandedRole: return n.expanded;
    case TrackCountRole: return n.count;
    case DimRole: return n.dim;
    case KindRole: return n.kind;
    case KeyRole: return n.key;
    }
    return {};
}

QVector<LibraryTree::Node> LibraryTree::topLevel() const {
    QVector<Node> out;
    auto group = [&](const QString &key, const QString &label) {
        Node n;
        n.key = key;
        n.kind = "group";
        n.label = label;
        n.expandable = true;
        out << n;
    };
    Node entire;
    entire.key = "entire";
    entire.kind = "entire";
    entire.label = tr("Entire Library");
    if (m_db.isOpen()) {
        QSqlQuery q(m_db);
        if (q.exec("SELECT count(*) FROM tracks") && q.next())
            entire.count = q.value(0).toLongLong();
    }
    out << entire;
    group("collections", tr("Collections"));
    group("artists", tr("Artist"));
    group("albumartists", tr("Album Artist"));
    group("albums", tr("Album"));
    group("genres", tr("Genre"));
    group("years", tr("Year"));
    group("ratings", tr("Rating"));
    group("composers", tr("Composer"));
    group("locations", tr("Location"));
    group("playlists", tr("Playlists"));
    return out;
}

QVector<LibraryTree::Node> LibraryTree::peopleWithRole(const Node &parent, const QString &role,
                                                       const QString &kind) const {
    QVector<Node> out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare("SELECT a.id, a.name, count(*) FROM track_artists ta JOIN artists a ON a.id = ta.artist_id "
              "WHERE ta.role = ? GROUP BY a.id ORDER BY a.name COLLATE NOCASE");
    if (!exec(q, {role}))
        return out;
    while (q.next()) {
        Node n;
        n.kind = kind;
        n.key = kind + ':' + q.value(0).toString();
        n.label = q.value(1).toString();
        n.depth = parent.depth + 1;
        n.count = q.value(2).toLongLong();
        n.value = q.value(0);
        n.where = QString("t.id IN (SELECT track_id FROM track_artists WHERE artist_id = ? AND role = '%1')").arg(role);
        n.params = {q.value(0)};
        n.expandable = kind == "artist";
        out << n;
    }
    return out;
}

QVector<LibraryTree::Node> LibraryTree::albumsWhere(const Node &parent, const QString &where,
                                                    const QVariantList &params) const {
    QVector<Node> out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    const bool byLetter = parent.kind == "letter";
    q.prepare(QString("SELECT al.id, al.title, al.year, al.album_artist, count(*) FROM tracks t "
                      "JOIN albums al ON al.id = t.album_id WHERE %1 GROUP BY al.id ORDER BY %2")
                  .arg(where, byLetter ? "al.title COLLATE NOCASE, al.album_artist COLLATE NOCASE"
                                       : "al.year, al.title COLLATE NOCASE"));
    if (!exec(q, params))
        return out;
    while (q.next()) {
        Node n;
        n.kind = "album";
        n.key = parent.key + "/album:" + q.value(0).toString();
        const QString title = q.value(1).toString().isEmpty() ? tr("Unknown album") : q.value(1).toString();
        const int year = q.value(2).toInt();
        // Under a letter, the artist tells albums of the same name apart;
        // under an artist, the year puts them in order.
        if (byLetter)
            n.label = q.value(3).toString().isEmpty() ? title : QString("%1 — %2").arg(title, q.value(3).toString());
        else
            n.label = year > 0 ? QString("%1 (%2)").arg(title).arg(year) : title;
        n.depth = parent.depth + 1;
        n.count = q.value(4).toLongLong();
        n.value = q.value(0);
        if (byLetter) {
            n.where = "t.album_id = ?";
            n.params = {q.value(0)};
        } else {
            n.where = "(" + parent.where + ") AND t.album_id = ?";
            n.params = parent.params;
            n.params << q.value(0);
        }
        n.dim = parent.dim;
        out << n;
    }
    return out;
}

QVector<LibraryTree::Node> LibraryTree::folderChildren(const Node &parent) const {
    QVector<Node> out;
    const QVariantList v = parent.value.toList();
    const qint64 volumeId = v.value(0).toLongLong();
    const QString prefix = v.value(1).toString();
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    if (prefix.isEmpty()) {
        q.prepare("SELECT rel_path FROM folders WHERE volume_id = ?");
        if (!exec(q, {volumeId}))
            return out;
    } else {
        // A range on the (volume_id, rel_path) index: everything under prefix/.
        q.prepare("SELECT rel_path FROM folders WHERE volume_id = ? AND rel_path >= ? AND rel_path < ?");
        if (!exec(q, {volumeId, prefix + '/', prefix + '0'}))
            return out;
    }
    QMap<QString, bool> names; // child name -> has folders under it
    const int skip = prefix.isEmpty() ? 0 : prefix.size() + 1;
    while (q.next()) {
        const QString rest = q.value(0).toString().mid(skip);
        if (rest.isEmpty())
            continue;
        const int slash = rest.indexOf('/');
        const QString name = slash < 0 ? rest : rest.left(slash);
        names[name] = names.value(name) || slash >= 0;
    }
    QStringList sorted = names.keys();
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(sorted.begin(), sorted.end(), collator);
    for (const QString &name : sorted) {
        const QString path = prefix.isEmpty() ? name : prefix + '/' + name;
        Node n;
        n.kind = "folder";
        n.key = QString("folder:%1:%2").arg(volumeId).arg(path);
        n.label = name;
        n.depth = parent.depth + 1;
        n.expandable = names.value(name);
        n.dim = parent.dim;
        n.value = QVariantList{volumeId, path};
        n.where = "t.volume_id = ? AND t.rel_path >= ? AND t.rel_path < ?";
        n.params = {volumeId, path + '/', path + '0'};
        out << n;
    }
    return out;
}

QVector<LibraryTree::Node> LibraryTree::children(const Node &parent) const {
    QVector<Node> out;
    if (!m_db.isOpen())
        return out;
    const int depth = parent.depth + 1;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    auto make = [&](const QString &kind, const QString &key, const QString &label, qint64 count,
                    const QString &where, const QVariantList &params) {
        Node n;
        n.kind = kind;
        n.key = key;
        n.label = label;
        n.depth = depth;
        n.count = count;
        n.where = where;
        n.params = params;
        n.dim = parent.dim;
        return n;
    };

    if (parent.key == "collections") {
        if (!q.exec("SELECT id, name, query FROM collections WHERE visible != 'never' ORDER BY position, name"))
            return out;
        while (q.next()) {
            const Query::Compiled c = Query::compile(q.value(2).toString());
            out << make("collection", "collection:" + q.value(0).toString(), q.value(1).toString(), -1,
                        c.ok() ? c.where : "0", c.params);
        }
    } else if (parent.key == "artists") {
        out = peopleWithRole(parent, "artist", "artist");
    } else if (parent.key == "composers") {
        out = peopleWithRole(parent, "composer", "composer");
    } else if (parent.kind == "artist" || parent.kind == "albumartist") {
        out = albumsWhere(parent, parent.where, parent.params);
    } else if (parent.key == "albumartists") {
        // Album artist as the library sorts by it: the album artist, else the
        // artist, else Various Artists for a compilation. One value per
        // track, read straight off the library-order index.
        if (!q.exec("SELECT t.sort_artist, count(*) FROM tracks t GROUP BY t.sort_artist COLLATE NOCASE "
                    "ORDER BY t.sort_artist COLLATE NOCASE"))
            return out;
        while (q.next()) {
            const QString name = q.value(0).toString();
            Node n = make("albumartist", "albumartist:" + name.toLower(),
                          name.isEmpty() ? tr("Unknown artist") : name, q.value(1).toLongLong(),
                          name.isEmpty() ? "coalesce(t.sort_artist, '') = ''" : "t.sort_artist = ? COLLATE NOCASE",
                          name.isEmpty() ? QVariantList{} : QVariantList{name});
            n.expandable = true;
            out << n;
        }
    } else if (parent.key == "albums") {
        if (!q.exec("SELECT substr(t.album, 1, 1), count(*) FROM tracks t WHERE t.album_id IS NOT NULL GROUP BY 1"))
            return out;
        QMap<QString, QPair<QStringList, qint64>> letters; // letter -> first characters, tracks
        while (q.next()) {
            const QString first = q.value(0).toString();
            auto &entry = letters[letterOf(first)];
            entry.first << first;
            entry.second += q.value(1).toLongLong();
        }
        QStringList keys = letters.keys();
        // # last, the rest in the language's order.
        QCollator collator;
        std::sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
            if ((a == "#") != (b == "#"))
                return b == "#";
            return collator.compare(a, b) < 0;
        });
        for (const QString &letter : keys) {
            const auto &entry = letters[letter];
            QVariantList params;
            for (const QString &c : entry.first)
                params << c;
            Node n = make("letter", "letter:" + letter, letter, entry.second,
                          QString("t.album_id IS NOT NULL AND substr(t.album, 1, 1) IN (%1)")
                              .arg(placeholders(params.size())),
                          params);
            n.expandable = true;
            out << n;
        }
    } else if (parent.kind == "letter") {
        // The letter's scope is over tracks; its albums are found by the
        // album's own title, which is what the label shows.
        out = albumsWhere(parent, parent.where, parent.params);
    } else if (parent.key == "genres") {
        if (!q.exec("SELECT g.id, g.name, count(*) FROM genres g JOIN track_genres tg ON tg.genre_id = g.id "
                    "GROUP BY g.id ORDER BY g.name COLLATE NOCASE"))
            return out;
        while (q.next())
            out << make("genre", "genre:" + q.value(0).toString(), q.value(1).toString(), q.value(2).toLongLong(),
                        "t.id IN (SELECT track_id FROM track_genres WHERE genre_id = ?)", {q.value(0)});
    } else if (parent.key == "years") {
        if (!q.exec("SELECT t.year, count(*) FROM tracks t GROUP BY t.year"))
            return out;
        QMap<int, qint64> decades;
        qint64 unknown = 0;
        while (q.next()) {
            const int year = q.value(0).toInt();
            if (year <= 0)
                unknown += q.value(1).toLongLong();
            else
                decades[year / 10 * 10] += q.value(1).toLongLong();
        }
        for (auto it = decades.cbegin(); it != decades.cend(); ++it) {
            Node n = make("decade", QString("decade:%1").arg(it.key()), QString("%1s").arg(it.key()), it.value(),
                          "t.year BETWEEN ? AND ?", {it.key(), it.key() + 9});
            n.value = it.key();
            n.expandable = true;
            out << n;
        }
        if (unknown)
            out << make("year", "year:unknown", tr("Unknown year"), unknown, "coalesce(t.year, 0) <= 0", {});
    } else if (parent.kind == "decade") {
        q.prepare("SELECT t.year, count(*) FROM tracks t WHERE t.year BETWEEN ? AND ? GROUP BY t.year");
        if (!exec(q, {parent.value, parent.value.toInt() + 9}))
            return out;
        while (q.next())
            out << make("year", "year:" + q.value(0).toString(), q.value(0).toString(), q.value(1).toLongLong(),
                        "t.year = ?", {q.value(0)});
    } else if (parent.key == "ratings") {
        if (!q.exec("SELECT t.rating, count(*) FROM tracks t GROUP BY t.rating"))
            return out;
        QMap<int, qint64> halves; // half stars -> tracks
        qint64 unrated = 0;
        while (q.next()) {
            if (q.value(0).isNull())
                unrated += q.value(1).toLongLong();
            else
                halves[(q.value(0).toInt() + 5) / 10] += q.value(1).toLongLong();
        }
        for (auto it = halves.cend(); it != halves.cbegin();) {
            --it;
            const int h = it.key();
            out << make("rating", QString("rating:%1").arg(h), stars(h), it.value(), "t.rating BETWEEN ? AND ?",
                        {qMax(0, h * 10 - 5), h * 10 + 4});
        }
        if (unrated)
            out << make("rating", "rating:none", tr("Unrated"), unrated, "t.rating IS NULL", {});
    } else if (parent.key == "locations") {
        if (!q.exec("SELECT v.id, v.label, v.root_path, v.online, "
                    "(SELECT count(*) FROM tracks WHERE volume_id = v.id) FROM volumes v ORDER BY v.root_path"))
            return out;
        while (q.next()) {
            const bool online = q.value(3).toBool();
            const QString root = q.value(2).toString();
            const QString label = q.value(1).toString();
            Node n = make("volume", "volume:" + q.value(0).toString(),
                          online ? root : tr("%1 (offline)").arg(label.isEmpty() ? root : label),
                          q.value(4).toLongLong(), "t.volume_id = ?", {q.value(0)});
            n.dim = !online;
            n.expandable = true;
            n.value = q.value(0);
            out << n;
        }
    } else if (parent.kind == "volume") {
        // A volume opens on its library folders; a folder that is the whole
        // volume opens straight onto its subfolders.
        q.prepare("SELECT rel_path FROM folders WHERE volume_id = ? AND is_root = 1 ORDER BY rel_path");
        if (!exec(q, {parent.value}))
            return out;
        QStringList roots;
        while (q.next())
            roots << q.value(0).toString();
        if (roots.contains(QString())) {
            Node whole = parent;
            whole.value = QVariantList{parent.value, QString()};
            out = folderChildren(whole);
        } else {
            for (const QString &root : roots) {
                Node n = make("folder", QString("folder:%1:%2").arg(parent.value.toString(), root), root, -1,
                              "t.volume_id = ? AND t.rel_path >= ? AND t.rel_path < ?",
                              {parent.value, root + '/', root + '0'});
                n.value = QVariantList{parent.value, root};
                n.expandable = true;
                out << n;
            }
        }
    } else if (parent.kind == "folder") {
        out = folderChildren(parent);
    } else if (parent.key == "playlists") {
        out << make("nowplaying", "nowplaying", tr("Now Playing"), -1, "0", {});
        if (!q.exec("SELECT p.id, p.name, p.kind, p.query FROM playlists p WHERE p.kind != 'folder' "
                    "ORDER BY p.name COLLATE NOCASE"))
            return out;
        while (q.next()) {
            if (q.value(2).toString() == "auto") {
                const Query::Compiled c = Query::compile(q.value(3).toString());
                out << make("playlist", "playlist:" + q.value(0).toString(), q.value(1).toString(), -1,
                            c.ok() ? c.where : "0", c.params);
            } else {
                out << make("playlist", "playlist:" + q.value(0).toString(), q.value(1).toString(), -1,
                            "t.id IN (SELECT track_id FROM playlist_items WHERE playlist_id = ?)", {q.value(0)});
            }
        }
    }
    return out;
}

void LibraryTree::expand(int row) {
    if (row < 0 || row >= m_nodes.size())
        return;
    Node &n = m_nodes[row];
    if (!n.expandable || n.expanded)
        return;
    const QVector<Node> kids = children(n);
    n.expanded = true;
    if (kids.isEmpty()) {
        // Nothing under it after all: drop the arrow.
        n.expandable = false;
        n.expanded = false;
        emit dataChanged(index(row), index(row), {ExpandableRole, ExpandedRole});
        return;
    }
    emit dataChanged(index(row), index(row), {ExpandedRole});
    beginInsertRows({}, row + 1, row + kids.size());
    for (int i = 0; i < kids.size(); ++i)
        m_nodes.insert(row + 1 + i, kids[i]);
    endInsertRows();
    emit countChanged();
}

void LibraryTree::collapse(int row) {
    if (row < 0 || row >= m_nodes.size() || !m_nodes[row].expanded)
        return;
    const int depth = m_nodes[row].depth;
    int end = row + 1;
    while (end < m_nodes.size() && m_nodes[end].depth > depth)
        ++end;
    m_nodes[row].expanded = false;
    emit dataChanged(index(row), index(row), {ExpandedRole});
    if (end > row + 1) {
        beginRemoveRows({}, row + 1, end - 1);
        m_nodes.remove(row + 1, end - row - 1);
        endRemoveRows();
        emit countChanged();
    }
}

void LibraryTree::toggle(int row) {
    if (isExpanded(row))
        collapse(row);
    else
        expand(row);
}

int LibraryTree::parentRow(int row) const {
    if (row <= 0 || row >= m_nodes.size())
        return -1;
    const int depth = m_nodes[row].depth;
    for (int i = row - 1; i >= 0; --i)
        if (m_nodes[i].depth < depth)
            return i;
    return -1;
}

int LibraryTree::rowOfKey(const QString &key) const {
    for (int i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].key == key)
            return i;
    return -1;
}

QString LibraryTree::kind(int row) const { return row >= 0 && row < m_nodes.size() ? m_nodes[row].kind : QString(); }
QString LibraryTree::key(int row) const { return row >= 0 && row < m_nodes.size() ? m_nodes[row].key : QString(); }
bool LibraryTree::isExpanded(int row) const { return row >= 0 && row < m_nodes.size() && m_nodes[row].expanded; }
bool LibraryTree::isExpandable(int row) const {
    return row >= 0 && row < m_nodes.size() && m_nodes[row].expandable;
}

void LibraryTree::reload() {
    QSet<QString> open;
    for (const Node &n : std::as_const(m_nodes))
        if (n.expanded)
            open.insert(n.key);
    beginResetModel();
    m_nodes = topLevel();
    // Rows are inserted after their parent as the loop goes, so nodes opened
    // inside other open nodes are reached and reopened too.
    for (int row = 0; row < m_nodes.size(); ++row) {
        if (!open.contains(m_nodes[row].key) || !m_nodes[row].expandable)
            continue;
        const QVector<Node> kids = children(m_nodes[row]);
        m_nodes[row].expanded = !kids.isEmpty();
        for (int i = 0; i < kids.size(); ++i)
            m_nodes.insert(row + 1 + i, kids[i]);
    }
    endResetModel();
    emit countChanged();
}
