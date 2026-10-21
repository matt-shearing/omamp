#include "subsonic.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>

QJsonObject SubsonicServer::toJson() const {
    return {{"api_version", apiVersion}, {"type", type}, {"server_version", serverVersion}, {"open_subsonic", openSubsonic}};
}

SubsonicClient::SubsonicClient(const QUrl &server, const QString &user, const QString &secret, Auth auth)
    : m_server(server), m_user(user), m_secret(secret), m_auth(auth) {
    // https://nas:4533/ and https://nas:4533 both mean the server root; a
    // reverse proxy may add a path prefix (https://nas/navidrome).
    QString path = m_server.path();
    while (path.endsWith('/'))
        path.chop(1);
    m_server.setPath(path);
}

SubsonicClient::~SubsonicClient() = default;

QString SubsonicClient::token(const QString &password, const QString &salt) {
    return QString::fromLatin1(QCryptographicHash::hash((password + salt).toUtf8(), QCryptographicHash::Md5).toHex());
}

QUrlQuery SubsonicClient::authQuery() const {
    QUrlQuery q;
    if (m_auth == Auth::ApiKey) {
        q.addQueryItem("apiKey", m_secret);
    } else {
        // A fresh salt per request, at least six characters as the API asks.
        const quint64 a = QRandomGenerator::system()->generate64();
        const QString salt = QString::number(a, 36).rightJustified(12, '0').right(12);
        q.addQueryItem("u", m_user);
        q.addQueryItem("t", token(m_secret, salt));
        q.addQueryItem("s", salt);
    }
    q.addQueryItem("v", apiVersion);
    q.addQueryItem("c", clientName);
    q.addQueryItem("f", "json");
    return q;
}

QUrl SubsonicClient::signedUrl(const QString &method, const QUrlQuery &params) const {
    QUrl url = m_server;
    url.setPath(m_server.path() + "/rest/" + method);
    QUrlQuery q = authQuery();
    for (const auto &item : params.queryItems(QUrl::FullyDecoded))
        q.addQueryItem(item.first, item.second);
    url.setQuery(q);
    return url;
}

QByteArray SubsonicClient::get(const QUrl &url, QString *contentType, int *status, QString *error) {
    if (!m_network)
        m_network = std::make_unique<QNetworkAccessManager>();
    ++m_requests;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("Thrum/%1").arg(QCoreApplication::applicationVersion().isEmpty()
                                                   ? QString("dev")
                                                   : QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(m_timeoutMs);
    QNetworkReply *reply = m_network->get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(m_timeoutMs + 1000);
    if (!reply->isFinished())
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    QByteArray body;
    if (!reply->isFinished()) {
        reply->abort();
        if (error)
            *error = QString("%1 did not answer within %2 s").arg(m_server.host()).arg(m_timeoutMs / 1000);
    } else {
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (contentType)
            *contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        body = reply->readAll();
        // Never reply->errorString(): it quotes the URL, which carries the
        // token and salt (or the API key).
        if (reply->error() != QNetworkReply::NoError && error) {
            const QString host = m_server.host();
            switch (reply->error()) {
            case QNetworkReply::ConnectionRefusedError: *error = QString("%1 refused the connection").arg(host); break;
            case QNetworkReply::HostNotFoundError: *error = QString("%1 was not found").arg(host); break;
            case QNetworkReply::TimeoutError:
            case QNetworkReply::OperationCanceledError: *error = QString("%1 did not answer in time").arg(host); break;
            case QNetworkReply::RemoteHostClosedError: *error = QString("%1 closed the connection").arg(host); break;
            case QNetworkReply::SslHandshakeFailedError:
                *error = QString("%1's certificate was not accepted").arg(host);
                break;
            default:
                *error = *status ? QString("%1 answered HTTP %2").arg(host).arg(*status)
                                 : QString("%1 could not be reached (network error %2)").arg(host).arg(int(reply->error()));
            }
        }
    }
    reply->deleteLater();
    return body;
}

SubsonicClient::Reply SubsonicClient::call(const QString &method, const QUrlQuery &params) {
    Reply r;
    QString error;
    const QByteArray body = get(signedUrl(method, params), nullptr, &r.httpStatus, &error);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QJsonObject response = doc.object().value("subsonic-response").toObject();
    if (response.isEmpty()) {
        r.error = !error.isEmpty() ? error
                                   : QString("%1 did not answer like a Subsonic server (HTTP %2)")
                                         .arg(m_server.toString(QUrl::RemoveUserInfo))
                                         .arg(r.httpStatus);
        return r;
    }
    r.body = response;
    if (response.value("status").toString() != "ok") {
        const QJsonObject e = response.value("error").toObject();
        r.code = e.value("code").toInt();
        r.error = e.value("message").toString();
        if (r.error.isEmpty())
            r.error = QString("The server refused %1 (error %2)").arg(method).arg(r.code);
        if (r.code == 41)
            r.error += ". This server can't check tokens (common with LDAP sign-in); use an API key instead "
                       "(thrum remote add ... --api-key-stdin)";
        return r;
    }
    r.ok = true;
    return r;
}

QByteArray SubsonicClient::callBytes(const QString &method, const QUrlQuery &params, QString *contentType,
                                     QString *error) {
    int status = 0;
    QString why, type;
    QByteArray body = get(signedUrl(method, params), &type, &status, &why);
    if (contentType)
        *contentType = type;
    // Errors come back as a JSON (or XML) document instead of bytes.
    if (type.contains("json") || type.contains("xml") || status >= 400 || !why.isEmpty()) {
        const QJsonObject response = QJsonDocument::fromJson(body).object().value("subsonic-response").toObject();
        if (error)
            *error = !response.isEmpty() ? response.value("error").toObject().value("message").toString()
                                         : (why.isEmpty() ? QString("HTTP %1").arg(status) : why);
        if (error && error->isEmpty())
            *error = QString("%1 failed").arg(method);
        return {};
    }
    return body;
}

bool SubsonicClient::ping(SubsonicServer *server, QString *error) {
    const Reply r = call("ping");
    if (!r.ok) {
        if (error)
            *error = r.error;
        return false;
    }
    if (server) {
        server->apiVersion = r.body.value("version").toString();
        server->type = r.body.value("type").toString();
        server->serverVersion = r.body.value("serverVersion").toString();
        server->openSubsonic = r.body.value("openSubsonic").toBool();
    }
    return true;
}

static QJsonArray arrayOf(const QJsonObject &o, const char *key) {
    const QJsonValue v = o.value(key);
    // Some older servers send a single object where a list has one entry.
    if (v.isObject())
        return QJsonArray{v};
    return v.toArray();
}

QJsonArray SubsonicClient::musicFolders(QString *error) {
    const Reply r = call("getMusicFolders");
    if (!r.ok) {
        if (error)
            *error = r.error;
        return {};
    }
    return arrayOf(r.body.value("musicFolders").toObject(), "musicFolder");
}

QJsonArray SubsonicClient::artists(QString *error, const QString &musicFolderId) {
    QUrlQuery q;
    if (!musicFolderId.isEmpty())
        q.addQueryItem("musicFolderId", musicFolderId);
    const Reply r = call("getArtists", q);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return {};
    }
    QJsonArray out;
    for (const QJsonValue &index : arrayOf(r.body.value("artists").toObject(), "index"))
        for (const QJsonValue &artist : arrayOf(index.toObject(), "artist"))
            out.append(artist);
    return out;
}

QJsonArray SubsonicClient::albumList2(const QString &type, int size, int offset, QString *error,
                                      const QString &musicFolderId) {
    QUrlQuery q;
    q.addQueryItem("type", type);
    q.addQueryItem("size", QString::number(qBound(1, size, 500)));
    q.addQueryItem("offset", QString::number(offset));
    if (!musicFolderId.isEmpty())
        q.addQueryItem("musicFolderId", musicFolderId);
    const Reply r = call("getAlbumList2", q);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return {};
    }
    return arrayOf(r.body.value("albumList2").toObject(), "album");
}

QJsonObject SubsonicClient::album(const QString &id, QString *error) {
    QUrlQuery q;
    q.addQueryItem("id", id);
    const Reply r = call("getAlbum", q);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return {};
    }
    QJsonObject album = r.body.value("album").toObject();
    album["song"] = arrayOf(album, "song");
    return album;
}

QJsonObject SubsonicClient::search3(const QString &query, int artistCount, int albumCount, int songCount,
                                    QString *error, int songOffset) {
    QUrlQuery q;
    q.addQueryItem("query", query);
    q.addQueryItem("artistCount", QString::number(artistCount));
    q.addQueryItem("albumCount", QString::number(albumCount));
    q.addQueryItem("songCount", QString::number(songCount));
    q.addQueryItem("songOffset", QString::number(songOffset));
    const Reply r = call("search3", q);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return {};
    }
    const QJsonObject result = r.body.value("searchResult3").toObject();
    return {{"artist", arrayOf(result, "artist")}, {"album", arrayOf(result, "album")}, {"song", arrayOf(result, "song")}};
}

QByteArray SubsonicClient::coverArt(const QString &id, int size, QString *contentType, QString *error) {
    QUrlQuery q;
    q.addQueryItem("id", id);
    if (size > 0)
        q.addQueryItem("size", QString::number(size));
    return callBytes("getCoverArt", q, contentType, error);
}

QUrl SubsonicClient::streamUrl(const QString &songId, int maxBitRate, const QString &format) const {
    QUrlQuery q;
    q.addQueryItem("id", songId);
    if (maxBitRate > 0)
        q.addQueryItem("maxBitRate", QString::number(maxBitRate));
    if (!format.isEmpty())
        q.addQueryItem("format", format);
    return signedUrl("stream", q);
}

bool SubsonicClient::scrobble(const QString &songId, bool submission, qint64 timeMs, QString *error) {
    QUrlQuery q;
    q.addQueryItem("id", songId);
    q.addQueryItem("submission", submission ? "true" : "false");
    if (timeMs > 0)
        q.addQueryItem("time", QString::number(timeMs));
    const Reply r = call("scrobble", q);
    if (!r.ok && error)
        *error = r.error;
    return r.ok;
}

bool SubsonicClient::setRating(const QString &id, int stars, QString *error) {
    QUrlQuery q;
    q.addQueryItem("id", id);
    q.addQueryItem("rating", QString::number(qBound(0, stars, 5)));
    const Reply r = call("setRating", q);
    if (!r.ok && error)
        *error = r.error;
    return r.ok;
}
