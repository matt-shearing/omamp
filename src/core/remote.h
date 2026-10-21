#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <memory>

class Job;
class Library;
class SubsonicClient;

// Where a remote source's password or API key lives. Never in the library
// database, which is copied by backups and read by anything that opens it.
//
// - libsecret: the desktop keyring (gnome-keyring, KWallet's Secret Service),
//   through the `secret-tool` command, under the attributes
//   application=thrum remote=<ref>. Used when secret-tool is installed and a
//   session bus is there.
// - file: ~/.config/thrum/credentials.json (following XDG_CONFIG_HOME),
//   created with mode 0600 inside a 0700 folder. The fallback, and what
//   THRUM_SECRET_STORE=file asks for (tests always do). Anyone who can read
//   your home folder as you can read it; the keyring is better.
namespace Secrets {

QString preferredStore();          // "libsecret" or "file"
QString filePath();
bool store(const QString &storeName, const QString &ref, const QString &secret, const QString &label, QString *error);
QString lookup(const QString &storeName, const QString &ref, QString *error);
bool remove(const QString &storeName, const QString &ref);

}

struct RemoteSource {
    qint64 id = 0;
    QString kind = "subsonic";
    QString name, url, username;
    QString auth;          // "token" or "apikey"
    QString secretStore;   // "libsecret" or "file"
    QString secretRef;
    QString serverType, serverVersion, apiVersion;
    bool openSubsonic = false;
    int maxBitrate = 0;    // kbit/s for streaming; 0 is the original
    QString streamFormat;  // "" (server default), "raw", "mp3", "opus", ...
    qint64 volumeId = 0, lastSync = 0;
    QString lastError;
    QJsonObject toJson() const;
};

struct RemoteSyncResult {
    bool ok = true;
    QString error;
    qint64 albums = 0;          // albums the server listed
    qint64 albumsFetched = 0;   // new or changed, so fetched with getAlbum
    qint64 albumsUnchanged = 0;
    qint64 albumsGone = 0;
    qint64 tracksAdded = 0, tracksUpdated = 0, tracksMissing = 0, tracksBack = 0;
    int requests = 0;
    qint64 elapsedMs = 0;
    bool cancelled = false;
    QJsonObject toJson() const;
};

// OpenSubsonic and Subsonic servers (Navidrome on a NAS, typically) as
// library sources. Their songs are mirrored into the library as tracks on a
// volume of kind "subsonic" whose rel_path is the server's song id, so
// search, browsing, playlists, ratings and Auto-DJ work on them like on
// files. Playing one goes through Library::playableUrl(), which gives the
// stream URL.
namespace RemoteSources {

// Signs in (ping), stores the secret, and records the source. name defaults
// to the server's host. auth is "token" (secret is the password) or
// "apikey". Nothing is synced yet.
qint64 add(Library &library, const QUrl &url, const QString &user, const QString &secret, const QString &auth,
           const QString &name, QString *error, RemoteSource *added = nullptr);
QList<RemoteSource> list(Library &library);
// By id or name.
RemoteSource find(Library &library, const QString &which, QString *error);
// Forgets the source, its secret, and (unless keepTracks) its volume and
// tracks. With keepTracks the tracks stay, offline.
bool remove(Library &library, qint64 id, bool keepTracks, QString *error);
bool setStreaming(Library &library, qint64 id, int maxBitrate, const QString &format, QString *error);

// A signed-in client for a source, or null with error set.
std::unique_ptr<SubsonicClient> client(const Library &library, const RemoteSource &source, QString *error);

// Mirrors the server's library into this one. Albums are listed page by
// page (getAlbumList2, 500 at a time); only albums that are new or whose
// listing changed (name, artist, song count, length, year, genre, cover,
// created, changed) are fetched song by song. full fetches every album.
// Songs no longer on the server become missing; songs that come back are
// online again with their history.
RemoteSyncResult sync(Library &library, qint64 id, bool full = false, Job *job = nullptr);

// The stream URL for one song of a source. Reads the secret once per
// process and caches it.
QUrl streamUrl(const Library &library, qint64 sourceId, const QString &songId, QString *error);

// A cached copy of a track's (or album's) cover from the server, as a file
// path under ~/.cache/thrum/subsonic/<source>/, fetched on first use.
QString coverArt(Library &library, qint64 trackId, int size, QString *error);

// Tell the server about a play, or set a rating (0 to 100 as the library
// keeps them; the server gets whole stars).
bool scrobble(Library &library, qint64 trackId, bool submission, qint64 timeMs, QString *error);
bool setRating(Library &library, qint64 trackId, int rating, QString *error);

// Drops the cached secrets (after a source is removed or changed).
void clearSecretCache();

}
