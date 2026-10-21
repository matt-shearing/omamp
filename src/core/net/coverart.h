#pragma once

#include "http.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

class Job;
class Library;

// Front covers from the Cover Art Archive, by release or release group
// MBID, and fetching pictures for albums that have none.
namespace CoverArt {

struct Fetched {
    bool ok = false;
    bool notFound = false; // the archive has no front cover for it
    bool offline = false;
    QByteArray bytes;
    QString mime, url, error;
    QString releaseId, releaseGroupId; // where it came from
};

// The sizes the archive serves: 250, 500, 1200, or 0 for the original.
int snapSize(int requested);

// The front cover of a release, at the size asked for. Falls back to the
// release group's chosen cover when releaseGroupId is given and the release
// has none.
Fetched front(Net::HttpClient &http, const QString &releaseId, int size, const QString &releaseGroupId = {});

// What `thrum fetch-art` does for one album.
struct AlbumOptions {
    int size = 500;          // the archive's size; the setting coverart.size overrides 0
    bool embed = false;      // write it into every track of the album (journalled, undoable)
    bool saveFile = false;   // save cover.jpg beside the tracks when there is no picture file yet
    bool search = true;      // search MusicBrainz by album and artist when no track has an MBID
    bool dryRun = false;
    QString artCacheDir;     // empty: ~/.cache/thrum/art
    QString sandbox;         // tests: never write outside this folder
};

struct AlbumResult {
    qint64 albumId = 0;
    QString album, artist;
    QString status; // fetched, embedded, not-found, no-match, offline, failed, would-fetch
    QString releaseId, releaseGroupId, how; // how: mbid, release-group, search
    QString picture;    // the original saved locally
    QString coverFile;  // cover.jpg written, if any
    QString batch;      // the tag journal batch when embedded
    QString error;
    QJsonObject toJson() const;
};

AlbumResult fetchForAlbum(Library &library, Net::HttpClient &http, qint64 albumId, const AlbumOptions &options,
                          Job *job = nullptr);

// Albums whose tracks the "missing artwork" audit lists.
QList<qint64> albumsMissingArt(Library &library);

}
