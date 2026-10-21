#pragma once

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class Library;
class QSqlQuery;
namespace Net {
class HttpClient;
}

// Internet radio: the stations Thrum knows, the Nightride FM catalogue and
// its live now-playing feed, ICY titles inside a stream, radio-browser.info
// search, and .pls/.m3u station lists. docs/radio.md describes it for users.
//
// A station plays from the queue as radio://<key>. The key never changes:
// "nightride/darksynth" for a built-in channel, "rb/<uuid>" for a station
// found on radio-browser.info, "my/<n>" for an address the user added. The
// stream URL is looked up when the station plays, so a channel whose
// address changes keeps working from an old queue.
namespace Radio {

struct Station {
    qint64 id = 0;
    QString key, name;
    // nightride, rekt, radio-browser or custom.
    QString network;
    // Best first: the player falls back along the list when one fails.
    QStringList streams;
    QString homepage, artUrl, artPath, color, description, tags, country, codec;
    int bitrate = 0;
    // Where the song's name comes from: nightride (the feed), icy (the
    // stream's own titles) or none.
    QString metaKind = QStringLiteral("icy");
    // The station's id in the Nightride feed.
    QString metaKey;
    bool builtin = false, favourite = false;
    int position = 0;
    qint64 lastPlayed = 0, listenedMs = 0;

    bool isValid() const { return !key.isEmpty(); }
    QString path() const { return QStringLiteral("radio://") + key; }
    // "Nightride FM", "Rekt Network", "radio-browser.info", "My stations".
    QString networkName() const;
    // The palette's and the CLI's long name: "Nightride FM: Darksynth".
    QString fullName() const;
    // nightride, rekt or mine: the tree group the station shows under.
    QString group() const;
    QJsonObject toJson() const;
    QVariantMap toMap() const;
};

// What a Nightride channel is playing, from the feed.
struct FeedRow {
    QString station, artist, title, trackId;
    qint64 at = 0;
    QString line() const;
};

bool isRadioPath(const QString &path);
QString keyFromPath(const QString &path);
QString networkName(const QString &network);

// nightride.fm, or THRUM_NIGHTRIDE_URL (tests). Its feed is <base>/meta.
QString nightrideBase();
QString nightrideFeedUrl();
// stream.nightride.fm, or THRUM_NIGHTRIDE_STREAM_URL (tests).
QString nightrideStreamBase();

// Every Nightride FM and Rekt Network channel, as seeded. Checked against
// nightride.fm on 20 October 2026: the site lists the seven Nightride FM
// channels; the feed and the stream server also carry the four Rekt
// Network ones. Each streams MP3 at 320 kbps and Opus in Ogg.
QList<Station> builtinStations();

// The Nightride feed is server-sent events: "data: [{...}]" lines, a
// keepalive now and then. Feed bytes as they arrive; complete events come
// back as their data payloads.
class SseParser {
public:
    QList<QByteArray> feed(const QByteArray &bytes);
    void reset() { m_buffer.clear(); m_data.clear(); }

private:
    QByteArray m_buffer, m_data;
};
// One data payload: an array (or a single object) of station rows. Rows
// marked live (a DJ on air) carry no track and are left out, as the site
// does.
QList<FeedRow> parseFeedData(const QByteArray &data);

// The channels nightride.fm's page lists (<a class="station"
// data-station="..." data-type="stream">), with their names and genres.
struct CatalogEntry {
    QString id, name, description;
};
QList<CatalogEntry> parseNightrideHomepage(const QString &html);

// ICY metadata: "StreamTitle='Artist - Title';StreamUrl='';".
QString parseStreamTitle(const QByteArray &block);
// "Artist - Title" into its parts; a title without " - " has no artist.
void splitStreamTitle(const QString &streamTitle, QString *artist, QString *title);

// Takes a stream's bytes with ICY metadata every metaInt bytes and returns
// each new StreamTitle as it passes. The audio is dropped.
class IcyParser {
public:
    explicit IcyParser(int metaInt = 0) : m_metaInt(metaInt), m_left(metaInt) {}
    void setMetaInt(int metaInt) { m_metaInt = metaInt; m_left = metaInt; m_meta.clear(); m_metaLeft = -1; }
    int metaInt() const { return m_metaInt; }
    QStringList feed(const QByteArray &bytes);

private:
    int m_metaInt = 0, m_left = 0, m_metaLeft = -1;
    QByteArray m_meta;
    QString m_last;
};

// A square picture for a station without one: its colour, its name.
// Needs a QGuiApplication (fonts).
QImage tile(const Station &station, int size = 600);

// The stations table (migration 10). Use on the thread that opened the
// library.
class Stations {
public:
    explicit Stations(Library &library);

    // Adds the built-in channels and brings their names, addresses and
    // colours up to date, keeping favourites and listening time. Cheap when
    // nothing changed.
    bool seed(QString *error = nullptr);

    QList<Station> all() const;
    // nightride, rekt, favourites, mine, recent or all, in the picker's order.
    QList<Station> group(const QString &group) const;
    Station byKey(const QString &key) const;
    Station byId(qint64 id) const;
    // A key, "radio://key", a Nightride id ("darksynth"), a name, or
    // "Network: Name", ignoring case; else the only station whose name
    // contains the text.
    Station find(const QString &text, QStringList *ambiguous = nullptr) const;
    // The station with this stream address, if any (so a search result or
    // an imported list doesn't add Nightride a second time).
    Station byStreamUrl(const QString &url) const;

    // Adds a station from radio-browser.info or a custom address. A custom
    // station gets the key my/<n>. Returns the stored station; an address
    // Thrum already has returns that station instead.
    Station add(Station station, QString *error = nullptr, bool *existed = nullptr);
    bool remove(const QString &key, QString *error = nullptr);
    bool rename(const QString &key, const QString &name);
    bool setFavourite(const QString &key, bool on);
    bool setArtPath(const QString &key, const QString &path);
    void notePlayed(const QString &key, qint64 at = 0);
    void addListened(const QString &key, qint64 ms);

    // Songs heard on the radio. started is Unix seconds; fromStart says the
    // song began while tuned in, so the whole of it could be heard.
    qint64 noteHeard(const QString &key, const QString &artist, const QString &title, bool fromStart,
                     qint64 started = 0);
    void setHeardMs(qint64 heardId, qint64 ms);
    // Newest first: {station, stationName, artist, title, started, heardMs}.
    QList<QVariantMap> recentlyHeard(int limit = 50) const;

    // Adds channels nightride.fm lists, and ones its feed names, that Thrum
    // doesn't have yet: on the site, Nightride FM; only in the feed, Rekt
    // Network. Nothing is removed. Returns how many were added.
    int mergeNightrideCatalog(const QList<CatalogEntry> &site, const QStringList &feedIds);

    // .pls, .m3u or .m3u8 station lists. Each http(s) entry becomes a
    // custom station (or is matched to one Thrum has).
    int importFile(const QString &file, QString *error = nullptr);
    // group as for group(); the format from the extension (.pls, else M3U).
    bool exportFile(const QString &file, const QString &group, QString *error = nullptr) const;

private:
    Station read(const QSqlQuery &q) const;
    Library &m_library;
};

// radio-browser.info, the open directory of internet radio.
namespace Browser {
// THRUM_RADIOBROWSER_URL (tests), else one of the API servers named by the
// DNS record _api._tcp.radio-browser.info (or its /json/servers list),
// picked at random as the service asks. No trailing slash.
QString baseUrl(Net::HttpClient *http = nullptr);
struct Result {
    QList<Station> stations;
    QString error;
    bool ok() const { return error.isEmpty(); }
};
// Stations whose name matches, most-listened first, broken ones left out.
Result search(Net::HttpClient &http, const QString &base, const QString &query, int limit = 50);
Station fromJson(const QJsonObject &o);
// Tells the directory a station was played, as it asks of clients.
void countClick(Net::HttpClient &http, const QString &base, const QString &uuid);
} // namespace Browser

} // namespace Radio
