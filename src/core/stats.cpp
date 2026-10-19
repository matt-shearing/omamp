#include "stats.h"

#include "library.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>

namespace Stats {

QString PlayResult::outcomeName() const {
    switch (outcome) {
    case Outcome::Counted:
        return "counted";
    case Outcome::Skipped:
        return "skipped";
    case Outcome::Ignored:
        break;
    }
    return "ignored";
}

QJsonObject PlayResult::toJson() const {
    QJsonObject o{{"ok", ok()}, {"outcome", outcomeName()}, {"history_id", historyId}};
    if (!ok())
        o["error"] = error;
    return o;
}

Outcome classify(qint64 playedMs, qint64 durationMs, bool userSkipped, const Rules &rules) {
    playedMs = qMax<qint64>(0, playedMs);
    const bool known = durationMs > 0;
    if (playedMs >= rules.countMs || (known && playedMs >= durationMs * rules.countFraction))
        return Outcome::Counted;
    if (userSkipped || (known && playedMs < durationMs * rules.skipFraction))
        return Outcome::Skipped;
    return Outcome::Ignored;
}

PlayResult recordPlay(Library &library, qint64 trackId, qint64 playedMs, qint64 durationMs, bool userSkipped,
                      qint64 at, const Rules &rules) {
    PlayResult r;
    QSqlDatabase db = library.db();
    QSqlQuery q(db);
    q.prepare("SELECT duration_ms FROM tracks WHERE id = ?");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next()) {
        r.error = QString("No track %1").arg(trackId);
        return r;
    }
    if (durationMs <= 0)
        durationMs = q.value(0).toLongLong();
    q.finish();
    if (at <= 0)
        at = QDateTime::currentSecsSinceEpoch();
    r.outcome = classify(playedMs, durationMs, userSkipped, rules);
    if (r.outcome == Outcome::Ignored)
        return r;
    const bool counted = r.outcome == Outcome::Counted;
    const bool own = db.transaction();
    QSqlQuery h(db);
    h.prepare("INSERT INTO play_history (track_id, played_at, played_ms, skipped) VALUES (?, ?, ?, ?)");
    h.addBindValue(trackId);
    h.addBindValue(at);
    h.addBindValue(qMax<qint64>(0, playedMs));
    h.addBindValue(counted ? 0 : 1);
    QSqlQuery u(db);
    u.prepare(counted ? "UPDATE tracks SET play_count = play_count + 1, "
                        "last_played = max(coalesce(last_played, 0), ?) WHERE id = ?"
                      : "UPDATE tracks SET skip_count = skip_count + 1 WHERE id = ?");
    if (counted)
        u.addBindValue(at);
    u.addBindValue(trackId);
    if (!h.exec() || !u.exec()) {
        r.error = (h.lastError().isValid() ? h.lastError() : u.lastError()).text();
        if (own)
            db.rollback();
        return r;
    }
    r.historyId = h.lastInsertId().toLongLong();
    if (own && !db.commit())
        r.error = db.lastError().text();
    return r;
}

bool parseBy(const QString &text, By *out) {
    const QString t = text.toLower();
    if (t == "tracks" || t == "track" || t == "songs")
        *out = By::Tracks;
    else if (t == "artists" || t == "artist")
        *out = By::Artists;
    else if (t == "albums" || t == "album")
        *out = By::Albums;
    else if (t == "years" || t == "year")
        *out = By::Years;
    else if (t == "genres" || t == "genre")
        *out = By::Genres;
    else
        return false;
    return true;
}

QString byName(By by) {
    switch (by) {
    case By::Tracks:
        return "tracks";
    case By::Artists:
        return "artists";
    case By::Albums:
        return "albums";
    case By::Years:
        return "years";
    case By::Genres:
        return "genres";
    }
    return {};
}

QJsonObject Row::toJson() const {
    QJsonObject o{{"id", id},         {"name", name},     {"plays", plays},          {"skips", skips},
                  {"listened_ms", listenedMs}, {"tracks", tracks}, {"last_played", lastPlayed ? QJsonValue(lastPlayed) : QJsonValue()}};
    if (!detail.isEmpty())
        o["detail"] = detail;
    return o;
}

QList<Row> top(Library &library, const TopOptions &options, QString *error) {
    // One shape for both sources: per track, plays, skips, time heard and the
    // last play. All time reads the tracks' own counters (which include
    // imported counts) and estimates time heard as plays x length.
    const QString source =
        options.since > 0
            ? QString("SELECT track_id, sum(skipped = 0) AS plays, sum(skipped) AS skips, "
                      "sum(CASE WHEN skipped = 0 THEN coalesce(played_ms, 0) ELSE 0 END) AS ms, "
                      "max(CASE WHEN skipped = 0 THEN played_at END) AS last "
                      "FROM play_history WHERE played_at >= %1 GROUP BY track_id")
                  .arg(options.since)
            : QString("SELECT id AS track_id, play_count AS plays, skip_count AS skips, "
                      "play_count * coalesce(duration_ms, 0) AS ms, last_played AS last FROM tracks "
                      "WHERE play_count > 0");
    const QString from = "FROM (" + source + ") h JOIN tracks t ON t.id = h.track_id ";
    const QString totals = "sum(h.plays), sum(h.skips), sum(h.ms), count(DISTINCT t.id), max(h.last)";
    QString sql;
    switch (options.by) {
    case By::Tracks:
        sql = "SELECT t.id, coalesce(nullif(t.title, ''), t.file_name), t.artist, h.plays, h.skips, h.ms, 1, h.last " +
              from + "WHERE h.plays > 0 ORDER BY h.plays DESC, h.last DESC, t.id";
        break;
    case By::Artists:
        sql = "SELECT a.id, a.name, '', " + totals + ' ' + from +
              "JOIN track_artists ta ON ta.track_id = t.id AND ta.role = 'artist' "
              "JOIN artists a ON a.id = ta.artist_id GROUP BY a.id HAVING sum(h.plays) > 0 "
              "ORDER BY 4 DESC, 8 DESC, a.name";
        break;
    case By::Albums:
        sql = "SELECT al.id, al.title, coalesce(al.album_artist, ''), " + totals + ' ' + from +
              "JOIN albums al ON al.id = t.album_id GROUP BY al.id HAVING sum(h.plays) > 0 "
              "ORDER BY 4 DESC, 8 DESC, al.title";
        break;
    case By::Years:
        sql = "SELECT t.year, CAST(t.year AS TEXT), '', " + totals + ' ' + from +
              "WHERE t.year IS NOT NULL GROUP BY t.year HAVING sum(h.plays) > 0 ORDER BY 4 DESC, t.year DESC";
        break;
    case By::Genres:
        sql = "SELECT g.id, g.name, '', " + totals + ' ' + from +
              "JOIN track_genres tg ON tg.track_id = t.id JOIN genres g ON g.id = tg.genre_id "
              "GROUP BY g.id HAVING sum(h.plays) > 0 ORDER BY 4 DESC, 8 DESC, g.name";
        break;
    }
    if (options.limit > 0)
        sql += QString(" LIMIT %1").arg(options.limit);
    QList<Row> out;
    QSqlQuery q(library.db());
    if (!q.exec(sql)) {
        if (error)
            *error = q.lastError().text();
        return out;
    }
    while (q.next()) {
        Row r;
        r.id = q.value(0).toLongLong();
        r.name = q.value(1).toString();
        r.detail = q.value(2).toString();
        r.plays = q.value(3).toLongLong();
        r.skips = q.value(4).toLongLong();
        r.listenedMs = q.value(5).toLongLong();
        r.tracks = q.value(6).toLongLong();
        r.lastPlayed = q.value(7).toLongLong();
        out << r;
    }
    return out;
}

QJsonObject Played::toJson() const {
    return {{"track_id", trackId}, {"title", title},       {"artist", artist},  {"album", album},
            {"played_at", playedAt}, {"played_ms", playedMs}, {"skipped", skipped}};
}

QList<Played> recent(Library &library, int limit, bool withSkips) {
    QList<Played> out;
    QSqlQuery q(library.db());
    q.prepare(QString("SELECT h.track_id, coalesce(nullif(t.title, ''), t.file_name), t.artist, t.album, "
                      "h.played_at, coalesce(h.played_ms, 0), h.skipped FROM play_history h "
                      "JOIN tracks t ON t.id = h.track_id %1 ORDER BY h.played_at DESC, h.id DESC LIMIT ?")
                  .arg(withSkips ? "" : "WHERE h.skipped = 0"));
    q.addBindValue(limit > 0 ? limit : -1);
    if (q.exec())
        while (q.next()) {
            Played p;
            p.trackId = q.value(0).toLongLong();
            p.title = q.value(1).toString();
            p.artist = q.value(2).toString();
            p.album = q.value(3).toString();
            p.playedAt = q.value(4).toLongLong();
            p.playedMs = q.value(5).toLongLong();
            p.skipped = q.value(6).toBool();
            out << p;
        }
    return out;
}

QList<qint64> neverPlayed(Library &library, int limit) {
    QList<qint64> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT id FROM tracks WHERE play_count = 0 AND last_played IS NULL ORDER BY added DESC, id LIMIT ?");
    q.addBindValue(limit > 0 ? limit : -1);
    if (q.exec())
        while (q.next())
            out << q.value(0).toLongLong();
    return out;
}

QJsonObject Summary::toJson() const {
    return {{"plays", plays}, {"skips", skips}, {"listened_ms", listenedMs}, {"tracks", tracks},
            {"never_played", neverPlayed}};
}

Summary summary(Library &library, qint64 since) {
    Summary s;
    QSqlQuery q(library.db());
    q.prepare("SELECT coalesce(sum(skipped = 0), 0), coalesce(sum(skipped), 0), "
              "coalesce(sum(CASE WHEN skipped = 0 THEN played_ms ELSE 0 END), 0), count(DISTINCT track_id) "
              "FROM play_history WHERE played_at >= ?");
    q.addBindValue(since);
    if (q.exec() && q.next()) {
        s.plays = q.value(0).toLongLong();
        s.skips = q.value(1).toLongLong();
        s.listenedMs = q.value(2).toLongLong();
        s.tracks = q.value(3).toLongLong();
    }
    QSqlQuery n(library.db());
    if (n.exec("SELECT count(*) FROM tracks WHERE play_count = 0 AND last_played IS NULL") && n.next())
        s.neverPlayed = n.value(0).toLongLong();
    return s;
}

qint64 parseSince(const QString &text, qint64 now) {
    if (now <= 0)
        now = QDateTime::currentSecsSinceEpoch();
    const QString t = text.trimmed().toLower();
    static const QRegularExpression age("^(\\d+)\\s*(h|d|w|m|y)$");
    const auto m = age.match(t);
    if (m.hasMatch()) {
        const qint64 n = m.captured(1).toLongLong();
        const QDateTime from = QDateTime::fromSecsSinceEpoch(now);
        const QString unit = m.captured(2);
        if (unit == "h")
            return now - n * 3600;
        if (unit == "d")
            return now - n * 86400;
        if (unit == "w")
            return now - n * 7 * 86400;
        if (unit == "m")
            return from.addMonths(-int(n)).toSecsSinceEpoch();
        return from.addYears(-int(n)).toSecsSinceEpoch();
    }
    for (const char *format : {"yyyy-MM-dd", "yyyy-MM", "yyyy"}) {
        const QDate d = QDate::fromString(t, format);
        if (d.isValid())
            return QDateTime(d, QTime(0, 0)).toSecsSinceEpoch();
    }
    return -1;
}

}
