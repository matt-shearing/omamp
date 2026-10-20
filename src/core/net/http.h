#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QUrl>
#include <memory>

class Job;
class Library;
class QNetworkAccessManager;

// The one way Omamp talks to the web.
//
// Every request names the service it goes to, so each service gets its own
// polite rate (MusicBrainz one a second, AcoustID three, LRCLIB two,
// ListenBrainz whatever its X-RateLimit headers say), shared by every thread
// in the process. Every request carries Omamp's User-Agent. 503 and 429 are
// retried with backoff (honouring Retry-After), and a lost network is
// reported as offline rather than retried forever. GET answers can be kept
// in a small file cache under ~/.cache/omamp/http for a time the caller
// chooses, so asking twice costs nothing and an offline lookup can still
// answer from what was fetched before.
//
// HttpClient is blocking: send() runs a local event loop until the answer
// arrives. Use one client per thread (the window's workers each make their
// own); the rate limits and the cache are shared.
namespace Net {

enum class Service { MusicBrainz, AcoustId, CoverArt, Lrclib, ListenBrainz, LastFm, Wikidata, Wikipedia, Other };

// "musicbrainz", "acoustid", "coverart", "lrclib", "listenbrainz", "lastfm",
// "wikidata", "wikipedia".
QString serviceName(Service service);

// "Omamp/0.3.0 ( https://github.com/matt-shearing/omamp )", as MusicBrainz
// and ListenBrainz ask.
QString userAgent();
QString version();

// Where a service lives. The environment (OMAMP_MUSICBRAINZ_URL,
// OMAMP_ACOUSTID_URL, OMAMP_COVERART_URL, OMAMP_LRCLIB_URL,
// OMAMP_LISTENBRAINZ_URL, OMAMP_LASTFM_URL, OMAMP_WIKIDATA_URL,
// OMAMP_WIKIPEDIA_URL) wins, then the library setting net.url.<service>,
// then the public address. No trailing slash. Wikipedia's address holds
// "{lang}", which the caller replaces with the language's code.
QString baseUrl(Service service, const Library *library = nullptr);
QString defaultBaseUrl(Service service);

struct Request {
    Service service = Service::Other;
    QByteArray method = "GET";
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    QByteArray contentType; // for POST bodies
    // Seconds to keep a 200 (or 404) answer in the cache; 0 never caches.
    // POSTs are cached only when this is set (AcoustID lookups are POSTs).
    int cacheSeconds = 0;
    // Skip the cache's copy and ask again (the fresh answer is still cached).
    bool refresh = false;
    bool retry = true;
    int timeoutMs = 30000;
};

struct Response {
    int status = 0; // 0 when no answer came
    QByteArray body;
    QMap<QByteArray, QByteArray> headers; // names in lower case
    QString error;                        // empty on success
    bool fromCache = false;
    bool offline = false; // no network, or the host could not be reached
    int attempts = 0;
    bool ok() const { return status >= 200 && status < 300 && error.isEmpty(); }
    bool notFound() const { return status == 404; }
    QJsonDocument json() const { return QJsonDocument::fromJson(body); }
    QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

// Tuning shared by every client. Tests make the waits short; the defaults
// are the services' own published limits.
struct Config {
    static int intervalMs(Service service);
    static void setIntervalMs(Service service, int ms); // -1 restores the default
    static int maxRetries();
    static void setMaxRetries(int retries);
    static int backoffMs(); // the first wait after a 503 or 429; doubles each time
    static void setBackoffMs(int ms);
    // True when the user turned the network off for Omamp: OMAMP_OFFLINE=1
    // or the library setting net.offline = 1.
    static bool forcedOffline(const Library *library = nullptr);
    static void resetForTests();
};

// The per-service pace, shared by all threads.
class RateLimiter {
public:
    // Waits until a request to service may go. False when the job was
    // cancelled while waiting.
    static bool acquire(Service service, const Job *job = nullptr);
    // Reads X-RateLimit-Remaining and X-RateLimit-Reset-In (ListenBrainz,
    // and MusicBrainz in places) and holds the next request back when the
    // allowance is spent.
    static void noteHeaders(Service service, const QMap<QByteArray, QByteArray> &headers);
    // Holds the service back for ms (after a 503 or a Retry-After).
    static void holdFor(Service service, int ms);
    static void reset();
};

// Answers kept on disk, one file per request, written atomically.
class HttpCache {
public:
    // Empty means ~/.cache/omamp/http (following XDG_CACHE_HOME).
    explicit HttpCache(const QString &dir = {});
    static QString defaultDir();
    QString dir() const { return m_dir; }

    static QByteArray keyFor(const Request &request);
    // A copy that has not expired; allowStale also returns expired ones (for
    // answering offline).
    bool get(const QByteArray &key, Response *response, bool allowStale = false) const;
    void put(const QByteArray &key, const Response &response, int seconds) const;
    int clear() const;

private:
    QString pathFor(const QByteArray &key) const;
    QString m_dir;
};

class HttpClient {
public:
    // library supplies settings (net.offline, net.url.*) and may be null.
    explicit HttpClient(const Library *library = nullptr, const QString &cacheDir = {});
    ~HttpClient();
    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    Response send(const Request &request);
    Response get(Service service, const QUrl &url, int cacheSeconds = 0);

    // Cancel waits and transfers when the job is cancelled.
    void setJob(const Job *job) { m_job = job; }
    void setCacheEnabled(bool on) { m_cacheEnabled = on; }
    const Library *library() const { return m_library; }
    HttpCache &cache() { return m_cache; }

    // How many requests actually went out (not answered from the cache).
    int requestsSent() const { return m_sent; }

private:
    Response sendOnce(const Request &request);

    const Library *m_library;
    HttpCache m_cache;
    std::unique_ptr<QNetworkAccessManager> m_manager;
    const Job *m_job = nullptr;
    bool m_cacheEnabled = true;
    int m_sent = 0;
};

// Builds a URL from a base and a path with query items, percent-encoding
// each value once.
QUrl makeUrl(const QString &base, const QString &path, const QList<QPair<QString, QString>> &query = {});

}
