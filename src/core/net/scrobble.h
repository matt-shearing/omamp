#pragma once

#include "http.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class Library;

// Scrobbling to ListenBrainz and Last.fm.
//
// The player tells a Scrobbler what it plays; listens that count go into
// the scrobble_queue table at once, one row per service, and flush() sends
// them later, in batches, retrying until the service accepts them. Plays
// made offline are therefore never lost. Only flush() and the log-in helpers
// use the network; trackStarted(), trackProgress() and trackFinished() only
// touch the database, so the player can call them on its own thread.
//
// A listen counts once the track has played for half its length or four
// minutes, whichever comes first, and only for tracks of 30 seconds or more
// (the rule both services publish).
namespace Scrobble {

bool qualifies(qint64 playedMs, qint64 durationMs);

struct Listen {
    qint64 trackId = 0;
    QString artist, title, album, albumArtist;
    qint64 durationMs = 0;
    int trackNumber = 0;
    QString recordingMbid, releaseMbid;
    QStringList artistMbids;
    qint64 listenedAt = 0; // Unix seconds, when it started playing

    static Listen fromTrack(Library &library, qint64 trackId);
    QJsonObject toJson() const; // what the queue's payload column holds
    static Listen fromJson(const QJsonObject &o);
    // ListenBrainz's track_metadata object.
    QJsonObject listenBrainzMetadata() const;
};

struct ServiceStatus {
    QString service;
    bool configured = false; // a token or session key is set
    bool enabled = false;
    QString user;
    int pending = 0, sent = 0, failed = 0;
    QString lastError;
    QJsonObject toJson() const;
};

struct FlushResult {
    int sent = 0, failed = 0, remaining = 0;
    bool offline = false;
    bool nowPlayingSent = false;
    QStringList errors;
    QJsonObject toJson() const;
};

namespace ListenBrainz {
// The user's token, from OMAMP_LISTENBRAINZ_TOKEN or the setting
// listenbrainz.token.
QString token(const Library &library);
// Asks ListenBrainz whether a token is valid; on success user is its name.
bool validateToken(Net::HttpClient &http, const QString &token, QString *user, QString *error);
// listenType: "single", "import" or "playing_now".
Net::Response submit(Net::HttpClient &http, const QString &token, const QList<Listen> &listens,
                     const QString &listenType);
}

namespace LastFm {
QString apiKey(const Library &library);
QString apiSecret(const Library &library);
QString sessionKey(const Library &library);
// md5 of the parameters sorted by name, each name then value, then the
// secret: Last.fm's api_sig. "format" and "callback" are left out.
QString signature(const QMap<QString, QString> &params, const QString &secret);
// Step 1 of desktop authentication: a request token.
bool getToken(Net::HttpClient &http, const QString &apiKey, const QString &secret, QString *token, QString *error);
// Step 2: the page where the user grants access.
QString authUrl(const QString &apiKey, const QString &token);
// Step 3, after the user has granted access: the session key.
bool getSession(Net::HttpClient &http, const QString &apiKey, const QString &secret, const QString &token,
                QString *sessionKey, QString *user, QString *error);
Net::Response call(Net::HttpClient &http, QMap<QString, QString> params, const QString &secret, bool post);
}

class Scrobbler {
public:
    explicit Scrobbler(Library &library, Net::HttpClient *http = nullptr);

    // The player's hooks. at is when the track started (Unix seconds); 0
    // means now. playedMs is the time actually heard, seeks not counted, as
    // Stats::recordPlay takes it.
    void trackStarted(qint64 trackId, qint64 at = 0);
    void trackProgress(qint64 trackId, qint64 playedMs);
    // Queues the listen for every enabled service when it counts. Returns
    // how many queue rows were added (0 when it didn't count).
    int trackFinished(qint64 trackId, qint64 playedMs);

    // Sends the pending now-playing notice and up to maxListens queued
    // listens per service. Needs an HttpClient.
    FlushResult flush(int maxListens = 500);

    // Services with credentials whose setting scrobble.<service> is not 0,
    // and scrobble.enabled not 0.
    QStringList enabledServices() const;
    QList<ServiceStatus> status() const;

    // The queue directly, for tests and the command line.
    int enqueue(const QString &service, const Listen &listen);

private:
    FlushResult flushListenBrainz(int maxListens);
    FlushResult flushLastFm(int maxListens);
    void sendNowPlaying(FlushResult *result);

    Library &m_library;
    Net::HttpClient *m_http;
    qint64 m_currentTrack = 0, m_startedAt = 0, m_playedMs = 0;
    bool m_nowPlayingPending = false;
};

}
