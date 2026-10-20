#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "query.h"

class Library;

// Playlists, auto-playlists, playlist folders and collections.
//
// A playlist row is one of three kinds:
//
// - static: an ordered list of entries. The same track may appear more than
//   once. Each entry remembers the path, title, artist and length it was
//   added with, so an entry whose track goes missing, goes offline or is
//   removed from the library stays in the list (playlist_items.track_id
//   becomes NULL) and still exports.
// - auto: saved query-language text, evaluated live every time it is read.
//   sort: and limit: clauses live in the text. A random auto-playlist
//   (sort:random without its own seed) uses the seed stored on the row, so it
//   stays the same until reroll() picks a new one.
// - folder: holds other playlists and no entries.
//
// Any playlist may hold child playlists (MediaMonkey allows it), so
// parent_id is free; a folder is simply a playlist with no entries of its own.
// Siblings are ordered by position, then name.
namespace Playlists {

struct Playlist {
    qint64 id = 0;
    qint64 parentId = 0; // 0 at the top level
    QString name, kind;  // kind: static, auto, folder
    QString query;       // auto only
    qint64 seed = 0;     // auto only: the shuffle a random auto-playlist uses
    QString note, source;
    int position = 0;
    qint64 created = 0, modified = 0;
    int items = 0; // static entries; for auto, -1 (count with trackIds)
    QJsonObject toJson() const;
};

struct Item {
    qint64 id = 0;      // playlist_items.id; 0 for an auto-playlist's rows
    qint64 trackId = 0; // 0 when the entry matches no track
    int position = 0;   // 0-based
    QString path;       // absolute; the track's current path when it has one
    QString title, artist, album;
    qint64 durationMs = 0;
    // online, offline, missing (the track's state), unmatched (no track),
    // or spotify (a Spotify track: path spotify://track/<id>).
    QString availability;
    // A Spotify entry's album id or picture URL, as it was added.
    QString art;
    bool isSpotify() const { return availability == QLatin1String("spotify"); }
    QJsonObject toJson() const;
};

// An entry to add that may or may not be a library track. A Spotify track
// is an entry with path spotify://track/<id> and no trackId; album and art
// (its album id or a picture URL) let it show without asking Spotify.
struct Entry {
    qint64 trackId = 0;
    QString path, title, artist;
    qint64 durationMs = 0;
    QString album, art;
};

// Spotify tracks in playlists. A playlist entry for one has the path
// spotify://track/<id>, the same path Now Playing uses.
bool isSpotifyPath(const QString &path);
// spotify://track/<id> from a spotify:track:<id> URI, an
// open.spotify.com/track/<id> link (with or without ?si=…) or the path
// itself; empty for anything else.
QString spotifyTrackPath(const QString &text);
// https://open.spotify.com/track/<id> for a spotify://track/<id> path.
QString spotifyWebUrl(const QString &path);

// Every playlist, parents before children, siblings in order.
QList<Playlist> list(Library &library);
QList<Playlist> children(Library &library, qint64 parentId);
bool get(Library &library, qint64 id, Playlist *out);
// An id, a name, or a path of names ("Road trips/Summer"). A bare name must
// be unique. Returns 0 and sets error when nothing (or more than one) fits.
qint64 resolve(Library &library, const QString &ref, QString *error = nullptr);

qint64 create(Library &library, const QString &name, qint64 parentId = 0, QString *error = nullptr);
qint64 createFolder(Library &library, const QString &name, qint64 parentId = 0, QString *error = nullptr);
// Checks the query parses before saving it.
qint64 createAuto(Library &library, const QString &name, const QString &query, qint64 parentId = 0,
                  QString *error = nullptr);
bool rename(Library &library, qint64 id, const QString &name, QString *error = nullptr);
bool setQuery(Library &library, qint64 id, const QString &query, QString *error = nullptr);
bool setNote(Library &library, qint64 id, const QString &note, const QString &source = {});
// Moves a playlist under another (0 for the top level) at a sibling
// position (-1 for the end). Refuses to move a playlist inside itself.
bool move(Library &library, qint64 id, qint64 newParentId, int position = -1, QString *error = nullptr);
// Deletes the playlist, its entries and its children.
bool remove(Library &library, qint64 id, QString *error = nullptr);

// The rows to show or play, in order. Auto-playlists run their query now.
QList<Item> items(Library &library, qint64 id, QString *error = nullptr);
// Just the track ids, in order (entries without a track are left out).
QList<qint64> trackIds(Library &library, qint64 id, QString *error = nullptr);
// A random auto-playlist's next shuffle; returns the new seed.
qint64 reroll(Library &library, qint64 id, qint64 seed = -1);

// Static playlists only. at is a 0-based position; -1 appends.
int addTracks(Library &library, qint64 id, const QList<qint64> &trackIds, int at = -1, QString *error = nullptr);
int addEntries(Library &library, qint64 id, const QList<Entry> &entries, int at = -1, QString *error = nullptr);
// Removes entries by playlist_items.id.
int removeItems(Library &library, qint64 id, const QList<qint64> &itemIds, QString *error = nullptr);
// Removes entries at 0-based positions.
int removePositions(Library &library, qint64 id, const QList<int> &positions, QString *error = nullptr);
// Moves the given entries, kept in their current order, to sit together
// starting at 0-based position `to` in the resulting list.
bool moveItems(Library &library, qint64 id, const QList<qint64> &itemIds, int to, QString *error = nullptr);
// Puts entries in exactly this order; every entry must be named once.
bool reorder(Library &library, qint64 id, const QList<qint64> &itemIds, QString *error = nullptr);
bool clear(Library &library, qint64 id, QString *error = nullptr);

}

// Collections: a saved query plus how to show it (columns, tree nodes, sort,
// view). Music, Classical, Audiobooks and Podcasts are made with every
// library from the content type; ensureDefaults() puts back any that were
// deleted.
namespace Collections {

struct Collection {
    qint64 id = 0;
    QString name, query;
    QJsonArray columns; // column ids in order, e.g. ["title","artist","album","length","rating"]
    QJsonArray tree;    // tree node ids in order, e.g. ["artist","album","genre","year"]
    QString sort;       // query-language sort text, e.g. "-rating,artist"
    QString view;       // details or grid
    QString visible = "always"; // always, never, nonempty
    int position = 0;
    QJsonObject toJson() const;
};

QList<Collection> list(Library &library);
bool get(Library &library, qint64 id, Collection *out);
qint64 resolve(Library &library, const QString &idOrName, QString *error = nullptr);
qint64 create(Library &library, const Collection &c, QString *error = nullptr);
bool update(Library &library, const Collection &c, QString *error = nullptr);
bool remove(Library &library, qint64 id, QString *error = nullptr);
// Adds Music, Classical, Audiobooks and Podcasts if missing; returns how many.
int ensureDefaults(Library &library);
// The collection's tracks, in its sort (or library order).
QList<qint64> trackIds(Library &library, qint64 id, const Query::Context &context = {}, QString *error = nullptr);

}
