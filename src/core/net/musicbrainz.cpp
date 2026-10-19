#include "musicbrainz.h"

#include <QRegularExpression>

namespace MusicBrainz {

QList<Credit> parseCredits(const QJsonArray &array) {
    QList<Credit> out;
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        const QJsonObject artist = o.value("artist").toObject();
        Credit c;
        c.artistId = artist.value("id").toString();
        c.name = o.value("name").toString(artist.value("name").toString());
        c.sortName = artist.value("sort-name").toString();
        c.joinPhrase = o.value("joinphrase").toString();
        out << c;
    }
    return out;
}

QString creditText(const QList<Credit> &credits) {
    QString text;
    for (const Credit &c : credits)
        text += c.name + c.joinPhrase;
    return text.trimmed();
}

QStringList creditNames(const QList<Credit> &credits) {
    QStringList out;
    for (const Credit &c : credits)
        if (!c.name.isEmpty() && !out.contains(c.name))
            out << c.name;
    return out;
}

QStringList creditIds(const QList<Credit> &credits) {
    QStringList out;
    for (const Credit &c : credits)
        if (!c.artistId.isEmpty() && !out.contains(c.artistId))
            out << c.artistId;
    return out;
}

QJsonObject Track::toJson() const {
    return {{"id", id},
            {"recording_id", recordingId},
            {"title", title},
            {"number", number},
            {"position", position},
            {"length_ms", lengthMs},
            {"artist", creditText(credits)},
            {"isrcs", QJsonArray::fromStringList(isrcs)}};
}

int Release::trackCount() const {
    int n = 0;
    for (const Medium &m : media)
        n += m.trackCount ? m.trackCount : int(m.tracks.size());
    return n;
}

int Release::year() const { return date.left(4).toInt(); }

QString Release::formats() const {
    QStringList parts;
    QMap<QString, int> counts;
    QStringList order;
    for (const Medium &m : media) {
        const QString f = m.format.isEmpty() ? QString("(unknown)") : m.format;
        if (!counts.contains(f))
            order << f;
        ++counts[f];
    }
    for (const QString &f : std::as_const(order))
        parts << (counts[f] > 1 ? QString("%1×%2").arg(counts[f]).arg(f) : f);
    return parts.join(" + ");
}

QJsonObject Release::toJson(bool withTracks) const {
    QJsonArray labelList;
    for (const Label &l : labels)
        labelList << QJsonObject{{"name", l.name}, {"catalog_number", l.catalogNumber}};
    QJsonObject o{{"id", id},
                  {"title", title},
                  {"artist", creditText(credits)},
                  {"date", date},
                  {"country", country},
                  {"status", status},
                  {"barcode", barcode},
                  {"disambiguation", disambiguation},
                  {"release_group_id", releaseGroupId},
                  {"primary_type", primaryType},
                  {"first_release_date", firstReleaseDate},
                  {"formats", formats()},
                  {"track_count", trackCount()},
                  {"media_count", int(media.size())},
                  {"labels", labelList},
                  {"genres", QJsonArray::fromStringList(genres)}};
    if (searchScore)
        o["search_score"] = searchScore;
    if (withTracks) {
        QJsonArray mediaList;
        for (const Medium &m : media) {
            QJsonArray tracks;
            for (const Track &t : m.tracks)
                tracks << t.toJson();
            mediaList << QJsonObject{{"position", m.position}, {"format", m.format}, {"title", m.title},
                                     {"tracks", tracks}};
        }
        o["media"] = mediaList;
    }
    return o;
}

Release Release::fromJson(const QJsonObject &o) {
    Release r;
    r.id = o.value("id").toString();
    r.title = o.value("title").toString();
    r.status = o.value("status").toString();
    r.date = o.value("date").toString();
    r.country = o.value("country").toString();
    r.barcode = o.value("barcode").toString();
    r.disambiguation = o.value("disambiguation").toString();
    r.searchScore = o.value("score").toInt();
    r.credits = parseCredits(o.value("artist-credit").toArray());
    const QJsonObject group = o.value("release-group").toObject();
    r.releaseGroupId = group.value("id").toString();
    r.releaseGroupTitle = group.value("title").toString();
    r.primaryType = group.value("primary-type").toString();
    r.firstReleaseDate = group.value("first-release-date").toString();
    for (const QJsonValue &v : group.value("secondary-types").toArray())
        r.secondaryTypes << v.toString();
    for (const QJsonValue &v : o.value("label-info").toArray()) {
        const QJsonObject li = v.toObject();
        const QJsonObject label = li.value("label").toObject();
        r.labels << Label{label.value("id").toString(), label.value("name").toString(),
                          li.value("catalog-number").toString()};
    }
    bool anyTracks = false;
    for (const QJsonValue &mv : o.value("media").toArray()) {
        const QJsonObject mo = mv.toObject();
        Medium m;
        m.position = mo.value("position").toInt();
        m.format = mo.value("format").toString();
        m.title = mo.value("title").toString();
        m.trackCount = mo.value("track-count").toInt();
        // A lookup says "tracks"; a recording search's release says "track".
        QJsonArray tracks = mo.value("tracks").toArray();
        if (tracks.isEmpty())
            tracks = mo.value("track").toArray();
        for (const QJsonValue &tv : std::as_const(tracks)) {
            const QJsonObject to = tv.toObject();
            const QJsonObject rec = to.value("recording").toObject();
            Track t;
            t.id = to.value("id").toString();
            t.recordingId = rec.value("id").toString();
            t.title = to.value("title").toString(rec.value("title").toString());
            t.number = to.value("number").toString();
            t.position = to.value("position").toInt();
            t.lengthMs = qint64(to.value("length").toDouble(rec.value("length").toDouble()));
            t.credits = parseCredits(to.value("artist-credit").toArray());
            if (t.credits.isEmpty())
                t.credits = parseCredits(rec.value("artist-credit").toArray());
            for (const QJsonValue &i : rec.value("isrcs").toArray())
                t.isrcs << i.toString();
            m.tracks << t;
        }
        anyTracks = anyTracks || !m.tracks.isEmpty();
        if (!m.trackCount)
            m.trackCount = int(m.tracks.size());
        r.media << m;
    }
    if (r.media.isEmpty() && o.contains("track-count")) {
        Medium m;
        m.position = 1;
        m.trackCount = o.value("track-count").toInt();
        r.media << m;
    }
    r.complete = anyTracks;
    // Genres by votes; MusicBrainz lists them alphabetically.
    QList<QPair<int, QString>> genres;
    for (const QJsonValue &v : o.value("genres").toArray())
        genres.append({v.toObject().value("count").toInt(), v.toObject().value("name").toString()});
    std::stable_sort(genres.begin(), genres.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &g : genres)
        r.genres << g.second;
    return r;
}

Recording Recording::fromJson(const QJsonObject &o) {
    Recording r;
    r.id = o.value("id").toString();
    r.title = o.value("title").toString();
    r.lengthMs = qint64(o.value("length").toDouble());
    r.searchScore = o.value("score").toInt();
    r.credits = parseCredits(o.value("artist-credit").toArray());
    for (const QJsonValue &v : o.value("isrcs").toArray())
        r.isrcs << v.toString();
    for (const QJsonValue &v : o.value("releases").toArray()) {
        const QJsonObject ro = v.toObject();
        ReleaseRef ref;
        ref.id = ro.value("id").toString();
        ref.title = ro.value("title").toString();
        ref.date = ro.value("date").toString();
        ref.country = ro.value("country").toString();
        ref.status = ro.value("status").toString();
        ref.trackCount = ro.value("track-count").toInt();
        ref.releaseGroupId = ro.value("release-group").toObject().value("id").toString();
        r.releases << ref;
    }
    return r;
}

QString quoted(const QString &text) {
    static const QRegularExpression special(R"(([+\-&|!(){}\[\]^"~*?:\\/]))");
    QString escaped = text;
    escaped.replace(special, "\\\\1");
    return '"' + escaped + '"';
}

Client::Client(Net::HttpClient &http) : m_http(http) {}

QJsonObject Client::fetch(const QString &path, const QList<QPair<QString, QString>> &query, QString *error) {
    QList<QPair<QString, QString>> q = query;
    q.append(QPair<QString, QString>("fmt", "json"));
    const QUrl url = Net::makeUrl(Net::baseUrl(Net::Service::MusicBrainz, m_http.library()), path, q);
    const Net::Response r = m_http.get(Net::Service::MusicBrainz, url, cacheSeconds);
    m_offline = r.offline;
    if (!r.ok()) {
        if (error) {
            QString message = r.json().object().value("error").toString();
            *error = r.notFound() ? QString("MusicBrainz has nothing at %1").arg(path)
                                  : QString("MusicBrainz: %1").arg(message.isEmpty() ? r.error : message);
        }
        return {};
    }
    const QJsonDocument doc = r.json();
    if (!doc.isObject()) {
        if (error)
            *error = "MusicBrainz sent something that is not JSON";
        return {};
    }
    return doc.object();
}

Release Client::release(const QString &mbid, QString *error) {
    const QJsonObject o = fetch("/release/" + mbid,
                                {{"inc", "recordings+artist-credits+labels+release-groups+isrcs+genres"}}, error);
    return o.isEmpty() ? Release() : Release::fromJson(o);
}

Recording Client::recording(const QString &mbid, QString *error) {
    const QJsonObject o = fetch("/recording/" + mbid, {{"inc", "releases+artist-credits+isrcs"}}, error);
    return o.isEmpty() ? Recording() : Recording::fromJson(o);
}

QList<Release> Client::searchReleases(const QString &artist, const QString &album, int trackCount, int limit,
                                      QString *error) {
    QStringList parts;
    if (!album.trimmed().isEmpty())
        parts << "release:" + quoted(album.trimmed());
    if (!artist.trimmed().isEmpty())
        parts << "artist:" + quoted(artist.trimmed());
    if (trackCount > 0)
        parts << QString("tracks:%1").arg(trackCount);
    if (parts.isEmpty()) {
        if (error)
            *error = "Nothing to search MusicBrainz by (no album or artist)";
        return {};
    }
    const QJsonObject o = fetch("/release", {{"query", parts.join(" AND ")}, {"limit", QString::number(limit)}}, error);
    QList<Release> out;
    for (const QJsonValue &v : o.value("releases").toArray())
        out << Release::fromJson(v.toObject());
    return out;
}

QList<Recording> Client::searchRecordings(const QString &artist, const QString &title, int limit, QString *error) {
    QStringList parts;
    if (!title.trimmed().isEmpty())
        parts << "recording:" + quoted(title.trimmed());
    if (!artist.trimmed().isEmpty())
        parts << "artist:" + quoted(artist.trimmed());
    if (parts.isEmpty()) {
        if (error)
            *error = "Nothing to search MusicBrainz by (no title or artist)";
        return {};
    }
    const QJsonObject o =
        fetch("/recording", {{"query", parts.join(" AND ")}, {"limit", QString::number(limit)}}, error);
    QList<Recording> out;
    for (const QJsonValue &v : o.value("recordings").toArray())
        out << Recording::fromJson(v.toObject());
    return out;
}

QList<Release> Client::releasesInGroup(const QString &releaseGroupId, QString *error) {
    const QJsonObject o = fetch("/release", {{"release-group", releaseGroupId}, {"limit", "25"}}, error);
    QList<Release> out;
    for (const QJsonValue &v : o.value("releases").toArray())
        out << Release::fromJson(v.toObject());
    return out;
}

}
