#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <memory>

class QNetworkAccessManager;

// What ping said about the server.
struct SubsonicServer {
    QString apiVersion;     // "1.16.1"
    QString type;           // "navidrome", "gonic", ... (OpenSubsonic servers say)
    QString serverVersion;
    bool openSubsonic = false;
    QJsonObject toJson() const;
};

// A client for the Subsonic API and its OpenSubsonic extensions, as served
// by Navidrome, gonic, Airsonic-Advanced, LMS and others. JSON only (f=json).
//
// Two ways to sign in:
// - Token: every request carries u (the user), s (a fresh random salt) and
//   t = md5(password + salt). The password itself never crosses the wire,
//   but it must be kept to sign requests, and the server must keep it too.
// - API key (OpenSubsonic apiKeyAuthentication): every request carries
//   apiKey and no user. Navidrome and others issue these per client; this
//   is the better choice where the server offers it.
// The legacy p=<password> (or p=enc:<hex>) form is never sent.
//
// Calls are synchronous: each runs a local event loop until the reply or
// the timeout (default 20 s). Use the client from one thread; a sync runs
// on a worker thread with its own client.
class SubsonicClient {
public:
    enum class Auth { Token, ApiKey };

    SubsonicClient(const QUrl &server, const QString &user, const QString &secret, Auth auth);
    ~SubsonicClient();
    SubsonicClient(const SubsonicClient &) = delete;
    SubsonicClient &operator=(const SubsonicClient &) = delete;

    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    static constexpr const char *clientName = "thrum";
    static constexpr const char *apiVersion = "1.16.1";

    struct Reply {
        bool ok = false;
        QJsonObject body;  // the "subsonic-response" object
        QString error;     // a sentence; the server's own message when it gave one
        int code = 0;      // the Subsonic error code (40 wrong credentials, 41 token auth not supported, 70 not found, ...)
        int httpStatus = 0;
    };

    // Any method, with the auth and format parameters added.
    Reply call(const QString &method, const QUrlQuery &params = {});
    // A method that answers with bytes (getCoverArt, stream, download).
    QByteArray callBytes(const QString &method, const QUrlQuery &params, QString *contentType, QString *error);
    // The signed URL for a method, for something else to fetch (the player
    // streaming a song). Each call has a fresh salt.
    QUrl signedUrl(const QString &method, const QUrlQuery &params = {}) const;
    // How many HTTP requests this client has made.
    int requests() const { return m_requests; }

    bool ping(SubsonicServer *server, QString *error);
    QJsonArray musicFolders(QString *error);
    // Every artist, flattened out of the A-Z index.
    QJsonArray artists(QString *error, const QString &musicFolderId = {});
    // One page of albums: type is alphabeticalByName, newest, recent,
    // frequent, random, starred, ...; size at most 500.
    QJsonArray albumList2(const QString &type, int size, int offset, QString *error,
                          const QString &musicFolderId = {});
    // An album with its songs ("song" array).
    QJsonObject album(const QString &id, QString *error);
    // {artist: [...], album: [...], song: [...]}
    QJsonObject search3(const QString &query, int artistCount, int albumCount, int songCount, QString *error,
                        int songOffset = 0);
    QByteArray coverArt(const QString &id, int size, QString *contentType, QString *error);
    // maxBitRate 0 means the original; format empty means the server's
    // default ("raw" asks for the original file without transcoding).
    QUrl streamUrl(const QString &songId, int maxBitRate = 0, const QString &format = {}) const;
    // submission false is "now playing"; timeMs is when it was played.
    bool scrobble(const QString &songId, bool submission, qint64 timeMs, QString *error);
    // 0 clears, 1 to 5 stars.
    bool setRating(const QString &id, int stars, QString *error);

    // md5(password + salt) as lower-case hex, the token auth's t parameter.
    static QString token(const QString &password, const QString &salt);

private:
    QUrlQuery authQuery() const;
    QByteArray get(const QUrl &url, QString *contentType, int *status, QString *error);

    QUrl m_server;
    QString m_user, m_secret;
    Auth m_auth;
    int m_timeoutMs = 20000;
    int m_requests = 0;
    std::unique_ptr<QNetworkAccessManager> m_network;
};
