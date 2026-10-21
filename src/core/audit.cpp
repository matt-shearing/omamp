#include "audit.h"

#include "library.h"

#include <QSqlError>
#include <QSqlQuery>

namespace Audits {

QJsonObject Audit::toJson() const {
    QJsonObject o{{"id", id}, {"title", title}, {"group", group}, {"description", description}};
    if (!query.isEmpty())
        o["query"] = query;
    else
        o["sql"] = sql;
    return o;
}

const QList<Audit> &audits() {
    static const QList<Audit> list = {
        {"unknown-title", "Unknown title", "Unknown", "Tracks with no title.", "title:none", {}},
        {"unknown-artist", "Unknown artist", "Unknown", "Tracks with no artist.", "artist:none", {}},
        {"unknown-album", "Unknown album", "Unknown", "Tracks with no album.", "album:none", {}},
        {"unknown-genre", "Unknown genre", "Unknown", "Tracks with no genre.", "genre:none", {}},
        {"unknown-year", "Unknown year", "Unknown", "Tracks with no year.", "year:none", {}},
        {"missing-artwork", "Missing artwork", "Artwork",
         "Tracks with no embedded picture whose album has no picture either. Albums the artwork cache has not "
         "looked at yet count as missing; thrum art all looks at every album.",
         {},
         "t.has_embedded_art = 0 AND NOT EXISTS (SELECT 1 FROM albums a WHERE a.id = t.album_id AND "
         "a.artwork_id IS NOT NULL)"},
        {"no-album-artist", "Several artists, no album artist", "Albums",
         "Albums whose tracks name different artists but no album artist, so they may split or sort apart. "
         "Compilations are left out.",
         {},
         "coalesce(t.album_artist, '') = '' AND t.compilation = 0 AND t.album_id IN (SELECT album_id FROM tracks "
         "WHERE album_id IS NOT NULL AND coalesce(album_artist, '') = '' GROUP BY album_id "
         "HAVING count(DISTINCT lower(coalesce(artist, ''))) > 1)"},
        {"mixed-album-artist", "Album artist differs within an album", "Albums",
         "Tracks of one album (same name, same folder) that give different album artists.",
         {},
         "coalesce(t.album, '') <> '' AND EXISTS (SELECT 1 FROM tracks o WHERE o.folder_id = t.folder_id AND "
         "lower(o.album) = lower(t.album) AND lower(coalesce(o.album_artist, '')) <> "
         "lower(coalesce(t.album_artist, '')))"},
        {"mixed-year", "Year differs within an album", "Albums", "Tracks of one album that give different years.",
         {},
         "t.album_id IS NOT NULL AND EXISTS (SELECT 1 FROM tracks o WHERE o.album_id = t.album_id AND "
         "coalesce(o.year, 0) <> coalesce(t.year, 0))"},
        {"mixed-genre", "Genre differs within an album", "Albums", "Tracks of one album that give different genres.",
         {},
         "t.album_id IS NOT NULL AND EXISTS (SELECT 1 FROM tracks o WHERE o.album_id = t.album_id AND "
         "lower(coalesce(o.genre, '')) <> lower(coalesce(t.genre, '')))"},
        {"no-track-number", "No track number", "Albums", "Tracks on an album with no track number.",
         "album:any track:none", {}},
        {"track-gaps", "Track numbers with gaps or repeats", "Albums",
         "Albums (per disc) whose track numbers skip a number or use one twice.",
         {},
         "t.album_id IN (SELECT album_id FROM tracks WHERE album_id IS NOT NULL AND track_no IS NOT NULL "
         "GROUP BY album_id, coalesce(disc_no, 1) "
         "HAVING max(track_no) > count(DISTINCT track_no) OR count(*) > count(DISTINCT track_no))"},
        {"unlevelled", "Not levelled", "Files", "Tracks with no ReplayGain track gain.", {},
         "t.rg_track_gain IS NULL"},
        {"dead-links", "Dead links", "Files", "Tracks whose file has gone from a volume that is online.",
         "status:missing", {}},
        {"offline", "Offline", "Files", "Tracks on a volume that is not mounted now.", "status:offline", {}},
        {"no-musicbrainz", "Not tagged from MusicBrainz", "Files", "Tracks with no MusicBrainz recording ID.", {},
         "coalesce(t.mb_track_id, '') = ''"},
    };
    return list;
}

const Audit *audit(const QString &id) {
    for (const Audit &a : audits())
        if (a.id == id)
            return &a;
    return nullptr;
}

Query::Compiled compile(const Audit &a) {
    if (!a.query.isEmpty())
        return Query::compile(a.query);
    Query::Compiled c;
    c.where = '(' + a.sql + ')';
    return c;
}

namespace {

bool runSql(QSqlQuery &q, const QString &sql, const QVariantList &params, QString *error) {
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    for (const QVariant &p : params)
        q.addBindValue(p);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

}

qint64 count(Library &library, const QString &id, QString *error) {
    const Audit *a = audit(id);
    if (!a) {
        if (error)
            *error = QString("No check called %1").arg(id);
        return -1;
    }
    const Query::Compiled c = compile(*a);
    if (!c.ok()) {
        if (error)
            *error = c.error;
        return -1;
    }
    QSqlQuery q(library.db());
    if (!runSql(q, "SELECT count(*) FROM tracks t WHERE " + c.where, c.params, error) || !q.next())
        return -1;
    return q.value(0).toLongLong();
}

QList<qint64> ids(Library &library, const QString &id, int limit, QString *error) {
    QList<qint64> out;
    const Audit *a = audit(id);
    if (!a) {
        if (error)
            *error = QString("No check called %1").arg(id);
        return out;
    }
    const Query::Compiled c = compile(*a);
    if (!c.ok()) {
        if (error)
            *error = c.error;
        return out;
    }
    QString sql = "SELECT t.id FROM tracks t WHERE " + c.where + " ORDER BY " + c.order();
    if (limit > 0)
        sql += QString(" LIMIT %1").arg(limit);
    QSqlQuery q(library.db());
    if (runSql(q, sql, c.params, error))
        while (q.next())
            out << q.value(0).toLongLong();
    return out;
}

QList<Count> counts(Library &library) {
    QList<Count> out;
    for (const Audit &a : audits())
        out << Count{a, count(library, a.id)};
    return out;
}

}
