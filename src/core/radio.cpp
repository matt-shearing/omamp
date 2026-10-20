#include "radio.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDnsLookup>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>

#include "http.h"
#include "library.h"
#include "m3u.h"

namespace Radio {

namespace {
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }

QString env(const char *name, const QString &fallback) {
    const QString v = qEnvironmentVariable(name).trimmed();
    return v.isEmpty() ? fallback : (v.endsWith('/') ? v.chopped(1) : v);
}

QString bumpSeed() { return QStringLiteral("radio.seed"); }

// Bumped whenever builtinStations() changes, so seed() writes again.
constexpr int kSeedVersion = 1;

QString streamsJson(const QStringList &streams) {
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(streams)).toJson(QJsonDocument::Compact));
}

QStringList streamsFrom(const QString &json) {
    QStringList out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json.toUtf8()).array())
        if (!v.toString().isEmpty())
            out << v.toString();
    return out;
}

QString titleCase(const QString &text) {
    QStringList words = text.toLower().split(' ', Qt::SkipEmptyParts);
    for (QString &w : words) {
        if (w == "fm")
            w = "FM";
        else if (!w.isEmpty())
            w[0] = w[0].toUpper();
    }
    return words.join(' ');
}

const char *kColumns = "id, key, name, network, streams, homepage, art_url, art_path, color, description, tags, "
                       "country, codec, bitrate, meta_kind, meta_key, builtin, position, favourite, last_played, "
                       "listened_ms";
} // namespace

// ---- Station -----------------------------------------------------------

QString networkName(const QString &network) {
    if (network == QLatin1String("nightride"))
        return QStringLiteral("Nightride FM");
    if (network == QLatin1String("rekt"))
        return QStringLiteral("Rekt Network");
    if (network == QLatin1String("radio-browser"))
        return QStringLiteral("radio-browser.info");
    return QStringLiteral("My stations");
}

QString Station::networkName() const { return Radio::networkName(network); }

QString Station::fullName() const {
    // "Nightride FM: Nightride FM" reads badly; the flagship is just its name.
    if (network == QLatin1String("nightride") || network == QLatin1String("rekt")) {
        const QString net = networkName();
        if (name.compare(net, Qt::CaseInsensitive) == 0)
            return name;
        return net + QStringLiteral(": ") + name;
    }
    return name;
}

QString Station::group() const {
    if (network == QLatin1String("nightride") || network == QLatin1String("rekt"))
        return network;
    return QStringLiteral("mine");
}

QJsonObject Station::toJson() const {
    QJsonObject o{{"key", key},
                  {"name", name},
                  {"full_name", fullName()},
                  {"network", network},
                  {"network_name", networkName()},
                  {"streams", QJsonArray::fromStringList(streams)},
                  {"meta", metaKind},
                  {"favourite", favourite},
                  {"builtin", builtin},
                  {"listened_ms", listenedMs}};
    if (!description.isEmpty())
        o["description"] = description;
    if (!homepage.isEmpty())
        o["homepage"] = homepage;
    if (!color.isEmpty())
        o["color"] = color;
    if (!tags.isEmpty())
        o["tags"] = tags;
    if (!country.isEmpty())
        o["country"] = country;
    if (!codec.isEmpty())
        o["codec"] = codec;
    if (bitrate > 0)
        o["bitrate"] = bitrate;
    if (!metaKey.isEmpty())
        o["meta_key"] = metaKey;
    if (lastPlayed > 0)
        o["last_played"] = lastPlayed;
    return o;
}

QVariantMap Station::toMap() const {
    return {{"key", key},
            {"name", name},
            {"fullName", fullName()},
            {"network", network},
            {"networkName", networkName()},
            {"description", description},
            {"color", color},
            {"favourite", favourite},
            {"builtin", builtin},
            {"homepage", homepage},
            {"tags", tags},
            {"country", country},
            {"codec", codec},
            {"bitrate", bitrate},
            {"artPath", artPath},
            {"stream", streams.value(0)}};
}

QString FeedRow::line() const {
    if (!artist.isEmpty() && !title.isEmpty())
        return artist + QStringLiteral(" – ") + title;
    return title.isEmpty() ? artist : title;
}

bool isRadioPath(const QString &path) { return path.startsWith(QLatin1String("radio://")); }

QString keyFromPath(const QString &path) { return isRadioPath(path) ? path.mid(8) : QString(); }

QString nightrideBase() { return env("OMAMP_NIGHTRIDE_URL", QStringLiteral("https://nightride.fm")); }
QString nightrideFeedUrl() { return nightrideBase() + QStringLiteral("/meta"); }
QString nightrideStreamBase() { return env("OMAMP_NIGHTRIDE_STREAM_URL", QStringLiteral("https://stream.nightride.fm")); }

QList<Station> builtinStations() {
    struct Def {
        const char *id, *network, *name, *description, *color;
    };
    // Names as the site writes them, in title case; genres from the site
    // (Nightride FM) and from the channels' own descriptions (Rekt). The
    // colours are Omamp's, chosen to tell the channels apart at a glance.
    static const Def defs[] = {
        {"nightride", "nightride", "Nightride FM", "Synthwave / Retrowave / Outrun", "#e23e8f"},
        {"chillsynth", "nightride", "Chillsynth FM", "Chillsynth / Chillwave / Instrumental", "#2fb8b0"},
        {"datawave", "nightride", "Datawave FM", "Glitchy Synthwave / IDM / Retro Computing", "#3d8bf2"},
        {"spacesynth", "nightride", "Spacesynth FM", "Spacesynth / Space Disco / Vocoder Italo", "#7b5cf0"},
        {"darksynth", "nightride", "Darksynth", "Darksynth / Cyberpunk / Synthmetal", "#d4332f"},
        {"horrorsynth", "nightride", "Horrorsynth", "Horrorsynth / Witch House", "#8e2a6b"},
        {"ebsm", "nightride", "EBSM", "EBSM / Industrial / Clubbing", "#e0902a"},
        {"rekt", "rekt", "Rekt", "Drum & Bass / Dubstep / Techno / Trance", "#ef5a24"},
        {"rektory", "rekt", "Rektory", "1930s Jazz / Swing", "#c9a227"},
        {"rektify", "rekt", "Rektify", "Lo-fi / Hip-hop / Chill", "#a07ad8"},
        {"d-notive", "rekt", "D-Notive", "Synthpop / Eighties", "#e8559f"},
    };
    const QString base = nightrideStreamBase();
    QList<Station> out;
    int position = 0;
    for (const Def &d : defs) {
        Station s;
        const QString id = QString::fromLatin1(d.id);
        s.key = QStringLiteral("nightride/") + id;
        s.name = QString::fromUtf8(d.name);
        s.network = QString::fromLatin1(d.network);
        // MP3 at 320 kbps first, as the site plays it; Opus in Ogg if that
        // won't open.
        s.streams = {base + '/' + id + QStringLiteral(".mp3"), base + '/' + id + QStringLiteral(".ogg")};
        s.homepage = nightrideBase() + QStringLiteral("/?station=") + id;
        s.color = QString::fromLatin1(d.color);
        s.description = QString::fromUtf8(d.description);
        s.codec = QStringLiteral("MP3");
        s.bitrate = 320;
        s.metaKind = QStringLiteral("nightride");
        s.metaKey = id;
        s.builtin = true;
        s.position = position++;
        out << s;
    }
    return out;
}

// ---- The Nightride feed ------------------------------------------------

QList<QByteArray> SseParser::feed(const QByteArray &bytes) {
    QList<QByteArray> events;
    m_buffer += bytes;
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.startsWith("\xEF\xBB\xBF"))
            line.remove(0, 3);
        if (line.isEmpty()) {
            // A blank line ends the event.
            if (!m_data.isEmpty()) {
                if (m_data.trimmed() != "keepalive")
                    events << m_data;
                m_data.clear();
            }
            continue;
        }
        if (line.startsWith(':'))
            continue; // a comment
        if (line.startsWith("data:")) {
            QByteArray value = line.mid(5);
            if (value.startsWith(' '))
                value.remove(0, 1);
            if (!m_data.isEmpty())
                m_data += '\n';
            m_data += value;
        }
    }
    return events;
}

QList<FeedRow> parseFeedData(const QByteArray &data) {
    QList<FeedRow> out;
    const QJsonDocument doc = QJsonDocument::fromJson(data.trimmed());
    QJsonArray rows;
    if (doc.isArray())
        rows = doc.array();
    else if (doc.isObject())
        rows.append(doc.object());
    for (const QJsonValue &v : rows) {
        const QJsonObject o = v.toObject();
        if (o.value("live").toBool())
            continue;
        FeedRow r;
        r.station = o.value("station").toString().trimmed();
        if (r.station.isEmpty())
            continue;
        r.artist = o.value("artist").toString().trimmed();
        r.title = o.value("title").toString().trimmed();
        r.trackId = o.value("track_id").toString();
        r.at = qint64(o.value("at").toDouble());
        out << r;
    }
    return out;
}

QList<CatalogEntry> parseNightrideHomepage(const QString &html) {
    QList<CatalogEntry> out;
    static const QRegularExpression anchor(
        QStringLiteral("<a\\s[^>]*class=\"station\"[^>]*data-station=\"([a-z0-9-]+)\"[^>]*data-type=\"stream\"[^>]*>"
                       "(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression h3(QStringLiteral("<h3>(.*?)</h3>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression h4(QStringLiteral("<h4>(.*?)</h4>"), QRegularExpression::DotMatchesEverythingOption);
    auto it = anchor.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        CatalogEntry e;
        e.id = m.captured(1);
        const QString inner = m.captured(2);
        e.name = titleCase(h3.match(inner).captured(1).trimmed());
        e.description = h4.match(inner).captured(1).trimmed();
        if (e.name.isEmpty())
            e.name = titleCase(e.id);
        out << e;
    }
    return out;
}

// ---- ICY ---------------------------------------------------------------

QString parseStreamTitle(const QByteArray &block) {
    // Servers write Latin-1 or UTF-8; UTF-8 that decodes cleanly wins.
    QByteArray bytes = block;
    const int nul = bytes.indexOf('\0');
    if (nul >= 0)
        bytes.truncate(nul);
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter))
        text = QString::fromLatin1(bytes);
    const int start = text.indexOf(QLatin1String("StreamTitle='"));
    if (start < 0)
        return {};
    const int from = start + 13;
    // The value ends at "';" (a title may hold an apostrophe).
    int end = text.indexOf(QLatin1String("';"), from);
    if (end < 0)
        end = text.lastIndexOf('\'');
    if (end < from)
        return {};
    return text.mid(from, end - from).trimmed();
}

void splitStreamTitle(const QString &streamTitle, QString *artist, QString *title) {
    const QString t = streamTitle.trimmed();
    const int dash = t.indexOf(QLatin1String(" - "));
    if (dash > 0) {
        *artist = t.left(dash).trimmed();
        *title = t.mid(dash + 3).trimmed();
    } else {
        artist->clear();
        *title = t;
    }
}

QStringList IcyParser::feed(const QByteArray &bytes) {
    QStringList titles;
    if (m_metaInt <= 0)
        return titles;
    int i = 0;
    const int n = bytes.size();
    while (i < n) {
        if (m_metaLeft < 0) {
            // In the audio.
            const int take = qMin(m_left, n - i);
            i += take;
            m_left -= take;
            if (m_left == 0)
                m_metaLeft = -2; // the length byte comes next
            continue;
        }
        if (m_metaLeft == -2) {
            m_metaLeft = quint8(bytes[i++]) * 16;
            m_meta.clear();
            if (m_metaLeft == 0) {
                m_metaLeft = -1;
                m_left = m_metaInt;
            }
            continue;
        }
        const int take = qMin(m_metaLeft, n - i);
        m_meta += bytes.mid(i, take);
        i += take;
        m_metaLeft -= take;
        if (m_metaLeft == 0) {
            const QString t = parseStreamTitle(m_meta);
            if (!t.isEmpty() && t != m_last) {
                m_last = t;
                titles << t;
            }
            m_metaLeft = -1;
            m_left = m_metaInt;
        }
    }
    return titles;
}

// ---- Pictures ----------------------------------------------------------

QImage tile(const Station &station, int size) {
    QImage image(size, size, QImage::Format_RGB32);
    QColor base(station.color.isEmpty() ? QStringLiteral("#5b6ee1") : station.color);
    if (!base.isValid())
        base = QColor("#5b6ee1");
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QLinearGradient g(0, 0, size, size);
    g.setColorAt(0, base.lighter(115));
    g.setColorAt(1, base.darker(260));
    p.fillRect(image.rect(), g);
    // A horizon: the sun of a thousand synthwave covers, kept plain.
    p.setPen(Qt::NoPen);
    QColor glow = base.lighter(160);
    glow.setAlpha(70);
    p.setBrush(glow);
    p.drawEllipse(QPointF(size * 0.5, size * 0.62), size * 0.34, size * 0.34);
    QColor line = QColor(255, 255, 255, 40);
    p.setPen(QPen(line, qMax(1, size / 200)));
    for (int i = 0; i < 6; ++i) {
        const qreal y = size * (0.66 + i * i * 0.012 + i * 0.02);
        p.drawLine(QPointF(0, y), QPointF(size, y));
    }
    // The name, as large as fits two lines.
    QFont font;
    font.setBold(true);
    font.setPixelSize(size / 7);
    const QRect box(size / 12, size / 10, size - size / 6, size / 2);
    while (font.pixelSize() > size / 20) {
        p.setFont(font);
        const QRect needed = p.boundingRect(box, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, station.name);
        if (needed.width() <= box.width() && needed.height() <= box.height())
            break;
        font.setPixelSize(font.pixelSize() - qMax(1, size / 100));
    }
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, station.name);
    QFont small;
    small.setPixelSize(size / 20);
    p.setFont(small);
    p.setPen(QColor(255, 255, 255, 200));
    p.drawText(QRect(size / 12, size - size / 6, size - size / 6, size / 10), Qt::AlignLeft | Qt::AlignVCenter,
               station.networkName().toUpper());
    return image;
}

// ---- The table ---------------------------------------------------------

Stations::Stations(Library &library) : m_library(library) {}

Station Stations::read(const QSqlQuery &q) const {
    Station s;
    s.id = q.value(0).toLongLong();
    s.key = q.value(1).toString();
    s.name = q.value(2).toString();
    s.network = q.value(3).toString();
    s.streams = streamsFrom(q.value(4).toString());
    s.homepage = q.value(5).toString();
    s.artUrl = q.value(6).toString();
    s.artPath = q.value(7).toString();
    s.color = q.value(8).toString();
    s.description = q.value(9).toString();
    s.tags = q.value(10).toString();
    s.country = q.value(11).toString();
    s.codec = q.value(12).toString();
    s.bitrate = q.value(13).toInt();
    s.metaKind = q.value(14).toString();
    s.metaKey = q.value(15).toString();
    s.builtin = q.value(16).toBool();
    s.position = q.value(17).toInt();
    s.favourite = q.value(18).toBool();
    s.lastPlayed = q.value(19).toLongLong();
    s.listenedMs = q.value(20).toLongLong();
    return s;
}

bool Stations::seed(QString *error) {
    if (m_library.setting(bumpSeed()).toInt() >= kSeedVersion) {
        // Already seeded; a changed stream base (tests) still rewrites.
        QSqlQuery q(m_library.db());
        q.prepare("SELECT streams FROM radio_stations WHERE key = 'nightride/nightride'");
        if (q.exec() && q.next() && streamsFrom(q.value(0).toString()).value(0).startsWith(nightrideStreamBase() + '/'))
            return true;
    }
    QSqlDatabase db = m_library.db();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO radio_stations (key, name, network, streams, homepage, color, description, codec, bitrate, "
        "meta_kind, meta_key, builtin, position, added) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET name = excluded.name, network = excluded.network, "
        "streams = excluded.streams, homepage = excluded.homepage, color = excluded.color, "
        "description = excluded.description, codec = excluded.codec, bitrate = excluded.bitrate, "
        "meta_kind = excluded.meta_kind, meta_key = excluded.meta_key, builtin = 1, position = excluded.position");
    for (const Station &s : builtinStations()) {
        q.addBindValue(s.key);
        q.addBindValue(s.name);
        q.addBindValue(s.network);
        q.addBindValue(streamsJson(s.streams));
        q.addBindValue(s.homepage);
        q.addBindValue(s.color);
        q.addBindValue(s.description);
        q.addBindValue(s.codec);
        q.addBindValue(s.bitrate);
        q.addBindValue(s.metaKind);
        q.addBindValue(s.metaKey);
        q.addBindValue(s.position);
        q.addBindValue(now());
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            db.rollback();
            return false;
        }
    }
    db.commit();
    m_library.setSetting(bumpSeed(), kSeedVersion);
    return true;
}

QList<Station> Stations::all() const { return group(QStringLiteral("all")); }

QList<Station> Stations::group(const QString &group) const {
    QString where = "1", order = "network = 'custom', network = 'radio-browser', network = 'rekt', position, name";
    if (group == QLatin1String("nightride") || group == QLatin1String("rekt"))
        where = "network = '" + group + "'";
    else if (group == QLatin1String("favourites"))
        where = "favourite = 1";
    else if (group == QLatin1String("mine"))
        where = "network IN ('radio-browser', 'custom')";
    else if (group == QLatin1String("recent")) {
        where = "last_played IS NOT NULL";
        order = "last_played DESC";
    }
    QSqlQuery q(m_library.db());
    QList<Station> out;
    if (!q.exec(QStringLiteral("SELECT %1 FROM radio_stations WHERE %2 ORDER BY %3")
                    .arg(QLatin1String(kColumns), where, order)))
        return out;
    while (q.next())
        out << read(q);
    return out;
}

Station Stations::byKey(const QString &key) const {
    QSqlQuery q(m_library.db());
    q.prepare(QStringLiteral("SELECT %1 FROM radio_stations WHERE key = ?").arg(QLatin1String(kColumns)));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return read(q);
    return {};
}

Station Stations::byId(qint64 id) const {
    QSqlQuery q(m_library.db());
    q.prepare(QStringLiteral("SELECT %1 FROM radio_stations WHERE id = ?").arg(QLatin1String(kColumns)));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return read(q);
    return {};
}

Station Stations::find(const QString &text, QStringList *ambiguous) const {
    QString want = text.trimmed();
    if (isRadioPath(want))
        want = keyFromPath(want);
    if (want.isEmpty())
        return {};
    const QList<Station> stations = all();
    auto same = [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; };
    for (const Station &s : stations)
        if (same(s.key, want) || (s.builtin && same(s.metaKey, want)))
            return s;
    for (const Station &s : stations)
        if (same(s.name, want) || same(s.fullName(), want))
            return s;
    QList<Station> partial;
    for (const Station &s : stations)
        if (s.fullName().contains(want, Qt::CaseInsensitive))
            partial << s;
    if (partial.size() == 1)
        return partial.first();
    if (ambiguous)
        for (const Station &s : partial)
            *ambiguous << s.fullName();
    return {};
}

Station Stations::byStreamUrl(const QString &url) const {
    const QString want = QUrl(url.trimmed()).adjusted(QUrl::StripTrailingSlash).toString();
    if (want.isEmpty())
        return {};
    for (const Station &s : all())
        for (const QString &u : s.streams)
            if (QUrl(u).adjusted(QUrl::StripTrailingSlash).toString() == want)
                return s;
    return {};
}

Station Stations::add(Station s, QString *error, bool *existed) {
    if (existed)
        *existed = false;
    s.streams.removeAll(QString());
    if (s.streams.isEmpty()) {
        if (error)
            *error = QStringLiteral("The station has no stream address");
        return {};
    }
    for (const QString &u : s.streams) {
        const QUrl url(u);
        if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
            if (error)
                *error = QStringLiteral("Not an http or https address: %1").arg(u);
            return {};
        }
    }
    const Station known = byStreamUrl(s.streams.first());
    if (known.isValid()) {
        if (existed)
            *existed = true;
        return known;
    }
    if (!s.key.isEmpty()) {
        const Station same = byKey(s.key);
        if (same.isValid()) {
            if (existed)
                *existed = true;
            return same;
        }
    }
    if (s.network != QLatin1String("radio-browser"))
        s.network = QStringLiteral("custom");
    if (s.name.trimmed().isEmpty())
        s.name = QUrl(s.streams.first()).host();
    if (s.metaKind.isEmpty() || s.metaKind == QLatin1String("nightride"))
        s.metaKind = QStringLiteral("icy");
    QSqlQuery q(m_library.db());
    if (s.key.isEmpty()) {
        // my/<n>, one past the highest.
        qint64 n = 1;
        if (q.exec("SELECT max(CAST(substr(key, 4) AS INTEGER)) FROM radio_stations WHERE key LIKE 'my/%'") && q.next())
            n = q.value(0).toLongLong() + 1;
        s.key = QStringLiteral("my/") + QString::number(n);
    }
    q.prepare("INSERT INTO radio_stations (key, name, network, streams, homepage, art_url, color, description, tags, "
              "country, codec, bitrate, meta_kind, position, added) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
              "(SELECT coalesce(max(position), 0) + 1 FROM radio_stations), ?)");
    q.addBindValue(s.key);
    q.addBindValue(s.name.trimmed());
    q.addBindValue(s.network);
    q.addBindValue(streamsJson(s.streams));
    q.addBindValue(s.homepage);
    q.addBindValue(s.artUrl);
    q.addBindValue(s.color);
    q.addBindValue(s.description);
    q.addBindValue(s.tags);
    q.addBindValue(s.country);
    q.addBindValue(s.codec);
    q.addBindValue(s.bitrate);
    q.addBindValue(s.metaKind);
    q.addBindValue(now());
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return {};
    }
    return byKey(s.key);
}

bool Stations::remove(const QString &key, QString *error) {
    const Station s = byKey(key);
    if (!s.isValid()) {
        if (error)
            *error = QStringLiteral("No such station");
        return false;
    }
    if (s.builtin) {
        if (error)
            *error = QStringLiteral("The Nightride channels are built in");
        return false;
    }
    QSqlQuery q(m_library.db());
    q.prepare("DELETE FROM radio_stations WHERE key = ?");
    q.addBindValue(key);
    return q.exec() && q.numRowsAffected() > 0;
}

bool Stations::rename(const QString &key, const QString &name) {
    if (name.trimmed().isEmpty())
        return false;
    QSqlQuery q(m_library.db());
    q.prepare("UPDATE radio_stations SET name = ? WHERE key = ? AND builtin = 0");
    q.addBindValue(name.trimmed());
    q.addBindValue(key);
    return q.exec() && q.numRowsAffected() > 0;
}

bool Stations::setFavourite(const QString &key, bool on) {
    QSqlQuery q(m_library.db());
    q.prepare("UPDATE radio_stations SET favourite = ? WHERE key = ?");
    q.addBindValue(on ? 1 : 0);
    q.addBindValue(key);
    return q.exec() && q.numRowsAffected() > 0;
}

bool Stations::setArtPath(const QString &key, const QString &path) {
    QSqlQuery q(m_library.db());
    q.prepare("UPDATE radio_stations SET art_path = ? WHERE key = ?");
    q.addBindValue(path);
    q.addBindValue(key);
    return q.exec() && q.numRowsAffected() > 0;
}

void Stations::notePlayed(const QString &key, qint64 at) {
    QSqlQuery q(m_library.db());
    q.prepare("UPDATE radio_stations SET last_played = ? WHERE key = ?");
    q.addBindValue(at > 0 ? at : now());
    q.addBindValue(key);
    q.exec();
}

void Stations::addListened(const QString &key, qint64 ms) {
    if (ms <= 0)
        return;
    QSqlQuery q(m_library.db());
    q.prepare("UPDATE radio_stations SET listened_ms = listened_ms + ? WHERE key = ?");
    q.addBindValue(ms);
    q.addBindValue(key);
    q.exec();
}

qint64 Stations::noteHeard(const QString &key, const QString &artist, const QString &title, bool fromStart,
                           qint64 started) {
    if (title.isEmpty() && artist.isEmpty())
        return 0;
    QSqlQuery q(m_library.db());
    q.prepare("INSERT INTO radio_heard (station_id, artist, title, started, from_start) "
              "SELECT id, ?, ?, ?, ? FROM radio_stations WHERE key = ?");
    q.addBindValue(artist);
    q.addBindValue(title.isEmpty() ? artist : title);
    q.addBindValue(started > 0 ? started : now());
    q.addBindValue(fromStart ? 1 : 0);
    q.addBindValue(key);
    if (!q.exec() || q.numRowsAffected() == 0)
        return 0;
    return q.lastInsertId().toLongLong();
}

void Stations::setHeardMs(qint64 heardId, qint64 ms) {
    QSqlQuery q(m_library.db());
    q.prepare("UPDATE radio_heard SET heard_ms = ? WHERE id = ?");
    q.addBindValue(ms);
    q.addBindValue(heardId);
    q.exec();
}

QList<QVariantMap> Stations::recentlyHeard(int limit) const {
    QSqlQuery q(m_library.db());
    q.prepare("SELECT s.key, s.name, h.artist, h.title, h.started, h.heard_ms FROM radio_heard h "
              "JOIN radio_stations s ON s.id = h.station_id ORDER BY h.started DESC, h.id DESC LIMIT ?");
    q.addBindValue(limit);
    QList<QVariantMap> out;
    if (q.exec())
        while (q.next())
            out << QVariantMap{{"station", q.value(0)}, {"stationName", q.value(1)}, {"artist", q.value(2)},
                               {"title", q.value(3)},   {"started", q.value(4)},     {"heardMs", q.value(5)}};
    return out;
}

int Stations::mergeNightrideCatalog(const QList<CatalogEntry> &site, const QStringList &feedIds) {
    QSet<QString> known;
    for (const Station &s : group("nightride") + group("rekt"))
        known << s.metaKey;
    QSet<QString> onSite;
    for (const CatalogEntry &e : site)
        onSite << e.id;
    static const QRegularExpression safe(QStringLiteral("^[a-z0-9-]{1,40}$"));
    auto addOne = [&](const QString &id, const QString &network, const QString &name, const QString &description) {
        if (known.contains(id) || !safe.match(id).hasMatch() || id == QLatin1String("archives"))
            return 0;
        known << id;
        QSqlQuery q(m_library.db());
        q.prepare("INSERT OR IGNORE INTO radio_stations (key, name, network, streams, homepage, color, description, "
                  "codec, bitrate, meta_kind, meta_key, builtin, position, added) VALUES (?, ?, ?, ?, ?, ?, ?, 'MP3', "
                  "320, 'nightride', ?, 1, (SELECT coalesce(max(position), 0) + 1 FROM radio_stations WHERE builtin = 1), ?)");
        const QString base = nightrideStreamBase();
        q.addBindValue(QStringLiteral("nightride/") + id);
        q.addBindValue(name);
        q.addBindValue(network);
        q.addBindValue(streamsJson({base + '/' + id + ".mp3", base + '/' + id + ".ogg"}));
        q.addBindValue(nightrideBase() + "/?station=" + id);
        // A colour from the id, so a new channel still looks like itself.
        const quint32 h = qHash(id);
        q.addBindValue(QColor::fromHsv(int(h % 360), 170, 210).name());
        q.addBindValue(description);
        q.addBindValue(id);
        q.addBindValue(now());
        return q.exec() ? q.numRowsAffected() : 0;
    };
    int added = 0;
    for (const CatalogEntry &e : site)
        added += addOne(e.id, QStringLiteral("nightride"), e.name, e.description);
    for (const QString &id : feedIds)
        if (!onSite.contains(id))
            added += addOne(id, QStringLiteral("rekt"), titleCase(QString(id).replace('-', ' ')), {});
    return added;
}

int Stations::importFile(const QString &file, QString *error) {
    const PlaylistFiles::Parsed parsed = PlaylistFiles::read(file);
    if (!parsed.ok()) {
        if (error)
            *error = parsed.error;
        return -1;
    }
    int added = 0;
    for (const PlaylistFiles::FileEntry &e : parsed.entries) {
        const QString url = e.location.trimmed();
        if (!url.startsWith("http://", Qt::CaseInsensitive) && !url.startsWith("https://", Qt::CaseInsensitive))
            continue;
        Station s;
        s.name = e.artist.isEmpty() ? e.title : e.artist + " - " + e.title;
        s.streams = {url};
        bool existed = false;
        if (add(s, nullptr, &existed).isValid() && !existed)
            ++added;
    }
    return added;
}

bool Stations::exportFile(const QString &file, const QString &which, QString *error) const {
    const QList<Station> stations = group(which.isEmpty() ? QStringLiteral("all") : which);
    QString text;
    const bool pls = file.endsWith(".pls", Qt::CaseInsensitive);
    if (pls) {
        text = "[playlist]\n";
        int n = 0;
        for (const Station &s : stations) {
            if (s.streams.isEmpty())
                continue;
            ++n;
            text += QStringLiteral("File%1=%2\nTitle%1=%3\nLength%1=-1\n").arg(n).arg(s.streams.first(), s.fullName());
        }
        text += QStringLiteral("NumberOfEntries=%1\nVersion=2\n").arg(n);
    } else {
        text = "#EXTM3U\n";
        for (const Station &s : stations) {
            if (s.streams.isEmpty())
                continue;
            text += QStringLiteral("#EXTINF:-1,%1\n%2\n").arg(s.fullName(), s.streams.first());
        }
    }
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(text.toUtf8()) < 0 || !out.commit()) {
        if (error)
            *error = QStringLiteral("Can't write %1: %2").arg(file, out.errorString());
        return false;
    }
    return true;
}

// ---- radio-browser.info --------------------------------------------------

namespace Browser {

QString baseUrl(Net::HttpClient *http) {
    const QString forced = env("OMAMP_RADIOBROWSER_URL", {});
    if (!forced.isEmpty())
        return forced;
    QStringList hosts;
    // The service's own advice: the SRV record names its API servers.
    QDnsLookup dns(QDnsLookup::SRV, QStringLiteral("_api._tcp.radio-browser.info"));
    QEventLoop loop;
    QObject::connect(&dns, &QDnsLookup::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    dns.lookup();
    loop.exec();
    if (dns.error() == QDnsLookup::NoError)
        for (const QDnsServiceRecord &r : dns.serviceRecords())
            hosts << r.target().chopped(r.target().endsWith('.') ? 1 : 0);
    if (hosts.isEmpty() && http) {
        const Net::Response r = http->get(Net::Service::Other,
                                          QUrl("https://all.api.radio-browser.info/json/servers"), 3600);
        for (const QJsonValue &v : r.json().array())
            if (!v.toObject().value("name").toString().isEmpty())
                hosts << v.toObject().value("name").toString();
        hosts.removeDuplicates();
    }
    if (hosts.isEmpty())
        hosts << QStringLiteral("de1.api.radio-browser.info");
    return QStringLiteral("https://") + hosts.at(int(QRandomGenerator::global()->bounded(hosts.size())));
}

Station fromJson(const QJsonObject &o) {
    Station s;
    const QString uuid = o.value("stationuuid").toString();
    if (!uuid.isEmpty())
        s.key = QStringLiteral("rb/") + uuid;
    s.name = o.value("name").toString().trimmed();
    s.network = QStringLiteral("radio-browser");
    const QString resolved = o.value("url_resolved").toString().trimmed();
    const QString url = o.value("url").toString().trimmed();
    if (!resolved.isEmpty())
        s.streams << resolved;
    if (!url.isEmpty() && url != resolved && !url.endsWith(".pls") && !url.endsWith(".m3u"))
        s.streams << url;
    s.homepage = o.value("homepage").toString();
    s.artUrl = o.value("favicon").toString();
    s.tags = o.value("tags").toString().split(',', Qt::SkipEmptyParts).mid(0, 5).join(", ");
    s.country = o.value("countrycode").toString();
    s.codec = o.value("codec").toString();
    s.bitrate = o.value("bitrate").toInt();
    s.description = s.tags;
    s.metaKind = QStringLiteral("icy");
    return s;
}

Result search(Net::HttpClient &http, const QString &base, const QString &query, int limit) {
    Result out;
    if (query.trimmed().isEmpty())
        return out;
    const QUrl url = Net::makeUrl(base, "/json/stations/search",
                                  {{"name", query.trimmed()},
                                   {"limit", QString::number(qBound(1, limit, 200))},
                                   {"hidebroken", "true"},
                                   {"order", "clickcount"},
                                   {"reverse", "true"}});
    const Net::Response r = http.get(Net::Service::Other, url, 600);
    if (!r.ok()) {
        out.error = r.offline ? QStringLiteral("radio-browser.info can't be reached")
                              : QStringLiteral("radio-browser.info answered %1").arg(r.error.isEmpty() ? QString::number(r.status) : r.error);
        return out;
    }
    const QJsonDocument doc = r.json();
    if (!doc.isArray()) {
        out.error = QStringLiteral("radio-browser.info sent something that isn't a station list");
        return out;
    }
    for (const QJsonValue &v : doc.array()) {
        Station s = fromJson(v.toObject());
        if (s.isValid() && !s.streams.isEmpty() && !s.name.isEmpty())
            out.stations << s;
    }
    return out;
}

void countClick(Net::HttpClient &http, const QString &base, const QString &uuid) {
    if (uuid.isEmpty())
        return;
    Net::Request r;
    r.service = Net::Service::Other;
    r.url = QUrl(base + "/json/url/" + QUrl::toPercentEncoding(uuid));
    r.retry = false;
    r.timeoutMs = 8000;
    http.send(r);
}

} // namespace Browser
} // namespace Radio
