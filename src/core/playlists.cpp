#include "playlists.h"

#include "library.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

namespace {

// Begins a transaction unless the caller already has one open, so these
// functions work alone and inside a larger batch (an M3U or MediaMonkey
// import) alike.
struct Transaction {
    QSqlDatabase db;
    bool own = false, done = false;
    explicit Transaction(QSqlDatabase d) : db(d), own(d.transaction()) {}
    bool commit() {
        done = true;
        return !own || db.commit();
    }
    ~Transaction() {
        if (own && !done)
            db.rollback();
    }
};

qint64 now() { return QDateTime::currentSecsSinceEpoch(); }

bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}

QVariant nullable(qint64 v) { return v ? QVariant(v) : QVariant(QMetaType(QMetaType::LongLong)); }
QVariant nullableText(const QString &s) { return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s); }

QString joinPath(const QString &root, const QString &rel) {
    if (root.isEmpty() || rel.isEmpty())
        return {};
    return root == "/" ? '/' + rel : root + '/' + rel;
}

const char *playlistColumns =
    "p.id, coalesce(p.parent_id, 0), p.name, p.kind, p.query, p.seed, p.note, p.source, p.position, p.created, "
    "p.modified, (SELECT count(*) FROM playlist_items i WHERE i.playlist_id = p.id)";

Playlists::Playlist readPlaylist(const QSqlQuery &q) {
    Playlists::Playlist p;
    p.id = q.value(0).toLongLong();
    p.parentId = q.value(1).toLongLong();
    p.name = q.value(2).toString();
    p.kind = q.value(3).toString();
    p.query = q.value(4).toString();
    p.seed = q.value(5).toLongLong();
    p.note = q.value(6).toString();
    p.source = q.value(7).toString();
    p.position = q.value(8).toInt();
    p.created = q.value(9).toLongLong();
    p.modified = q.value(10).toLongLong();
    p.items = p.kind == "auto" ? -1 : q.value(11).toInt();
    return p;
}

void touch(Library &library, qint64 id) {
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlists SET modified = ? WHERE id = ?");
    q.addBindValue(now());
    q.addBindValue(id);
    q.exec();
}

int nextSiblingPosition(Library &library, qint64 parentId) {
    QSqlQuery q(library.db());
    q.prepare("SELECT coalesce(max(position) + 1, 0) FROM playlists WHERE coalesce(parent_id, 0) = ?");
    q.addBindValue(parentId);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

qint64 insertPlaylist(Library &library, const QString &name, const QString &kind, qint64 parentId,
                      const QString &query, QString *error) {
    if (name.trimmed().isEmpty()) {
        fail(error, "A playlist needs a name");
        return 0;
    }
    if (parentId) {
        Playlists::Playlist parent;
        if (!Playlists::get(library, parentId, &parent)) {
            fail(error, QString("No playlist %1 to put it in").arg(parentId));
            return 0;
        }
    }
    QSqlQuery q(library.db());
    q.prepare("INSERT INTO playlists (parent_id, name, kind, query, seed, created, modified, position) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(nullable(parentId));
    q.addBindValue(name.trimmed());
    q.addBindValue(kind);
    q.addBindValue(nullableText(query));
    q.addBindValue(kind == "auto" ? QVariant(qint64(QRandomGenerator::global()->bounded(1u << 31)))
                                  : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(now());
    q.addBindValue(now());
    q.addBindValue(nextSiblingPosition(library, parentId));
    if (!q.exec()) {
        fail(error, q.lastError().text());
        return 0;
    }
    return q.lastInsertId().toLongLong();
}

bool requireStatic(Library &library, qint64 id, QString *error) {
    Playlists::Playlist p;
    if (!Playlists::get(library, id, &p))
        return fail(error, QString("No playlist %1").arg(id));
    if (p.kind == "auto")
        return fail(error, QString("\"%1\" is an auto-playlist; change its query instead of its tracks").arg(p.name));
    if (p.kind == "folder")
        return fail(error, QString("\"%1\" is a folder; it holds playlists, not tracks").arg(p.name));
    return true;
}

QList<qint64> orderedItemIds(Library &library, qint64 id) {
    QList<qint64> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT id FROM playlist_items WHERE playlist_id = ? ORDER BY position, id");
    q.addBindValue(id);
    if (q.exec())
        while (q.next())
            out << q.value(0).toLongLong();
    return out;
}

bool writePositions(Library &library, const QList<qint64> &itemIds, QString *error) {
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlist_items SET position = ? WHERE id = ?");
    for (int i = 0; i < itemIds.size(); ++i) {
        q.addBindValue(i);
        q.addBindValue(itemIds[i]);
        if (!q.exec())
            return fail(error, q.lastError().text());
    }
    return true;
}

// Track rows for ids, in the order given (ids may repeat).
QList<Playlists::Item> trackItems(Library &library, const QList<qint64> &ids) {
    QHash<qint64, Playlists::Item> rows;
    QList<qint64> unique;
    QSet<qint64> seen;
    for (qint64 id : ids)
        if (!seen.contains(id)) {
            seen.insert(id);
            unique << id;
        }
    for (int start = 0; start < unique.size(); start += 500) {
        const QList<qint64> chunk = unique.mid(start, 500);
        QStringList marks;
        for (int i = 0; i < chunk.size(); ++i)
            marks << "?";
        QSqlQuery q(library.db());
        q.prepare("SELECT t.id, v.root_path, t.rel_path, t.title, t.artist, t.album, t.duration_ms, t.availability "
                  "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id IN (" +
                  marks.join(',') + ")");
        for (qint64 id : chunk)
            q.addBindValue(id);
        if (!q.exec())
            continue;
        while (q.next()) {
            Playlists::Item it;
            it.trackId = q.value(0).toLongLong();
            it.path = joinPath(q.value(1).toString(), q.value(2).toString());
            it.title = q.value(3).toString();
            it.artist = q.value(4).toString();
            it.album = q.value(5).toString();
            it.durationMs = q.value(6).toLongLong();
            it.availability = q.value(7).toString();
            rows.insert(it.trackId, it);
        }
    }
    QList<Playlists::Item> out;
    for (qint64 id : ids) {
        auto found = rows.constFind(id);
        if (found == rows.constEnd())
            continue;
        Playlists::Item it = *found;
        it.position = out.size();
        out << it;
    }
    return out;
}

} // namespace

namespace Playlists {

QJsonObject Playlist::toJson() const {
    QJsonObject o{{"id", id},
                  {"parent_id", parentId ? QJsonValue(parentId) : QJsonValue()},
                  {"name", name},
                  {"kind", kind},
                  {"position", position},
                  {"created", created},
                  {"modified", modified}};
    if (kind == "auto") {
        o["query"] = query;
        o["seed"] = seed;
    } else if (kind == "static") {
        o["items"] = items;
    }
    if (!note.isEmpty())
        o["note"] = note;
    if (!source.isEmpty())
        o["source"] = source;
    return o;
}

QJsonObject Item::toJson() const {
    QJsonObject o{{"id", id ? QJsonValue(id) : QJsonValue()},
            {"track_id", trackId ? QJsonValue(trackId) : QJsonValue()},
            {"position", position},
            {"path", path.isEmpty() ? QJsonValue() : QJsonValue(path)},
            {"title", title},
            {"artist", artist},
            {"album", album},
            {"duration_ms", durationMs},
            {"availability", availability},
            {"spotify", isSpotify()}};
    if (isSpotify())
        o["url"] = spotifyWebUrl(path);
    if (!art.isEmpty())
        o["art"] = art;
    return o;
}

bool isSpotifyPath(const QString &path) { return path.startsWith(QLatin1String("spotify://track/")); }

QString spotifyTrackPath(const QString &text) {
    static const QRegularExpression forms(
        "^(?:spotify://track/|spotify:track:|https?://open\\.spotify\\.com/(?:intl-[a-z-]+/)?track/)"
        "([A-Za-z0-9]{10,40})(?:[?#].*)?$",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = forms.match(text.trimmed());
    return m.hasMatch() ? "spotify://track/" + m.captured(1) : QString();
}

QString spotifyWebUrl(const QString &path) {
    return isSpotifyPath(path) ? "https://open.spotify.com/track/" + path.mid(16) : QString();
}

static void walk(Library &library, qint64 parent, QList<Playlist> &out, int depth) {
    // Depth guards against a loop written into the table by hand.
    if (depth > 64)
        return;
    for (const Playlist &p : children(library, parent)) {
        out << p;
        walk(library, p.id, out, depth + 1);
    }
}

QList<Playlist> list(Library &library) {
    // Parents before children, depth first, siblings in order.
    QList<Playlist> out;
    walk(library, 0, out, 0);
    return out;
}

QList<Playlist> children(Library &library, qint64 parentId) {
    QList<Playlist> out;
    QSqlQuery q(library.db());
    q.prepare(QString("SELECT %1 FROM playlists p WHERE coalesce(p.parent_id, 0) = ? "
                      "ORDER BY p.position, p.name COLLATE NOCASE, p.id")
                  .arg(playlistColumns));
    q.addBindValue(parentId);
    if (q.exec())
        while (q.next())
            out << readPlaylist(q);
    return out;
}

bool get(Library &library, qint64 id, Playlist *out) {
    QSqlQuery q(library.db());
    q.prepare(QString("SELECT %1 FROM playlists p WHERE p.id = ?").arg(playlistColumns));
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return false;
    if (out)
        *out = readPlaylist(q);
    return true;
}

qint64 resolve(Library &library, const QString &ref, QString *error) {
    bool isNumber = false;
    const qint64 asId = ref.toLongLong(&isNumber);
    if (isNumber && get(library, asId, nullptr))
        return asId;
    if (ref.contains('/')) {
        qint64 parent = 0;
        for (const QString &part : ref.split('/', Qt::SkipEmptyParts)) {
            qint64 found = 0;
            for (const Playlist &p : children(library, parent))
                if (p.name.compare(part, Qt::CaseInsensitive) == 0) {
                    found = p.id;
                    break;
                }
            if (!found) {
                fail(error, QString("No playlist called %1").arg(ref));
                return 0;
            }
            parent = found;
        }
        return parent;
    }
    QSqlQuery q(library.db());
    q.prepare("SELECT id FROM playlists WHERE name = ? COLLATE NOCASE");
    q.addBindValue(ref);
    QList<qint64> hits;
    if (q.exec())
        while (q.next())
            hits << q.value(0).toLongLong();
    if (hits.size() == 1)
        return hits.first();
    fail(error, hits.isEmpty() ? QString("No playlist called %1").arg(ref)
                               : QString("%1 playlists are called %2; use its id or its folder path (Folder/Name)")
                                     .arg(hits.size())
                                     .arg(ref));
    return 0;
}

qint64 create(Library &library, const QString &name, qint64 parentId, QString *error) {
    return insertPlaylist(library, name, "static", parentId, {}, error);
}

qint64 createFolder(Library &library, const QString &name, qint64 parentId, QString *error) {
    return insertPlaylist(library, name, "folder", parentId, {}, error);
}

qint64 createAuto(Library &library, const QString &name, const QString &query, qint64 parentId, QString *error) {
    const Query::Compiled c = Query::compile(query);
    if (!c.ok()) {
        fail(error, c.error);
        return 0;
    }
    return insertPlaylist(library, name, "auto", parentId, query, error);
}

bool rename(Library &library, qint64 id, const QString &name, QString *error) {
    if (name.trimmed().isEmpty())
        return fail(error, "A playlist needs a name");
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlists SET name = ?, modified = ? WHERE id = ?");
    q.addBindValue(name.trimmed());
    q.addBindValue(now());
    q.addBindValue(id);
    if (!q.exec())
        return fail(error, q.lastError().text());
    return q.numRowsAffected() == 1 || fail(error, QString("No playlist %1").arg(id));
}

bool setQuery(Library &library, qint64 id, const QString &query, QString *error) {
    Playlist p;
    if (!get(library, id, &p))
        return fail(error, QString("No playlist %1").arg(id));
    if (p.kind != "auto")
        return fail(error, QString("\"%1\" is not an auto-playlist").arg(p.name));
    const Query::Compiled c = Query::compile(query);
    if (!c.ok())
        return fail(error, c.error);
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlists SET query = ?, modified = ? WHERE id = ?");
    q.addBindValue(query);
    q.addBindValue(now());
    q.addBindValue(id);
    return q.exec() || fail(error, q.lastError().text());
}

bool setNote(Library &library, qint64 id, const QString &note, const QString &source) {
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlists SET note = ?, source = coalesce(?, source) WHERE id = ?");
    q.addBindValue(nullableText(note));
    q.addBindValue(nullableText(source));
    q.addBindValue(id);
    return q.exec();
}

bool move(Library &library, qint64 id, qint64 newParentId, int position, QString *error) {
    Playlist p;
    if (!get(library, id, &p))
        return fail(error, QString("No playlist %1").arg(id));
    // Walk up from the new parent: meeting the playlist itself means a loop.
    for (qint64 up = newParentId; up;) {
        if (up == id)
            return fail(error, "A playlist can't go inside itself");
        Playlist parent;
        if (!get(library, up, &parent))
            return fail(error, QString("No playlist %1 to put it in").arg(newParentId));
        up = parent.parentId;
    }
    Transaction tx(library.db());
    QList<qint64> siblings;
    for (const Playlist &s : children(library, newParentId))
        if (s.id != id)
            siblings << s.id;
    if (position < 0 || position > siblings.size())
        position = siblings.size();
    siblings.insert(position, id);
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlists SET parent_id = ?, modified = ? WHERE id = ?");
    q.addBindValue(nullable(newParentId));
    q.addBindValue(now());
    q.addBindValue(id);
    if (!q.exec())
        return fail(error, q.lastError().text());
    QSqlQuery u(library.db());
    u.prepare("UPDATE playlists SET position = ? WHERE id = ?");
    for (int i = 0; i < siblings.size(); ++i) {
        u.addBindValue(i);
        u.addBindValue(siblings[i]);
        if (!u.exec())
            return fail(error, u.lastError().text());
    }
    return tx.commit() || fail(error, library.db().lastError().text());
}

bool remove(Library &library, qint64 id, QString *error) {
    // Foreign keys cascade to the entries and the children.
    QSqlQuery q(library.db());
    q.prepare("DELETE FROM playlists WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec())
        return fail(error, q.lastError().text());
    return q.numRowsAffected() == 1 || fail(error, QString("No playlist %1").arg(id));
}

QList<Item> items(Library &library, qint64 id, QString *error) {
    Playlist p;
    if (!get(library, id, &p)) {
        fail(error, QString("No playlist %1").arg(id));
        return {};
    }
    if (p.kind == "folder")
        return {};
    if (p.kind == "auto") {
        Query::Context context;
        context.seed = quint32(p.seed);
        QString why;
        const QList<qint64> ids = Query::ids(library, p.query, context, &why);
        if (!why.isEmpty()) {
            fail(error, why);
            return {};
        }
        return trackItems(library, ids);
    }
    QList<Item> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT i.id, i.track_id, i.path, i.title, i.artist, i.duration_ms, v.root_path, t.rel_path, t.title, "
              "t.artist, t.album, t.duration_ms, t.availability, i.album, i.art FROM playlist_items i "
              "LEFT JOIN tracks t ON t.id = i.track_id LEFT JOIN volumes v ON v.id = t.volume_id "
              "WHERE i.playlist_id = ? ORDER BY i.position, i.id");
    q.addBindValue(id);
    if (!q.exec()) {
        fail(error, q.lastError().text());
        return {};
    }
    while (q.next()) {
        Item it;
        it.id = q.value(0).toLongLong();
        it.trackId = q.value(1).toLongLong();
        it.position = out.size();
        if (it.trackId) {
            it.path = joinPath(q.value(6).toString(), q.value(7).toString());
            it.title = q.value(8).toString();
            it.artist = q.value(9).toString();
            it.album = q.value(10).toString();
            it.durationMs = q.value(11).toLongLong();
            it.availability = q.value(12).toString();
        } else {
            it.path = q.value(2).toString();
            it.title = q.value(3).toString();
            it.artist = q.value(4).toString();
            it.durationMs = q.value(5).toLongLong();
            it.album = q.value(13).toString();
            it.art = q.value(14).toString();
            it.availability = isSpotifyPath(it.path) ? "spotify" : "unmatched";
        }
        out << it;
    }
    return out;
}

QList<qint64> trackIds(Library &library, qint64 id, QString *error) {
    QList<qint64> out;
    for (const Item &it : items(library, id, error))
        if (it.trackId)
            out << it.trackId;
    return out;
}

qint64 reroll(Library &library, qint64 id, qint64 seed) {
    if (seed < 0)
        seed = QRandomGenerator::global()->bounded(1u << 31);
    QSqlQuery q(library.db());
    q.prepare("UPDATE playlists SET seed = ?, modified = ? WHERE id = ? AND kind = 'auto'");
    q.addBindValue(seed);
    q.addBindValue(now());
    q.addBindValue(id);
    return q.exec() && q.numRowsAffected() == 1 ? seed : -1;
}

int addTracks(Library &library, qint64 id, const QList<qint64> &ids, int at, QString *error) {
    QList<Entry> entries;
    for (const Item &it : trackItems(library, ids)) {
        Entry e;
        e.trackId = it.trackId;
        e.path = it.path;
        e.title = it.title;
        e.artist = it.artist;
        e.durationMs = it.durationMs;
        entries << e;
    }
    if (entries.size() != ids.size()) {
        fail(error, QString("%1 of the tracks are not in the library").arg(ids.size() - entries.size()));
        return -1;
    }
    return addEntries(library, id, entries, at, error);
}

int addEntries(Library &library, qint64 id, const QList<Entry> &entries, int at, QString *error) {
    if (!requireStatic(library, id, error))
        return -1;
    Transaction tx(library.db());
    QList<qint64> order = orderedItemIds(library, id);
    if (at < 0 || at > order.size())
        at = order.size();
    QSqlQuery q(library.db());
    q.prepare("INSERT INTO playlist_items (playlist_id, track_id, position, path, title, artist, duration_ms, added, "
              "album, art) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    QList<qint64> added;
    for (const Entry &e : entries) {
        q.addBindValue(id);
        q.addBindValue(nullable(e.trackId));
        q.addBindValue(0);
        q.addBindValue(nullableText(e.path));
        q.addBindValue(nullableText(e.title));
        q.addBindValue(nullableText(e.artist));
        q.addBindValue(e.durationMs > 0 ? QVariant(e.durationMs) : QVariant(QMetaType(QMetaType::LongLong)));
        q.addBindValue(now());
        q.addBindValue(nullableText(e.album));
        q.addBindValue(nullableText(e.art));
        if (!q.exec()) {
            fail(error, q.lastError().text());
            return -1;
        }
        added << q.lastInsertId().toLongLong();
    }
    for (int i = 0; i < added.size(); ++i)
        order.insert(at + i, added[i]);
    if (!writePositions(library, order, error))
        return -1;
    touch(library, id);
    if (!tx.commit()) {
        fail(error, library.db().lastError().text());
        return -1;
    }
    return added.size();
}

int removeItems(Library &library, qint64 id, const QList<qint64> &itemIds, QString *error) {
    if (!requireStatic(library, id, error))
        return -1;
    Transaction tx(library.db());
    QList<qint64> order = orderedItemIds(library, id);
    QSqlQuery q(library.db());
    q.prepare("DELETE FROM playlist_items WHERE id = ? AND playlist_id = ?");
    int removed = 0;
    for (qint64 item : itemIds) {
        q.addBindValue(item);
        q.addBindValue(id);
        if (q.exec() && q.numRowsAffected() == 1) {
            order.removeOne(item);
            ++removed;
        }
    }
    if (!writePositions(library, order, error))
        return -1;
    touch(library, id);
    tx.commit();
    return removed;
}

int removePositions(Library &library, qint64 id, const QList<int> &positions, QString *error) {
    const QList<qint64> order = orderedItemIds(library, id);
    QList<qint64> ids;
    for (int p : positions) {
        if (p < 0 || p >= order.size()) {
            fail(error, QString("There is no entry %1; the playlist has %2").arg(p + 1).arg(order.size()));
            return -1;
        }
        ids << order[p];
    }
    return removeItems(library, id, ids, error);
}

bool moveItems(Library &library, qint64 id, const QList<qint64> &itemIds, int to, QString *error) {
    if (!requireStatic(library, id, error))
        return false;
    Transaction tx(library.db());
    QList<qint64> order = orderedItemIds(library, id);
    const QSet<qint64> moving(itemIds.begin(), itemIds.end());
    QList<qint64> picked, rest;
    for (qint64 item : order)
        (moving.contains(item) ? picked : rest) << item;
    if (picked.size() != moving.size())
        return fail(error, "Some of those entries are not in this playlist");
    to = qBound(0, to, int(rest.size()));
    for (int i = 0; i < picked.size(); ++i)
        rest.insert(to + i, picked[i]);
    if (!writePositions(library, rest, error))
        return false;
    touch(library, id);
    return tx.commit() || fail(error, library.db().lastError().text());
}

bool reorder(Library &library, qint64 id, const QList<qint64> &itemIds, QString *error) {
    if (!requireStatic(library, id, error))
        return false;
    QList<qint64> current = orderedItemIds(library, id);
    QList<qint64> wanted = itemIds;
    std::sort(current.begin(), current.end());
    std::sort(wanted.begin(), wanted.end());
    if (current != wanted)
        return fail(error, "A new order must name every entry of the playlist once");
    Transaction tx(library.db());
    if (!writePositions(library, itemIds, error))
        return false;
    touch(library, id);
    return tx.commit() || fail(error, library.db().lastError().text());
}

bool clear(Library &library, qint64 id, QString *error) {
    if (!requireStatic(library, id, error))
        return false;
    QSqlQuery q(library.db());
    q.prepare("DELETE FROM playlist_items WHERE playlist_id = ?");
    q.addBindValue(id);
    if (!q.exec())
        return fail(error, q.lastError().text());
    touch(library, id);
    return true;
}

} // namespace Playlists

namespace Collections {

namespace {

QJsonArray parseArray(const QString &text) {
    return text.isEmpty() ? QJsonArray() : QJsonDocument::fromJson(text.toUtf8()).array();
}

QVariant arrayText(const QJsonArray &a) {
    return a.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                       : QVariant(QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
}

struct Default {
    const char *name, *query;
};
const Default defaults[] = {{"Music", "type:music"},
                            {"Classical", "type:classical"},
                            {"Audiobooks", "type:audiobook"},
                            {"Podcasts", "type:podcast"}};

bool valid(const Collection &c, QString *error) {
    if (c.name.trimmed().isEmpty())
        return fail(error, "A collection needs a name");
    if (!QStringList{"always", "never", "nonempty"}.contains(c.visible))
        return fail(error, "visible is always, never or nonempty");
    const Query::Compiled compiled = Query::compile(c.query);
    if (!compiled.ok())
        return fail(error, compiled.error);
    if (!c.sort.isEmpty()) {
        const Query::Compiled sorted = Query::compile("sort:" + c.sort);
        if (!sorted.ok())
            return fail(error, sorted.error);
    }
    return true;
}

} // namespace

QJsonObject Collection::toJson() const {
    return {{"id", id},       {"name", name}, {"query", query},     {"columns", columns},
            {"tree", tree},   {"sort", sort}, {"view", view},       {"visible", visible},
            {"position", position}};
}

static Collection readCollection(const QSqlQuery &q) {
    Collection c;
    c.id = q.value(0).toLongLong();
    c.name = q.value(1).toString();
    c.query = q.value(2).toString();
    c.columns = parseArray(q.value(3).toString());
    c.tree = parseArray(q.value(4).toString());
    c.sort = q.value(5).toString();
    c.view = q.value(6).toString();
    c.visible = q.value(7).toString();
    c.position = q.value(8).toInt();
    return c;
}

static const char *collectionColumns = "id, name, query, columns, tree, sort, view, visible, position";

QList<Collection> list(Library &library) {
    QList<Collection> out;
    QSqlQuery q(library.db());
    if (q.exec(QString("SELECT %1 FROM collections ORDER BY position, name").arg(collectionColumns)))
        while (q.next())
            out << readCollection(q);
    return out;
}

bool get(Library &library, qint64 id, Collection *out) {
    QSqlQuery q(library.db());
    q.prepare(QString("SELECT %1 FROM collections WHERE id = ?").arg(collectionColumns));
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return false;
    if (out)
        *out = readCollection(q);
    return true;
}

qint64 resolve(Library &library, const QString &idOrName, QString *error) {
    bool isNumber = false;
    const qint64 asId = idOrName.toLongLong(&isNumber);
    if (isNumber && get(library, asId, nullptr))
        return asId;
    QSqlQuery q(library.db());
    q.prepare("SELECT id FROM collections WHERE name = ? COLLATE NOCASE");
    q.addBindValue(idOrName);
    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    fail(error, QString("No collection called %1").arg(idOrName));
    return 0;
}

qint64 create(Library &library, const Collection &c, QString *error) {
    if (!valid(c, error))
        return 0;
    QSqlQuery q(library.db());
    q.prepare("INSERT INTO collections (name, query, columns, tree, sort, view, visible, position) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, (SELECT coalesce(max(position) + 1, 0) FROM collections))");
    q.addBindValue(c.name.trimmed());
    q.addBindValue(c.query);
    q.addBindValue(arrayText(c.columns));
    q.addBindValue(arrayText(c.tree));
    q.addBindValue(nullableText(c.sort));
    q.addBindValue(nullableText(c.view));
    q.addBindValue(c.visible);
    if (!q.exec()) {
        fail(error, q.lastError().text().contains("UNIQUE") ? QString("There is already a collection called %1")
                                                                    .arg(c.name)
                                                              : q.lastError().text());
        return 0;
    }
    return q.lastInsertId().toLongLong();
}

bool update(Library &library, const Collection &c, QString *error) {
    if (!valid(c, error))
        return false;
    QSqlQuery q(library.db());
    q.prepare("UPDATE collections SET name = ?, query = ?, columns = ?, tree = ?, sort = ?, view = ?, visible = ?, "
              "position = ? WHERE id = ?");
    q.addBindValue(c.name.trimmed());
    q.addBindValue(c.query);
    q.addBindValue(arrayText(c.columns));
    q.addBindValue(arrayText(c.tree));
    q.addBindValue(nullableText(c.sort));
    q.addBindValue(nullableText(c.view));
    q.addBindValue(c.visible);
    q.addBindValue(c.position);
    q.addBindValue(c.id);
    if (!q.exec())
        return fail(error, q.lastError().text());
    return q.numRowsAffected() == 1 || fail(error, QString("No collection %1").arg(c.id));
}

bool remove(Library &library, qint64 id, QString *error) {
    QSqlQuery q(library.db());
    q.prepare("DELETE FROM collections WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec())
        return fail(error, q.lastError().text());
    return q.numRowsAffected() == 1 || fail(error, QString("No collection %1").arg(id));
}

int ensureDefaults(Library &library) {
    int added = 0;
    for (const Default &d : defaults) {
        QSqlQuery q(library.db());
        q.prepare("INSERT OR IGNORE INTO collections (name, query, position) "
                  "VALUES (?, ?, (SELECT coalesce(max(position) + 1, 0) FROM collections))");
        q.addBindValue(QString(d.name));
        q.addBindValue(QString(d.query));
        if (q.exec())
            added += q.numRowsAffected();
    }
    return added;
}

QList<qint64> trackIds(Library &library, qint64 id, const Query::Context &context, QString *error) {
    Collection c;
    if (!get(library, id, &c)) {
        fail(error, QString("No collection %1").arg(id));
        return {};
    }
    // The collection's sort applies unless its query has a sort: of its own.
    QString text = c.query;
    if (!c.sort.isEmpty() && Query::parse(c.query).sort.isEmpty())
        text += " sort:" + c.sort;
    return Query::ids(library, text, context, error);
}

} // namespace Collections
