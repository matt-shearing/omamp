#pragma once

#include "http.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// A small MusicBrainz web service (ws/2, JSON) client: release and
// recording lookups by MBID and searches by tags, parsed into plain structs.
// One request a second, the User-Agent the service asks for, and answers
// cached for a week (the data is CC0 and changes slowly).
namespace MusicBrainz {

struct Credit {
    QString artistId, name, sortName, joinPhrase;
};

// "Name A feat. Name B", as the release credits it.
QString creditText(const QList<Credit> &credits);
// The credited artists' names, for Thrum's multi-value artist field.
QStringList creditNames(const QList<Credit> &credits);
QStringList creditIds(const QList<Credit> &credits);
QList<Credit> parseCredits(const QJsonArray &array);

struct Track {
    QString id;          // the release track
    QString recordingId; // what Picard writes as MUSICBRAINZ_TRACKID
    QString title, number;
    int position = 0;
    qint64 lengthMs = 0;
    QList<Credit> credits;
    QStringList isrcs;
    QJsonObject toJson() const;
};

struct Medium {
    int position = 0;
    QString format, title;
    int trackCount = 0;
    QList<Track> tracks;
};

struct Label {
    QString id, name, catalogNumber;
};

struct Release {
    QString id, title, status, date, country, barcode, disambiguation;
    QList<Credit> credits;
    QString releaseGroupId, releaseGroupTitle, primaryType, firstReleaseDate;
    QStringList secondaryTypes;
    QList<Label> labels;
    QList<Medium> media;
    QStringList genres; // most votes first
    int searchScore = 0; // 0 to 100, from a search
    bool complete = false; // true when media hold their tracks (a lookup, not a search)

    int trackCount() const;
    int year() const;
    QString formats() const; // "CD", "2×CD", "Digital Media"
    QJsonObject toJson(bool withTracks = false) const;
    static Release fromJson(const QJsonObject &o);
};

struct ReleaseRef {
    QString id, title, date, country, status, releaseGroupId;
    int trackCount = 0;
};

struct Recording {
    QString id, title;
    qint64 lengthMs = 0;
    QList<Credit> credits;
    QStringList isrcs;
    QList<ReleaseRef> releases;
    int searchScore = 0;
    static Recording fromJson(const QJsonObject &o);
};

// Lucene query text: a phrase with its special characters escaped.
QString quoted(const QString &text);

class Client {
public:
    explicit Client(Net::HttpClient &http);

    // The full release: media, tracks, recordings, ISRCs, labels, release
    // group and genres. Empty id on failure, with error set.
    Release release(const QString &mbid, QString *error = nullptr);
    Recording recording(const QString &mbid, QString *error = nullptr);
    // Releases whose title and artist match, best first (no tracks; look
    // the chosen ones up with release()). trackCount 0 leaves it out.
    QList<Release> searchReleases(const QString &artist, const QString &album, int trackCount = 0, int limit = 10,
                                  QString *error = nullptr);
    QList<Recording> searchRecordings(const QString &artist, const QString &title, int limit = 10,
                                      QString *error = nullptr);
    // A release group's releases (ids only), for cover art fallbacks.
    QList<Release> releasesInGroup(const QString &releaseGroupId, QString *error = nullptr);

    bool lastWasOffline() const { return m_offline; }
    static constexpr int cacheSeconds = 7 * 86400;

private:
    QJsonObject fetch(const QString &path, const QList<QPair<QString, QString>> &query, QString *error);

    Net::HttpClient &m_http;
    bool m_offline = false;
};

}
