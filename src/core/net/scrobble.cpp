#include "scrobble.h"

#include "library.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrlQuery>

namespace Scrobble {

namespace {

constexpr qint64 minimumTrackMs = 30000;
constexpr qint64 fourMinutesMs = 240000;
constexpr int listenBrainzBatch = 100; // well under the service's 1000
constexpr int lastFmBatch = 50;        // the service's maximum
constexpr int keepSentDays = 30;

bool settingOn(const Library &library, const QString &key, bool fallback) {
    const QString v = library.setting(key, fallback ? "1" : "0").toString().toLower();
    return v == "1" || v == "true" || v == "yes";
}

QString envOrSetting(const Library &library, const char *env, const QString &key) {
    const QString e = qEnvironmentVariable(env).trimmed();
    return e.isEmpty() ? library.setting(key).toString().trimmed() : e;
}

struct Row {
    qint64 id = 0;
    Listen listen;
};

QList<Row> pendingRows(Library &library, const QString &service, int limit) {
    QList<Row> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT id, payload, listened_at FROM scrobble_queue WHERE service = ? AND state = 'pending' "
              "ORDER BY listened_at, id LIMIT ?");
    q.addBindValue(service);
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            Row r;
            r.id = q.value(0).toLongLong();
            r.listen = Listen::fromJson(QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object());
            r.listen.listenedAt = q.value(2).toLongLong();
            out << r;
        }
    }
    return out;
}

void markSent(Library &library, const QList<Row> &rows) {
    QSqlDatabase db = library.db();
    db.transaction();
    QSqlQuery q(db);
    q.prepare("UPDATE scrobble_queue SET state = 'sent', sent = ?, attempts = attempts + 1, last_error = NULL "
              "WHERE id = ?");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const Row &r : rows) {
        q.addBindValue(now);
        q.addBindValue(r.id);
        q.exec();
    }
    db.commit();
}

// A temporary failure keeps the rows pending; a permanent one marks them
// failed, so one bad listen can't block the queue forever.
void markFailed(Library &library, const QList<Row> &rows, const QString &error, bool permanent) {
    QSqlDatabase db = library.db();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QString("UPDATE scrobble_queue SET attempts = attempts + 1, last_error = ?%1 WHERE id = ?")
                  .arg(permanent ? ", state = 'failed'" : ""));
    for (const Row &r : rows) {
        q.addBindValue(error);
        q.addBindValue(r.id);
        q.exec();
    }
    db.commit();
}

int pendingCount(Library &library, const QString &service) {
    QSqlQuery q(library.db());
    q.prepare("SELECT count(*) FROM scrobble_queue WHERE service = ? AND state = 'pending'");
    q.addBindValue(service);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

} // namespace

bool qualifies(qint64 playedMs, qint64 durationMs) {
    if (durationMs > 0 && durationMs < minimumTrackMs)
        return false;
    if (durationMs <= 0) // unknown length: four minutes heard counts
        return playedMs >= fourMinutesMs;
    return playedMs >= qMin(durationMs / 2, fourMinutesMs);
}

// --- Listen ---------------------------------------------------------------

Listen Listen::fromTrack(Library &library, qint64 trackId) {
    Listen l;
    l.trackId = trackId;
    QSqlQuery q(library.db());
    q.prepare("SELECT artist, title, album, album_artist, duration_ms, track_no, mb_track_id, mb_release_id, "
              "mb_artist_id FROM tracks WHERE id = ?");
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        // Thrum keeps several artists as "A; B"; the services want one credit.
        l.artist = q.value(0).toString().replace("; ", ", ");
        l.title = q.value(1).toString();
        l.album = q.value(2).toString();
        l.albumArtist = q.value(3).toString().replace("; ", ", ");
        l.durationMs = q.value(4).toLongLong();
        l.trackNumber = q.value(5).toInt();
        l.recordingMbid = q.value(6).toString();
        l.releaseMbid = q.value(7).toString();
        for (const QString &id : q.value(8).toString().split(';', Qt::SkipEmptyParts))
            l.artistMbids << id.trimmed();
    }
    return l;
}

QJsonObject Listen::toJson() const {
    QJsonObject o{{"artist", artist}, {"title", title}};
    if (trackId)
        o["track_id"] = trackId;
    if (!album.isEmpty())
        o["album"] = album;
    if (!albumArtist.isEmpty())
        o["album_artist"] = albumArtist;
    if (durationMs)
        o["duration_ms"] = durationMs;
    if (trackNumber)
        o["track_number"] = trackNumber;
    if (!recordingMbid.isEmpty())
        o["recording_mbid"] = recordingMbid;
    if (!releaseMbid.isEmpty())
        o["release_mbid"] = releaseMbid;
    if (!artistMbids.isEmpty())
        o["artist_mbids"] = QJsonArray::fromStringList(artistMbids);
    return o;
}

Listen Listen::fromJson(const QJsonObject &o) {
    Listen l;
    l.trackId = qint64(o.value("track_id").toDouble());
    l.artist = o.value("artist").toString();
    l.title = o.value("title").toString();
    l.album = o.value("album").toString();
    l.albumArtist = o.value("album_artist").toString();
    l.durationMs = qint64(o.value("duration_ms").toDouble());
    l.trackNumber = o.value("track_number").toInt();
    l.recordingMbid = o.value("recording_mbid").toString();
    l.releaseMbid = o.value("release_mbid").toString();
    for (const QJsonValue &v : o.value("artist_mbids").toArray())
        l.artistMbids << v.toString();
    return l;
}

QJsonObject Listen::listenBrainzMetadata() const {
    QJsonObject info{{"media_player", "Thrum"},
                     {"submission_client", "Thrum"},
                     {"submission_client_version", Net::version()}};
    if (durationMs)
        info["duration_ms"] = durationMs;
    if (trackNumber)
        info["tracknumber"] = trackNumber;
    if (!recordingMbid.isEmpty())
        info["recording_mbid"] = recordingMbid;
    if (!releaseMbid.isEmpty())
        info["release_mbid"] = releaseMbid;
    if (!artistMbids.isEmpty())
        info["artist_mbids"] = QJsonArray::fromStringList(artistMbids);
    QJsonObject meta{{"artist_name", artist}, {"track_name", title}, {"additional_info", info}};
    if (!album.isEmpty())
        meta["release_name"] = album;
    return meta;
}

QJsonObject ServiceStatus::toJson() const {
    QJsonObject o{{"service", service}, {"configured", configured}, {"enabled", enabled},
                  {"pending", pending}, {"sent", sent},             {"failed", failed}};
    if (!user.isEmpty())
        o["user"] = user;
    if (!lastError.isEmpty())
        o["last_error"] = lastError;
    return o;
}

QJsonObject FlushResult::toJson() const {
    return {{"sent", sent},
            {"failed", failed},
            {"remaining", remaining},
            {"offline", offline},
            {"now_playing_sent", nowPlayingSent},
            {"errors", QJsonArray::fromStringList(errors)}};
}

// --- ListenBrainz ---------------------------------------------------------

namespace ListenBrainz {

QString token(const Library &library) { return envOrSetting(library, "THRUM_LISTENBRAINZ_TOKEN", "listenbrainz.token"); }

bool validateToken(Net::HttpClient &http, const QString &token, QString *user, QString *error) {
    Net::Request r;
    r.service = Net::Service::ListenBrainz;
    r.url = QUrl(Net::baseUrl(Net::Service::ListenBrainz, http.library()) + "/validate-token");
    r.headers.append(QPair<QByteArray, QByteArray>("Authorization", "Token " + token.toUtf8()));
    const Net::Response response = http.send(r);
    const QJsonObject o = response.json().object();
    if (response.ok() && o.value("valid").toBool()) {
        if (user)
            *user = o.value("user_name").toString();
        return true;
    }
    if (error)
        *error = response.offline ? response.error
                 : o.contains("message") ? QString("ListenBrainz: %1").arg(o.value("message").toString())
                                         : QString("ListenBrainz: %1").arg(response.error);
    return false;
}

Net::Response submit(Net::HttpClient &http, const QString &token, const QList<Listen> &listens,
                     const QString &listenType) {
    QJsonArray payload;
    for (const Listen &l : listens) {
        QJsonObject item{{"track_metadata", l.listenBrainzMetadata()}};
        if (listenType != "playing_now")
            item["listened_at"] = l.listenedAt;
        payload << item;
    }
    Net::Request r;
    r.service = Net::Service::ListenBrainz;
    r.method = "POST";
    r.url = QUrl(Net::baseUrl(Net::Service::ListenBrainz, http.library()) + "/submit-listens");
    r.contentType = "application/json";
    r.headers.append(QPair<QByteArray, QByteArray>("Authorization", "Token " + token.toUtf8()));
    r.body = QJsonDocument(QJsonObject{{"listen_type", listenType}, {"payload", payload}}).toJson(QJsonDocument::Compact);
    return http.send(r);
}

}

// --- Last.fm --------------------------------------------------------------

namespace LastFm {

QString apiKey(const Library &library) { return envOrSetting(library, "THRUM_LASTFM_API_KEY", "lastfm.api_key"); }
QString apiSecret(const Library &library) {
    return envOrSetting(library, "THRUM_LASTFM_API_SECRET", "lastfm.api_secret");
}
QString sessionKey(const Library &library) { return library.setting("lastfm.session_key").toString().trimmed(); }

QString signature(const QMap<QString, QString> &params, const QString &secret) {
    QByteArray text;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (it.key() == "format" || it.key() == "callback")
            continue;
        text += it.key().toUtf8() + it.value().toUtf8();
    }
    text += secret.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Md5).toHex());
}

Net::Response call(Net::HttpClient &http, QMap<QString, QString> params, const QString &secret, bool post) {
    if (!secret.isEmpty())
        params["api_sig"] = signature(params, secret);
    params["format"] = "json";
    QUrlQuery q;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        q.addQueryItem(QString::fromLatin1(QUrl::toPercentEncoding(it.key())),
                       QString::fromLatin1(QUrl::toPercentEncoding(it.value())));
    const QString base = Net::baseUrl(Net::Service::LastFm, http.library()) + '/';
    Net::Request r;
    r.service = Net::Service::LastFm;
    if (post) {
        r.method = "POST";
        r.url = QUrl(base);
        r.contentType = "application/x-www-form-urlencoded";
        r.body = q.query(QUrl::FullyEncoded).toUtf8();
    } else {
        QUrl url(base);
        url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);
        r.url = url;
    }
    return http.send(r);
}

namespace {
QString lastFmError(const Net::Response &r) {
    const QJsonObject o = r.json().object();
    if (o.contains("error"))
        return QString("Last.fm: %1 (error %2)").arg(o.value("message").toString()).arg(o.value("error").toInt());
    return r.error.isEmpty() ? QString() : QString("Last.fm: %1").arg(r.error);
}
}

bool getToken(Net::HttpClient &http, const QString &apiKey, const QString &secret, QString *token, QString *error) {
    const Net::Response r = call(http, {{"method", "auth.getToken"}, {"api_key", apiKey}}, secret, false);
    const QString t = r.json().object().value("token").toString();
    if (r.ok() && !t.isEmpty()) {
        *token = t;
        return true;
    }
    if (error)
        *error = lastFmError(r);
    return false;
}

QString authUrl(const QString &apiKey, const QString &token) {
    QUrl url("https://www.last.fm/api/auth/");
    QUrlQuery q;
    q.addQueryItem("api_key", apiKey);
    q.addQueryItem("token", token);
    url.setQuery(q);
    return url.toString();
}

bool getSession(Net::HttpClient &http, const QString &apiKey, const QString &secret, const QString &token,
                QString *sessionKey, QString *user, QString *error) {
    const Net::Response r =
        call(http, {{"method", "auth.getSession"}, {"api_key", apiKey}, {"token", token}}, secret, false);
    const QJsonObject session = r.json().object().value("session").toObject();
    if (r.ok() && !session.value("key").toString().isEmpty()) {
        *sessionKey = session.value("key").toString();
        if (user)
            *user = session.value("name").toString();
        return true;
    }
    if (error)
        *error = lastFmError(r);
    return false;
}

}

// --- Scrobbler ------------------------------------------------------------

Scrobbler::Scrobbler(Library &library, Net::HttpClient *http) : m_library(library), m_http(http) {}

QStringList Scrobbler::enabledServices() const {
    QStringList out;
    if (!settingOn(m_library, "scrobble.enabled", true))
        return out;
    if (!ListenBrainz::token(m_library).isEmpty() && settingOn(m_library, "scrobble.listenbrainz", true))
        out << "listenbrainz";
    if (!LastFm::sessionKey(m_library).isEmpty() && !LastFm::apiKey(m_library).isEmpty() &&
        !LastFm::apiSecret(m_library).isEmpty() && settingOn(m_library, "scrobble.lastfm", true))
        out << "lastfm";
    return out;
}

void Scrobbler::trackStarted(qint64 trackId, qint64 at) {
    m_currentTrack = trackId;
    m_startedAt = at > 0 ? at : QDateTime::currentSecsSinceEpoch();
    m_playedMs = 0;
    m_nowPlayingPending = trackId > 0 && !enabledServices().isEmpty();
}

void Scrobbler::trackProgress(qint64 trackId, qint64 playedMs) {
    if (trackId == m_currentTrack)
        m_playedMs = qMax(m_playedMs, playedMs);
}

int Scrobbler::trackFinished(qint64 trackId, qint64 playedMs) {
    if (trackId == m_currentTrack)
        playedMs = qMax(playedMs, m_playedMs);
    const qint64 startedAt = trackId == m_currentTrack && m_startedAt ? m_startedAt
                                                                      : QDateTime::currentSecsSinceEpoch() - playedMs / 1000;
    if (trackId == m_currentTrack) {
        m_currentTrack = 0;
        m_nowPlayingPending = false;
    }
    const QStringList services = enabledServices();
    if (services.isEmpty())
        return 0;
    Listen listen = Listen::fromTrack(m_library, trackId);
    if (listen.title.isEmpty() || listen.artist.isEmpty())
        return 0; // the services refuse a listen without both
    if (!qualifies(playedMs, listen.durationMs))
        return 0;
    listen.listenedAt = startedAt;
    int added = 0;
    for (const QString &s : services)
        added += enqueue(s, listen);
    return added;
}

int Scrobbler::enqueue(const QString &service, const Listen &listen) {
    QSqlQuery q(m_library.db());
    q.prepare("INSERT INTO scrobble_queue (service, track_id, listened_at, duration_ms, payload, created) "
              "VALUES (?, ?, ?, ?, ?, ?)");
    q.addBindValue(service);
    q.addBindValue(listen.trackId ? QVariant(listen.trackId) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(listen.listenedAt);
    q.addBindValue(listen.durationMs);
    q.addBindValue(QString::fromUtf8(QJsonDocument(listen.toJson()).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    return q.exec() ? 1 : 0;
}

void Scrobbler::sendNowPlaying(FlushResult *result) {
    if (!m_nowPlayingPending || !m_currentTrack)
        return;
    m_nowPlayingPending = false;
    const Listen listen = Listen::fromTrack(m_library, m_currentTrack);
    if (listen.title.isEmpty() || listen.artist.isEmpty())
        return;
    const QStringList services = enabledServices();
    // A now-playing notice is never queued: it means nothing later.
    if (services.contains("listenbrainz")) {
        const Net::Response r = ListenBrainz::submit(*m_http, ListenBrainz::token(m_library), {listen}, "playing_now");
        result->nowPlayingSent = result->nowPlayingSent || r.ok();
        result->offline = result->offline || r.offline;
    }
    if (services.contains("lastfm")) {
        QMap<QString, QString> p{{"method", "track.updateNowPlaying"},
                                 {"api_key", LastFm::apiKey(m_library)},
                                 {"sk", LastFm::sessionKey(m_library)},
                                 {"artist", listen.artist},
                                 {"track", listen.title}};
        if (!listen.album.isEmpty())
            p["album"] = listen.album;
        if (listen.durationMs)
            p["duration"] = QString::number(listen.durationMs / 1000);
        const Net::Response r = LastFm::call(*m_http, p, LastFm::apiSecret(m_library), true);
        result->nowPlayingSent = result->nowPlayingSent || (r.ok() && !r.json().object().contains("error"));
        result->offline = result->offline || r.offline;
    }
}

FlushResult Scrobbler::flush(int maxListens) {
    FlushResult total;
    if (!m_http) {
        total.errors << "No network client";
        return total;
    }
    sendNowPlaying(&total);
    const QStringList services = enabledServices();
    for (const QString &s : services) {
        const FlushResult r = s == "listenbrainz" ? flushListenBrainz(maxListens) : flushLastFm(maxListens);
        total.sent += r.sent;
        total.failed += r.failed;
        total.offline = total.offline || r.offline;
        total.errors += r.errors;
    }
    for (const QString &s : {QString("listenbrainz"), QString("lastfm")})
        total.remaining += pendingCount(m_library, s);
    // Sent rows are kept a while for `thrum scrobble status`, then dropped.
    QSqlQuery prune(m_library.db());
    prune.prepare("DELETE FROM scrobble_queue WHERE state = 'sent' AND sent < ?");
    prune.addBindValue(QDateTime::currentSecsSinceEpoch() - qint64(keepSentDays) * 86400);
    prune.exec();
    return total;
}

FlushResult Scrobbler::flushListenBrainz(int maxListens) {
    FlushResult result;
    const QString token = ListenBrainz::token(m_library);
    int budget = maxListens;
    while (budget > 0) {
        const QList<Row> rows = pendingRows(m_library, "listenbrainz", qMin(budget, listenBrainzBatch));
        if (rows.isEmpty())
            break;
        budget -= rows.size();
        QList<Listen> listens;
        for (const Row &r : rows)
            listens << r.listen;
        const Net::Response response =
            ListenBrainz::submit(*m_http, token, listens, rows.size() == 1 ? "single" : "import");
        if (response.ok()) {
            markSent(m_library, rows);
            result.sent += rows.size();
            continue;
        }
        const QString message = response.json().object().value("error").toString();
        const QString error = QString("ListenBrainz: %1").arg(message.isEmpty() ? response.error : message);
        if (response.status == 400 && rows.size() > 1) {
            // One bad listen spoils the batch: send them one by one so only
            // the bad one is set aside.
            for (const Row &r : rows) {
                const Net::Response single = ListenBrainz::submit(*m_http, token, {r.listen}, "single");
                if (single.ok()) {
                    markSent(m_library, {r});
                    ++result.sent;
                } else {
                    const bool permanent = single.status == 400;
                    markFailed(m_library, {r}, QString("ListenBrainz: %1").arg(single.error), permanent);
                    result.failed += permanent ? 1 : 0;
                }
            }
            continue;
        }
        const bool permanent = response.status == 400;
        markFailed(m_library, rows, error, permanent);
        result.failed += permanent ? rows.size() : 0;
        result.offline = response.offline;
        result.errors << (response.status == 401 ? QString("ListenBrainz refused the token; log in again with "
                                                           "`thrum scrobble login listenbrainz --token <token>`")
                                                 : error);
        if (!permanent)
            break; // offline, busy or unauthorised: try again later
    }
    return result;
}

FlushResult Scrobbler::flushLastFm(int maxListens) {
    FlushResult result;
    const QString key = LastFm::apiKey(m_library), secret = LastFm::apiSecret(m_library),
                  session = LastFm::sessionKey(m_library);
    int budget = maxListens;
    while (budget > 0) {
        const QList<Row> rows = pendingRows(m_library, "lastfm", qMin(budget, lastFmBatch));
        if (rows.isEmpty())
            break;
        budget -= rows.size();
        QMap<QString, QString> p{{"method", "track.scrobble"}, {"api_key", key}, {"sk", session}};
        for (int i = 0; i < rows.size(); ++i) {
            const Listen &l = rows[i].listen;
            const QString n = QString("[%1]").arg(i);
            p["artist" + n] = l.artist;
            p["track" + n] = l.title;
            p["timestamp" + n] = QString::number(l.listenedAt);
            if (!l.album.isEmpty())
                p["album" + n] = l.album;
            if (!l.albumArtist.isEmpty())
                p["albumArtist" + n] = l.albumArtist;
            if (l.durationMs)
                p["duration" + n] = QString::number(l.durationMs / 1000);
            if (l.trackNumber)
                p["trackNumber" + n] = QString::number(l.trackNumber);
            if (!l.recordingMbid.isEmpty())
                p["mbid" + n] = l.recordingMbid;
        }
        const Net::Response response = LastFm::call(*m_http, p, secret, true);
        const QJsonObject o = response.json().object();
        if (response.ok() && !o.contains("error")) {
            markSent(m_library, rows);
            result.sent += rows.size();
            continue;
        }
        const int code = o.value("error").toInt();
        const QString error = o.contains("error")
                                  ? QString("Last.fm: %1 (error %2)").arg(o.value("message").toString()).arg(code)
                                  : QString("Last.fm: %1").arg(response.error);
        // 9: the session is no longer valid; 11, 16: the service is down;
        // 29: too fast. Those wait; anything else is the listens' fault.
        const bool temporary = response.offline || response.status == 0 || response.status >= 500 ||
                               QList<int>{9, 11, 16, 29}.contains(code);
        markFailed(m_library, rows, error, !temporary);
        result.failed += temporary ? 0 : rows.size();
        result.offline = response.offline;
        result.errors << (code == 9 ? QString("Last.fm refused the session; log in again with "
                                              "`thrum scrobble login lastfm`")
                                    : error);
        if (temporary)
            break;
    }
    return result;
}

QList<ServiceStatus> Scrobbler::status() const {
    QList<ServiceStatus> out;
    const QStringList enabled = enabledServices();
    for (const QString &service : {QString("listenbrainz"), QString("lastfm")}) {
        ServiceStatus s;
        s.service = service;
        s.configured = service == "listenbrainz" ? !ListenBrainz::token(m_library).isEmpty()
                                                 : !LastFm::sessionKey(m_library).isEmpty();
        s.enabled = enabled.contains(service);
        s.user = m_library.setting(service + ".user").toString();
        QSqlQuery q(m_library.db());
        q.prepare("SELECT state, count(*) FROM scrobble_queue WHERE service = ? GROUP BY state");
        q.addBindValue(service);
        if (q.exec()) {
            while (q.next()) {
                const QString state = q.value(0).toString();
                const int n = q.value(1).toInt();
                if (state == "pending")
                    s.pending = n;
                else if (state == "sent")
                    s.sent = n;
                else
                    s.failed = n;
            }
        }
        QSqlQuery e(m_library.db());
        e.prepare("SELECT last_error FROM scrobble_queue WHERE service = ? AND last_error IS NOT NULL "
                  "ORDER BY id DESC LIMIT 1");
        e.addBindValue(service);
        if (e.exec() && e.next())
            s.lastError = e.value(0).toString();
        out << s;
    }
    return out;
}

}
