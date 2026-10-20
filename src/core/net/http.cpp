#include "http.h"

#include "job.h"
#include "library.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <atomic>

namespace Net {

namespace {

// Keep in step with src/app/main.cpp. The command line and the window set
// the application version; tests and libraries that don't fall back to this.
const char *fallbackVersion = "0.3.0";

constexpr int serviceCount = 9;

int index(Service s) { return int(s); }

// The published limits: MusicBrainz and ListenBrainz one request a second,
// AcoustID three, LRCLIB asks only for politeness (two), the Cover Art
// Archive has no published limit (two), Last.fm five.
int defaultInterval(Service s) {
    switch (s) {
    case Service::MusicBrainz: return 1000;
    case Service::AcoustId: return 334;
    case Service::CoverArt: return 500;
    case Service::Lrclib: return 500;
    case Service::ListenBrainz: return 1000;
    case Service::LastFm: return 200;
    // Wikimedia asks for a User-Agent and serial requests; five a second is
    // well inside that.
    case Service::Wikidata: return 200;
    case Service::Wikipedia: return 200;
    case Service::Other: return 0;
    }
    return 1000;
}

std::atomic_int intervals[serviceCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
std::atomic_int maxRetriesValue = -1;
std::atomic_int backoffValue = -1;

QMutex limiterMutex;
qint64 nextAllowed[serviceCount] = {};

qint64 nowMs() {
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

int envInt(const char *name, int fallback) {
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok ? v : fallback;
}

bool isLoopback(const QUrl &url) {
    const QString host = url.host();
    if (host == "localhost")
        return true;
    QHostAddress a;
    return a.setAddress(host) && a.isLoopback();
}

// The Retry-After header in milliseconds (seconds or an HTTP date), or -1.
int retryAfterMs(const QByteArray &value) {
    if (value.isEmpty())
        return -1;
    bool ok = false;
    const int seconds = value.trimmed().toInt(&ok);
    if (ok)
        return qMax(0, seconds) * 1000;
    const QDateTime when = QDateTime::fromString(QString::fromLatin1(value).trimmed(), Qt::RFC2822Date);
    if (when.isValid())
        return int(qBound(qint64(0), QDateTime::currentDateTimeUtc().msecsTo(when), qint64(600000)));
    return -1;
}

bool sleepFor(int ms, const Job *job) {
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        if (job && job->isCancelled())
            return false;
        QThread::msleep(qMin<qint64>(50, qMax<qint64>(1, deadline.remainingTime())));
    }
    return !(job && job->isCancelled());
}

} // namespace

QString serviceName(Service service) {
    switch (service) {
    case Service::MusicBrainz: return "musicbrainz";
    case Service::AcoustId: return "acoustid";
    case Service::CoverArt: return "coverart";
    case Service::Lrclib: return "lrclib";
    case Service::ListenBrainz: return "listenbrainz";
    case Service::LastFm: return "lastfm";
    case Service::Wikidata: return "wikidata";
    case Service::Wikipedia: return "wikipedia";
    case Service::Other: return "other";
    }
    return "other";
}

QString version() {
    const QString v = QCoreApplication::instance() ? QCoreApplication::applicationVersion() : QString();
    return v.isEmpty() ? QString::fromLatin1(fallbackVersion) : v;
}

QString userAgent() { return QString("Omamp/%1 ( https://github.com/matt-shearing/omamp )").arg(version()); }

QString defaultBaseUrl(Service service) {
    switch (service) {
    case Service::MusicBrainz: return "https://musicbrainz.org/ws/2";
    case Service::AcoustId: return "https://api.acoustid.org/v2";
    case Service::CoverArt: return "https://coverartarchive.org";
    case Service::Lrclib: return "https://lrclib.net/api";
    case Service::ListenBrainz: return "https://api.listenbrainz.org/1";
    case Service::LastFm: return "https://ws.audioscrobbler.com/2.0";
    case Service::Wikidata: return "https://www.wikidata.org";
    case Service::Wikipedia: return "https://{lang}.wikipedia.org";
    case Service::Other: return {};
    }
    return {};
}

QString baseUrl(Service service, const Library *library) {
    const QByteArray env = "OMAMP_" + serviceName(service).toUpper().toLatin1() + "_URL";
    QString url = qEnvironmentVariable(env.constData());
    if (url.isEmpty() && library && library->isOpen())
        url = library->setting("net.url." + serviceName(service)).toString();
    if (url.isEmpty())
        url = defaultBaseUrl(service);
    while (url.endsWith('/'))
        url.chop(1);
    return url;
}

QUrl makeUrl(const QString &base, const QString &path, const QList<QPair<QString, QString>> &query) {
    QUrl url(base + path);
    if (!query.isEmpty()) {
        QUrlQuery q;
        for (const auto &item : query)
            q.addQueryItem(QString::fromLatin1(QUrl::toPercentEncoding(item.first)),
                           QString::fromLatin1(QUrl::toPercentEncoding(item.second)));
        url.setQuery(q.query(QUrl::FullyEncoded), QUrl::StrictMode);
    }
    return url;
}

// --- Config ---------------------------------------------------------------

int Config::intervalMs(Service service) {
    const int set = intervals[index(service)].load();
    if (set >= 0)
        return set;
    const QByteArray env = "OMAMP_RATE_" + serviceName(service).toUpper().toLatin1() + "_MS";
    return envInt(env.constData(), defaultInterval(service));
}

void Config::setIntervalMs(Service service, int ms) { intervals[index(service)] = ms; }

int Config::maxRetries() {
    const int set = maxRetriesValue.load();
    return set >= 0 ? set : envInt("OMAMP_NET_RETRIES", 3);
}

void Config::setMaxRetries(int retries) { maxRetriesValue = retries; }

int Config::backoffMs() {
    const int set = backoffValue.load();
    return set >= 0 ? set : envInt("OMAMP_NET_BACKOFF_MS", 1000);
}

void Config::setBackoffMs(int ms) { backoffValue = ms; }

bool Config::forcedOffline(const Library *library) {
    if (qEnvironmentVariable("OMAMP_OFFLINE") == "1")
        return true;
    return library && library->isOpen() &&
           QStringList{"1", "true", "yes"}.contains(library->setting("net.offline", "0").toString().toLower());
}

void Config::resetForTests() {
    for (auto &i : intervals)
        i = -1;
    maxRetriesValue = -1;
    backoffValue = -1;
    RateLimiter::reset();
}

// --- RateLimiter ----------------------------------------------------------

bool RateLimiter::acquire(Service service, const Job *job) {
    const int interval = Config::intervalMs(service);
    qint64 wait = 0;
    {
        QMutexLocker lock(&limiterMutex);
        const qint64 now = nowMs();
        qint64 &next = nextAllowed[index(service)];
        wait = qMax<qint64>(0, next - now);
        // Reserve the slot now, so two threads never take the same one.
        next = qMax(now, next) + interval;
    }
    return wait <= 0 ? !(job && job->isCancelled()) : sleepFor(int(wait), job);
}

void RateLimiter::noteHeaders(Service service, const QMap<QByteArray, QByteArray> &headers) {
    bool ok = false;
    const int remaining = headers.value("x-ratelimit-remaining").toInt(&ok);
    if (!ok || remaining > 0)
        return;
    bool resetOk = false;
    const double resetIn = headers.value("x-ratelimit-reset-in").toDouble(&resetOk);
    if (resetOk)
        holdFor(service, int(qBound(0.0, resetIn, 600.0) * 1000));
}

void RateLimiter::holdFor(Service service, int ms) {
    QMutexLocker lock(&limiterMutex);
    qint64 &next = nextAllowed[index(service)];
    next = qMax(next, nowMs() + ms);
}

void RateLimiter::reset() {
    QMutexLocker lock(&limiterMutex);
    for (qint64 &n : nextAllowed)
        n = 0;
}

// --- HttpCache ------------------------------------------------------------

HttpCache::HttpCache(const QString &dir) : m_dir(dir.isEmpty() ? defaultDir() : dir) {}

QString HttpCache::defaultDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omamp/http";
}

QByteArray HttpCache::keyFor(const Request &request) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(request.method);
    hash.addData("\n");
    hash.addData(request.url.toEncoded());
    hash.addData("\n");
    hash.addData(request.body);
    return hash.result().toHex();
}

QString HttpCache::pathFor(const QByteArray &key) const {
    return QString("%1/%2/%3.bin").arg(m_dir, QString::fromLatin1(key.left(2)), QString::fromLatin1(key));
}

bool HttpCache::get(const QByteArray &key, Response *response, bool allowStale) const {
    QFile file(pathFor(key));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QDataStream in(&file);
    quint32 magic = 0;
    qint32 status = 0;
    qint64 expires = 0;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    in >> magic >> status >> expires >> headers >> body;
    if (in.status() != QDataStream::Ok || magic != 0x4f4d4831)
        return false;
    if (!allowStale && expires < QDateTime::currentSecsSinceEpoch())
        return false;
    response->status = status;
    response->headers = headers;
    response->body = body;
    response->fromCache = true;
    response->error.clear();
    return true;
}

void HttpCache::put(const QByteArray &key, const Response &response, int seconds) const {
    const QString path = pathFor(key);
    QDir().mkpath(path.left(path.lastIndexOf('/')));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QDataStream out(&file);
    // Only the headers a later reader could need.
    QMap<QByteArray, QByteArray> keep;
    for (const QByteArray &name : {QByteArray("content-type"), QByteArray("etag"), QByteArray("last-modified")})
        if (response.headers.contains(name))
            keep.insert(name, response.headers.value(name));
    out << quint32(0x4f4d4831) << qint32(response.status) << qint64(QDateTime::currentSecsSinceEpoch() + seconds)
        << keep << response.body;
    file.commit();
}

int HttpCache::clear() const {
    int removed = 0;
    QDirIterator it(m_dir, {"*.bin"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        removed += QFile::remove(it.next()) ? 1 : 0;
    return removed;
}

// --- HttpClient -----------------------------------------------------------

HttpClient::HttpClient(const Library *library, const QString &cacheDir) : m_library(library), m_cache(cacheDir) {}

HttpClient::~HttpClient() = default;

Response HttpClient::get(Service service, const QUrl &url, int cacheSeconds) {
    Request r;
    r.service = service;
    r.url = url;
    r.cacheSeconds = cacheSeconds;
    return send(r);
}

Response HttpClient::send(const Request &request) {
    const bool cacheable = m_cacheEnabled && request.cacheSeconds > 0;
    const QByteArray key = cacheable ? HttpCache::keyFor(request) : QByteArray();
    Response cached;
    if (cacheable && !request.refresh && m_cache.get(key, &cached))
        return cached;

    if (Config::forcedOffline(m_library)) {
        // A stale copy is better than nothing when the user is offline.
        if (cacheable && m_cache.get(key, &cached, true))
            return cached;
        Response r;
        r.offline = true;
        r.error = "Offline: the network is turned off for Omamp (net.offline or OMAMP_OFFLINE)";
        return r;
    }

    // Ask the system first, so a laptop on a train fails at once instead of
    // after every retry. A loopback address (the tests' mock server) is
    // always reachable.
    if (!isLoopback(request.url)) {
        static std::atomic_bool loaded = false;
        if (!loaded.exchange(true) && QCoreApplication::instance())
            QNetworkInformation::loadDefaultBackend();
        const QNetworkInformation *info = QNetworkInformation::instance();
        if (info && info->reachability() == QNetworkInformation::Reachability::Disconnected) {
            if (cacheable && m_cache.get(key, &cached, true))
                return cached;
            Response r;
            r.offline = true;
            r.error = "Offline: the system reports no network";
            return r;
        }
    }

    Response r;
    const int retries = request.retry ? Config::maxRetries() : 0;
    int backoff = Config::backoffMs();
    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (!RateLimiter::acquire(request.service, m_job)) {
            r = Response();
            r.error = "Cancelled";
            break;
        }
        r = sendOnce(request);
        r.attempts = attempt + 1;
        ++m_sent;
        RateLimiter::noteHeaders(request.service, r.headers);
        const bool busy = r.status == 503 || r.status == 429 || r.status == 502 || r.status == 504;
        const bool timedOut = r.status == 0 && !r.offline && r.error.startsWith("Timed out");
        if (!(busy || timedOut) || attempt == retries)
            break;
        int wait = retryAfterMs(r.header("retry-after"));
        if (wait < 0)
            wait = backoff;
        backoff *= 2;
        RateLimiter::holdFor(request.service, qMin(wait, 120000));
    }

    if (r.offline && cacheable && m_cache.get(key, &cached, true))
        return cached;
    if (cacheable && (r.ok() || r.status == 404))
        m_cache.put(key, r, r.status == 404 ? qMin(request.cacheSeconds, 86400) : request.cacheSeconds);
    return r;
}

Response HttpClient::sendOnce(const Request &request) {
    if (!m_manager)
        m_manager = std::make_unique<QNetworkAccessManager>();
    QNetworkRequest nr(request.url);
    nr.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    nr.setRawHeader("Accept", "application/json");
    nr.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    nr.setTransferTimeout(request.timeoutMs);
    for (const auto &h : request.headers)
        nr.setRawHeader(h.first, h.second);
    if (!request.contentType.isEmpty())
        nr.setHeader(QNetworkRequest::ContentTypeHeader, request.contentType);

    QNetworkReply *reply = nullptr;
    if (request.method == "GET")
        reply = m_manager->get(nr);
    else if (request.method == "POST")
        reply = m_manager->post(nr, request.body);
    else
        reply = m_manager->sendCustomRequest(nr, request.method, request.body);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelPoll;
    if (m_job) {
        QObject::connect(&cancelPoll, &QTimer::timeout, reply, [this, reply] {
            if (m_job->isCancelled())
                reply->abort();
        });
        cancelPoll.start(100);
    }
    if (!reply->isFinished())
        loop.exec(QEventLoop::ExcludeUserInputEvents);

    Response r;
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body = reply->readAll();
    for (const auto &pair : reply->rawHeaderPairs())
        r.headers.insert(pair.first.toLower(), pair.second);
    const QNetworkReply::NetworkError e = reply->error();
    if (r.status == 0) {
        switch (e) {
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::UnknownNetworkError:
            r.offline = true;
            r.error = QString("Offline: can't reach %1 (%2)").arg(request.url.host(), reply->errorString());
            break;
        case QNetworkReply::OperationCanceledError:
            r.error = m_job && m_job->isCancelled() ? QString("Cancelled") : QString("Timed out: %1").arg(reply->errorString());
            break;
        case QNetworkReply::TimeoutError:
            r.error = QString("Timed out: %1").arg(reply->errorString());
            break;
        default:
            r.error = reply->errorString();
        }
    } else if (r.status < 200 || r.status >= 300) {
        r.error = QString("HTTP %1 from %2").arg(r.status).arg(request.url.host());
    }
    cancelPoll.stop();
    // Deleted here, not with deleteLater: a worker thread may have no event
    // loop to run it.
    delete reply;
    return r;
}

}
