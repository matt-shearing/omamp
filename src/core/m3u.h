#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class Library;

// Playlist files: M3U, M3U8 and PLS in, M3U8 and PLS out, plus the matcher
// that finds a library track for a path or for an artist and title. The
// MediaMonkey importer uses the same matcher.
namespace PlaylistFiles {

struct FileEntry {
    QString location;   // exactly as written in the file
    QString path;       // absolute and cleaned, or a URL, or a Windows path left as written
    QString title, artist;
    qint64 durationMs = -1; // -1 when the file doesn't say
};

struct Parsed {
    QString name; // the file's base name
    QList<FileEntry> entries;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// By extension: .pls is PLS, anything else M3U. M3U files may be UTF-8
// (always, for .m3u8) or Latin-1; a byte-order mark is skipped. Relative
// paths are taken from the playlist's own folder; file:// URLs become paths;
// backslashes in relative paths become slashes.
Parsed read(const QString &file);
Parsed readM3u(const QString &file);
Parsed readPls(const QString &file);

// Finds library tracks. Loads every track's path and tags once, so matching a
// large playlist or a whole MediaMonkey library is a hash lookup per entry.
class Matcher {
public:
    explicit Matcher(Library &library);

    // An exact absolute path, then the same path ignoring case if only one
    // track fits (a playlist made on a case-insensitive filesystem).
    qint64 byPath(const QString &absolutePath) const;
    // The last `parts` path components ("Artist/Album/01 Song.flac"), ignoring
    // case and slash direction; only a single hit counts.
    qint64 bySuffix(const QString &anyPath, int parts) const;
    // Artist and title ignoring case, within toleranceMs of the length when
    // both lengths are known. Several hits: the same album first, then the
    // closest length, then the lowest id.
    qint64 byTags(const QString &artist, const QString &title, qint64 durationMs = -1, const QString &album = {},
                  qint64 toleranceMs = 3000) const;

    QString pathOf(qint64 trackId) const { return m_paths.value(trackId); }
    int size() const { return m_paths.size(); }

private:
    struct Row {
        qint64 id;
        qint64 durationMs;
        QString album;
    };
    QHash<QString, qint64> m_exact;
    QHash<QString, QList<qint64>> m_folded;
    QHash<QString, QList<Row>> m_tags;
    QHash<qint64, QString> m_paths;
    mutable QHash<int, QHash<QString, QList<qint64>>> m_suffixes;
};

struct ImportResult {
    QString file;
    qint64 playlistId = 0;
    QString name;
    // spotify: Spotify tracks (open.spotify.com links or spotify: URIs),
    // kept as Spotify entries.
    int entries = 0, byPath = 0, byTags = 0, unmatched = 0, spotify = 0;
    QStringList unmatchedLocations;
    QString error;
    bool ok() const { return error.isEmpty(); }
    QJsonObject toJson() const;
};

// Makes a static playlist from a playlist file. Entries that match nothing
// are kept, with their path and #EXTINF title, as unmatched entries.
ImportResult import(Library &library, const QString &file, qint64 parentId = 0, const QString &name = {},
                    const Matcher *matcher = nullptr);
// Imports every .m3u, .m3u8 and .pls under a folder into a playlist folder
// named after it (or `name`), with a nested folder for each subfolder that
// holds playlists.
QList<ImportResult> importFolder(Library &library, const QString &folder, qint64 parentId = 0,
                                 const QString &name = {}, qint64 *folderId = nullptr, QString *error = nullptr);

struct ExportOptions {
    bool relative = false; // paths relative to the playlist file's folder
};
// Writes an M3U8 (for .m3u and .m3u8) or a PLS (for .pls) with each entry's
// length, artist and title. A Spotify entry is written as its
// https://open.spotify.com/track/<id> link, which import reads back. Auto-playlists export their current tracks.
// Written beside the target and renamed into place.
bool exportFile(Library &library, qint64 playlistId, const QString &file, const ExportOptions &options = {},
                int *written = nullptr, QString *error = nullptr);

}
