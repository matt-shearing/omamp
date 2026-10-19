#include "autodj.h"

#include "library.h"
#include "playlists.h"
#include "query.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlQuery>

namespace AutoDj {

namespace {

QStringList splitArtists(const QString &artist) {
    QStringList out;
    for (const QString &a : artist.split(';', Qt::SkipEmptyParts)) {
        const QString f = a.simplified().toCaseFolded();
        if (!f.isEmpty())
            out << f;
    }
    return out;
}

} // namespace

Source Source::parse(const QString &text) {
    Source s;
    const QString t = text.trimmed();
    auto after = [&](const QString &prefix) { return t.mid(prefix.size()).trimmed(); };
    if (t.isEmpty() || t.compare("library", Qt::CaseInsensitive) == 0) {
        s.kind = Library;
    } else if (t.startsWith("collection:", Qt::CaseInsensitive)) {
        s.kind = Collection;
        s.ref = after("collection:");
    } else if (t.startsWith("playlist:", Qt::CaseInsensitive)) {
        s.kind = Playlist;
        s.ref = after("playlist:");
    } else if (t.startsWith("query:", Qt::CaseInsensitive)) {
        s.kind = Query;
        s.ref = after("query:");
    } else {
        s.kind = Query;
        s.ref = t;
    }
    return s;
}

QString Source::toString() const {
    switch (kind) {
    case Library:
        return "library";
    case Collection:
        return "collection:" + ref;
    case Playlist:
        return "playlist:" + ref;
    case Query:
        return "query:" + ref;
    }
    return {};
}

QJsonObject Settings::toJson() const {
    return {{"source", source.toString()},
            {"upcoming", upcoming},
            {"artist_separation", artistSeparation},
            {"skip_played_within_days", skipPlayedWithinDays},
            {"weight_by_rating", weightByRating},
            {"never_played_boost", neverPlayedBoost},
            {"online_only", onlineOnly},
            {"exclude_types", QJsonArray::fromStringList(excludeTypes)}};
}

Settings Settings::fromJson(const QJsonObject &o) {
    Settings s;
    s.source = Source::parse(o.value("source").toString("library"));
    s.upcoming = qBound(1, o.value("upcoming").toInt(s.upcoming), 500);
    s.artistSeparation = qBound(0, o.value("artist_separation").toInt(s.artistSeparation), 500);
    s.skipPlayedWithinDays = qMax(0, o.value("skip_played_within_days").toInt(s.skipPlayedWithinDays));
    s.weightByRating = o.value("weight_by_rating").toBool(s.weightByRating);
    s.neverPlayedBoost = qMax(0.0, o.value("never_played_boost").toDouble(s.neverPlayedBoost));
    s.onlineOnly = o.value("online_only").toBool(s.onlineOnly);
    if (o.contains("exclude_types")) {
        s.excludeTypes.clear();
        for (const QJsonValue &v : o.value("exclude_types").toArray())
            s.excludeTypes << v.toString();
    }
    return s;
}

Settings Settings::load(::Library &library) {
    return fromJson(QJsonDocument::fromJson(library.setting("autodj").toString().toUtf8()).object());
}

void Settings::save(::Library &library) const {
    library.setSetting("autodj", QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact)));
}

QJsonObject Pick::toJson() const {
    return {{"track_id", trackId}, {"artist", artist}, {"title", title}, {"relaxed", QJsonArray::fromStringList(relaxed)}};
}

QList<qint64> sourceIds(::Library &library, const Source &source, QString *error) {
    QString why;
    QList<qint64> ids;
    switch (source.kind) {
    case Source::Library:
        ids = Query::ids(library, "", {}, &why);
        break;
    case Source::Collection: {
        const qint64 id = Collections::resolve(library, source.ref, &why);
        if (id)
            ids = Collections::trackIds(library, id, {}, &why);
        break;
    }
    case Source::Playlist: {
        const qint64 id = Playlists::resolve(library, source.ref, &why);
        if (id)
            ids = Playlists::trackIds(library, id, &why);
        break;
    }
    case Source::Query:
        ids = Query::ids(library, source.ref, {}, &why);
        break;
    }
    if (error)
        *error = why;
    return ids;
}

Dj::Dj(::Library &library, const Settings &settings, quint32 seed)
    : m_library(library), m_settings(settings),
      m_seed(seed ? seed : QRandomGenerator::global()->bounded(1u, 1u << 31)), m_random(m_seed) {}

void Dj::reload() {
    m_loaded = false;
    m_candidates.clear();
    m_index.clear();
}

int Dj::candidates() {
    load();
    return m_candidates.size();
}

bool Dj::load() {
    if (m_loaded)
        return m_error.isEmpty();
    m_loaded = true;
    m_error.clear();
    QList<qint64> ids = sourceIds(m_library, m_settings.source, &m_error);
    if (!m_error.isEmpty())
        return false;
    // A playlist may list a track twice; Auto-DJ draws each once.
    QSet<qint64> seen;
    QList<qint64> unique;
    for (qint64 id : ids)
        if (!seen.contains(id)) {
            seen.insert(id);
            unique << id;
        }
    QHash<qint64, Candidate> rows;
    for (int start = 0; start < unique.size(); start += 500) {
        const QList<qint64> chunk = unique.mid(start, 500);
        QStringList marks;
        for (int i = 0; i < chunk.size(); ++i)
            marks << "?";
        QSqlQuery q(m_library.db());
        q.prepare("SELECT id, artist, title, rating, play_count, last_played, availability, content_type "
                  "FROM tracks WHERE id IN (" + marks.join(',') + ")");
        for (qint64 id : chunk)
            q.addBindValue(id);
        if (!q.exec())
            continue;
        while (q.next()) {
            if (m_settings.onlineOnly && q.value(6).toString() != "online")
                continue;
            if (m_settings.excludeTypes.contains(q.value(7).toString()))
                continue;
            Candidate c;
            c.id = q.value(0).toLongLong();
            c.artist = q.value(1).toString();
            c.title = q.value(2).toString();
            c.artists = splitArtists(c.artist);
            c.lastPlayed = q.value(5).toLongLong();
            double w = 1.0;
            if (m_settings.weightByRating && !q.value(3).isNull()) {
                const double r = q.value(3).toDouble() / 100.0;
                w = 0.25 + 2.75 * r * r; // 0 stars 0.25, 3 stars 1.24, 5 stars 3.0; unrated 1.0
            }
            if (q.value(4).toLongLong() == 0 && q.value(5).isNull())
                w *= m_settings.neverPlayedBoost > 0 ? m_settings.neverPlayedBoost : 1.0;
            c.weight = qMax(0.01, w);
            rows.insert(c.id, c);
        }
    }
    // Keep the source's order, so a seed always gives the same picks.
    for (qint64 id : unique)
        if (rows.contains(id)) {
            m_index.insert(id, m_candidates.size());
            m_candidates << rows.value(id);
        }
    if (m_candidates.isEmpty())
        m_error = QString("Auto-DJ has nothing to play from %1").arg(m_settings.source.toString());
    return m_error.isEmpty();
}

QStringList Dj::artistsOf(qint64 trackId) {
    if (const auto it = m_index.constFind(trackId); it != m_index.constEnd())
        return m_candidates[*it].artists;
    QSqlQuery q(m_library.db());
    q.prepare("SELECT artist FROM tracks WHERE id = ?");
    q.addBindValue(trackId);
    return q.exec() && q.next() ? splitArtists(q.value(0).toString()) : QStringList();
}

QList<Pick> Dj::pick(int count, const QList<qint64> &history) {
    QList<Pick> out;
    if (count <= 0 || !load())
        return out;
    const qint64 now = m_now > 0 ? m_now : QDateTime::currentSecsSinceEpoch();
    const qint64 cutoff =
        m_settings.skipPlayedWithinDays > 0 ? now - qint64(m_settings.skipPlayedWithinDays) * 86400 : LLONG_MIN;
    QList<qint64> sequence = history;
    QHash<qint64, QStringList> artistCache;
    auto artists = [&](qint64 id) {
        auto it = artistCache.find(id);
        if (it == artistCache.end())
            it = artistCache.insert(id, artistsOf(id));
        return *it;
    };
    for (int n = 0; n < count; ++n) {
        // Artists of the last K tracks.
        QSet<QString> recentArtists;
        for (int i = qMax(0, int(sequence.size()) - m_settings.artistSeparation); i < sequence.size(); ++i)
            for (const QString &a : artists(sequence[i]))
                recentArtists.insert(a);
        // Tracks already played or queued: avoid them while the source has
        // others, looking back no further than the source is long.
        QSet<qint64> recentIds;
        const int window = qMax(0, int(m_candidates.size()) - 1);
        for (int i = qMax(0, int(sequence.size()) - window); i < sequence.size(); ++i)
            recentIds.insert(sequence[i]);

        // Strictest first; each level drops one more rule.
        const QList<QStringList> levels = {{}, {"artist"}, {"artist", "recent"}, {"artist", "recent", "repeat"}};
        QList<int> eligible;
        QStringList relaxed;
        for (const QStringList &level : levels) {
            eligible.clear();
            for (int i = 0; i < m_candidates.size(); ++i) {
                const Candidate &c = m_candidates[i];
                if (!level.contains("repeat") && recentIds.contains(c.id))
                    continue;
                if (!level.contains("recent") && c.lastPlayed > cutoff && c.lastPlayed > 0)
                    continue;
                if (!level.contains("artist")) {
                    bool clash = false;
                    for (const QString &a : c.artists)
                        if (recentArtists.contains(a)) {
                            clash = true;
                            break;
                        }
                    if (clash)
                        continue;
                }
                eligible << i;
            }
            if (!eligible.isEmpty()) {
                relaxed = level;
                break;
            }
        }
        if (eligible.isEmpty())
            break;
        double total = 0;
        for (int i : eligible)
            total += m_candidates[i].weight;
        double roll = m_random.generateDouble() * total;
        int chosen = eligible.last();
        for (int i : eligible) {
            roll -= m_candidates[i].weight;
            if (roll < 0) {
                chosen = i;
                break;
            }
        }
        const Candidate &c = m_candidates[chosen];
        out << Pick{c.id, c.artist, c.title, relaxed};
        sequence << c.id;
    }
    return out;
}

QList<Pick> Dj::refill(const QList<qint64> &history, int queued) {
    return pick(m_settings.upcoming - queued, history);
}

} // namespace AutoDj

namespace PartyMode {

namespace {

struct Named {
    Action action;
    const char *name;
};
const Named names[] = {{Action::Play, "play"},
                       {Action::Skip, "skip"},
                       {Action::Seek, "seek"},
                       {Action::Volume, "volume"},
                       {Action::AddToQueue, "add"},
                       {Action::RemoveFromQueue, "remove"},
                       {Action::Reorder, "reorder"},
                       {Action::EditTags, "edit-tags"},
                       {Action::EditPlaylists, "edit-playlists"},
                       {Action::Settings, "settings"},
                       {Action::Library, "library"},
                       {Action::Quit, "quit"}};

const int pinIterationsDefault = 100000;

QString hashPin(const QString &pin, const QString &salt, int iterations) {
    const QByteArray s = salt.toUtf8();
    QByteArray h = QCryptographicHash::hash(s + pin.toUtf8(), QCryptographicHash::Sha256);
    for (int i = 1; i < iterations; ++i)
        h = QCryptographicHash::hash(h + s, QCryptographicHash::Sha256);
    return QString::fromLatin1(h.toHex());
}

} // namespace

QString actionName(Action a) {
    for (const Named &n : names)
        if (n.action == a)
            return n.name;
    return {};
}

bool parseAction(const QString &name, Action *out) {
    for (const Named &n : names)
        if (name == QLatin1String(n.name)) {
            *out = n.action;
            return true;
        }
    return false;
}

QList<Action> allActions() {
    QList<Action> out;
    for (const Named &n : names)
        out << n.action;
    return out;
}

QList<Action> presetActions(const QString &preset) {
    if (preset == "jukebox")
        return {Action::Library, Action::AddToQueue, Action::Play};
    if (preset == "dj")
        return {Action::Library, Action::AddToQueue, Action::RemoveFromQueue, Action::Reorder,
                Action::Play,    Action::Skip,       Action::Seek,            Action::Volume};
    return {}; // display: look, don't touch
}

bool applyPreset(Settings &s, const QString &preset) {
    if (!QStringList{"jukebox", "dj", "display"}.contains(preset))
        return false;
    s.preset = preset;
    s.allowed = presetActions(preset);
    return true;
}

QJsonObject Settings::toJson(bool withSecrets) const {
    QJsonArray actions;
    for (Action a : allowed)
        actions << actionName(a);
    QJsonObject o{{"enabled", enabled}, {"preset", preset}, {"allowed", actions}, {"has_pin", hasPin()}};
    if (withSecrets) {
        o["pin_hash"] = pinHash;
        o["pin_salt"] = pinSalt;
        o["pin_iterations"] = pinIterations;
    }
    return o;
}

Settings Settings::fromJson(const QJsonObject &o) {
    Settings s;
    s.enabled = o.value("enabled").toBool();
    s.preset = o.value("preset").toString("jukebox");
    if (o.contains("allowed")) {
        for (const QJsonValue &v : o.value("allowed").toArray()) {
            Action a;
            if (parseAction(v.toString(), &a))
                s.allowed << a;
        }
    } else {
        s.allowed = presetActions(s.preset);
    }
    s.pinHash = o.value("pin_hash").toString();
    s.pinSalt = o.value("pin_salt").toString();
    s.pinIterations = o.value("pin_iterations").toInt();
    return s;
}

Settings Settings::load(::Library &library) {
    const QJsonObject o = QJsonDocument::fromJson(library.setting("party_mode").toString().toUtf8()).object();
    return fromJson(o);
}

void Settings::save(::Library &library) const {
    library.setSetting("party_mode", QString::fromUtf8(QJsonDocument(toJson(true)).toJson(QJsonDocument::Compact)));
}

void setPin(Settings &s, const QString &pin) {
    if (pin.isEmpty()) {
        s.pinHash.clear();
        s.pinSalt.clear();
        s.pinIterations = 0;
        return;
    }
    QByteArray salt(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(salt.data()), salt.size() / 4);
    s.pinSalt = QString::fromLatin1(salt.toHex());
    s.pinIterations = pinIterationsDefault;
    s.pinHash = hashPin(pin, s.pinSalt, s.pinIterations);
}

bool checkPin(const Settings &s, const QString &pin) {
    if (!s.hasPin())
        return true;
    const QByteArray expected = s.pinHash.toLatin1();
    const QByteArray got = hashPin(pin, s.pinSalt, qMax(1, s.pinIterations)).toLatin1();
    // Compare every byte, so the time taken says nothing about the PIN.
    if (expected.size() != got.size())
        return false;
    int diff = 0;
    for (int i = 0; i < got.size(); ++i)
        diff |= expected[i] ^ got[i];
    return diff == 0;
}

bool allowed(const Settings &s, Action a) { return !s.enabled || s.allowed.contains(a); }

bool enable(::Library &library, const QString &preset, const QString &pin) {
    Settings s = Settings::load(library);
    if (!preset.isEmpty() && !applyPreset(s, preset))
        return false;
    if (!pin.isEmpty())
        setPin(s, pin);
    s.enabled = true;
    s.save(library);
    return true;
}

bool disable(::Library &library, const QString &pin) {
    Settings s = Settings::load(library);
    if (!s.enabled)
        return true;
    if (!checkPin(s, pin))
        return false;
    s.enabled = false;
    s.save(library);
    return true;
}

}
