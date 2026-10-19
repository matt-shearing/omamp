// A small library for the query and palette tests, built directly in SQL so
// dates, play counts and ratings are exact. All of it is made up.
#pragma once

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "library.h"

namespace Fixture {

// Noon on 20 October 2026, local time.
const QDateTime now(QDate(2026, 10, 20), QTime(12, 0));

inline qint64 daysAgo(double days) { return now.toSecsSinceEpoch() - qint64(days * 86400); }
inline qint64 on(int y, int m, int d, int hour = 12, int minute = 0) {
    return QDateTime(QDate(y, m, d), QTime(hour, minute)).toSecsSinceEpoch();
}

struct Row {
    QString title, artist, albumArtist, album, genre;
    QVariant year;
    int disc, track;
    qint64 durationMs;
    double sizeMb;
    QVariant rating; // 0..100 or null
    int plays;
    QVariant lastPlayed; // Unix seconds or null
    qint64 added;
    QString fileName;
    QString type = "music";
    int compilation = 0;
};

const QVariant null = QVariant(QMetaType(QMetaType::LongLong));

inline QList<Row> rows() {
    return {
        {"Midnight Highway", "Neon Coast", "Neon Coast", "Night Drive", "Synthwave", 1997, 1, 1, 245000, 8, 80, 12,
         daysAgo(3), daysAgo(400), "1-01 Midnight Highway"},
        {"Coastline", "Neon Coast", "Neon Coast", "Night Drive", "Synthwave", 1997, 1, 2, 185000, 6, 100, 30,
         daysAgo(10), daysAgo(400), "1-02 Coastline"},
        {"Afterglow", "Neon Coast; Lia", "Neon Coast", "Night Drive", "Synthwave; Pop", 1997, 2, 1, 320000, 10, null, 0,
         null, daysAgo(400), "2-01 Afterglow"},
        {"Heatwave", "Sun Machine", "", "Summer Mix", "Pop", 2004, 1, 1, 200000, 5, 60, 3, daysAgo(500), daysAgo(20),
         "01 Heatwave", "music", 1},
        {"Snow Day", "Frost Club", "", "Summer Mix", "Christmas; Pop", 2003, 1, 2, 150000, 4, 40, 0, null, daysAgo(20),
         "02 Snow Day", "music", 1},
        {"Tidal", "Björk Tribute", "", "Summer Mix", "Rock", 2001, 1, 3, 260000, 7, 90, 5, daysAgo(1), daysAgo(2),
         "03 Tidal", "music", 1},
        {"Chapter One", "Reader", "Reader", "The Book", "", null, 1, 1, 3600000, 60, null, 1, daysAgo(40),
         on(2024, 3, 15), "Chapter One", "audiobook"},
        {"", "", "", "", "", null, 0, 0, 61000, 1, null, 0, null, on(2024, 7, 1), "untagged-file"},
        {"Love Song", "The Lovers", "The Lovers", "Live at Home", "Rock", 1985, 1, 1, 600000, 15, 20, 50, daysAgo(400),
         on(2024, 1, 10), "01 Love Song"},
        {"Glove Box", "The Lovers", "The Lovers", "Live at Home", "Rock", 1985, 1, 2, 660000, 16, 0, 2, daysAgo(800),
         on(2024, 1, 10), "02 Glove Box"},
        {"Go West", "Pet Dogs", "Pet Dogs", "Go", "Pop", 1993, 1, 1, 290000, 7, null, 0, null, on(2025, 6, 30),
         "01 Go West"},
        {"夜の歌", "歌手", "歌手", "夜", "J-Pop", 2010, 1, 1, 180000, 3.5, null, 0, null, on(2023, 12, 31, 23, 30),
         "01 夜の歌"},
    };
}

// Fills a library with rows(), with the multi-value tables, as a scan would.
inline bool fill(Library &library) {
    QSqlDatabase db = library.db();
    QSqlQuery q(db);
    db.transaction();
    q.exec("INSERT INTO volumes (id, key, kind, root_path, online, added) VALUES (1, 'uuid:TEST', 'local', '/music', 1, 0)");
    QSqlQuery album(db), track(db), artist(db), link(db), genre(db), linkGenre(db);
    album.prepare("INSERT OR IGNORE INTO albums (title, album_artist, artist_key, title_key) VALUES (?, ?, ?, ?)");
    track.prepare("INSERT INTO tracks (volume_id, rel_path, file_name, format, size, mtime_ns, title, artist, "
                  "album_artist, sort_artist, album, album_id, genre, year, disc_no, track_no, duration_ms, rating, "
                  "play_count, last_played, added, content_type, compilation) VALUES (1, ?, ?, 'flac', ?, 0, ?, ?, "
                  "?, ?, ?, (SELECT id FROM albums WHERE title = ?), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    artist.prepare("INSERT OR IGNORE INTO artists (name) VALUES (?)");
    link.prepare("INSERT OR IGNORE INTO track_artists (track_id, artist_id, role, position) "
                 "VALUES (?, (SELECT id FROM artists WHERE name = ?), ?, ?)");
    genre.prepare("INSERT OR IGNORE INTO genres (name) VALUES (?)");
    linkGenre.prepare("INSERT OR IGNORE INTO track_genres (track_id, genre_id, position) "
                      "VALUES (?, (SELECT id FROM genres WHERE name = ?), ?)");
    auto nullable = [](const QString &s) { return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s); };
    for (const Row &r : rows()) {
        if (!r.album.isEmpty()) {
            const QString key = r.albumArtist.isEmpty() ? QString("Various Artists") : r.albumArtist;
            album.addBindValue(r.album);
            album.addBindValue(key);
            album.addBindValue(key.toLower());
            album.addBindValue(r.album.toLower());
            if (!album.exec())
                return false;
        }
        const QString sortArtist = !r.albumArtist.isEmpty() ? r.albumArtist
                                   : r.compilation          ? QString("Various Artists")
                                                            : r.artist;
        const QVariantList values = {
            r.album + '/' + r.fileName + ".flac", r.fileName, qint64(r.sizeMb * 1048576), nullable(r.title),
            nullable(r.artist), nullable(r.albumArtist), nullable(sortArtist), nullable(r.album), r.album,
            nullable(r.genre), r.year, r.disc ? QVariant(r.disc) : null, r.track ? QVariant(r.track) : null,
            r.durationMs, r.rating, r.plays, r.lastPlayed, r.added, r.type, r.compilation};
        for (const QVariant &v : values)
            track.addBindValue(v);
        if (!track.exec()) {
            qWarning() << track.lastError().text();
            return false;
        }
        const qint64 id = track.lastInsertId().toLongLong();
        auto people = [&](const QString &joined, const QString &role) {
            const QStringList names = joined.split("; ", Qt::SkipEmptyParts);
            for (int i = 0; i < names.size(); ++i) {
                artist.addBindValue(names[i]);
                artist.exec();
                link.addBindValue(id);
                link.addBindValue(names[i]);
                link.addBindValue(role);
                link.addBindValue(i);
                link.exec();
            }
        };
        people(r.artist, "artist");
        people(r.albumArtist, "album_artist");
        const QStringList genres = r.genre.split("; ", Qt::SkipEmptyParts);
        for (int i = 0; i < genres.size(); ++i) {
            genre.addBindValue(genres[i]);
            genre.exec();
            linkGenre.addBindValue(id);
            linkGenre.addBindValue(genres[i]);
            linkGenre.addBindValue(i);
            linkGenre.exec();
        }
    }
    return db.commit();
}

}
