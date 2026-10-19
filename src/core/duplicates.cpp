#include "duplicates.h"

#include "job.h"
#include "library.h"
#include "tagwriter.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <deque>
#include <memory>
#include <numeric>

namespace Duplicates {

QString name(By by) {
    switch (by) {
    case By::Tags: return "tags";
    case By::Content: return "content";
    case By::Fingerprint: return "fingerprint";
    }
    return {};
}

bool parse(const QString &text, By *by) {
    const QString t = text.trimmed().toLower();
    if (t == "tags" || t == "tag" || t == "title")
        *by = By::Tags;
    else if (t == "content" || t == "hash" || t == "bytes")
        *by = By::Content;
    else if (t == "fingerprint" || t == "audio" || t == "chromaprint")
        *by = By::Fingerprint;
    else
        return false;
    return true;
}

QJsonObject Track::toJson() const {
    return {{"id", id},
            {"path", path.isEmpty() ? QJsonValue() : QJsonValue(path)},
            {"title", title},
            {"artist", artist},
            {"album", album},
            {"format", format},
            {"bitrate", bitrate},
            {"duration_ms", durationMs},
            {"size", size},
            {"plays", plays},
            {"rating", rating < 0 ? QJsonValue() : QJsonValue(rating)},
            {"availability", availability},
            {"keeper", keeper}};
}

QJsonObject Group::toJson() const {
    QJsonArray list;
    for (const Track &t : tracks)
        list << t.toJson();
    return {{"key", key}, {"keeper_id", keeperId}, {"reason", reason}, {"tracks", list}};
}

QJsonObject Report::toJson() const {
    QJsonArray list;
    for (const Group &g : groups)
        list << g.toJson();
    QJsonObject o{{"ok", ok},
                  {"by", name(by)},
                  {"groups", list},
                  {"group_count", groups.size()},
                  {"tracks", tracks},
                  {"elapsed_ms", elapsedMs}};
    if (by == By::Fingerprint)
        o["without_fingerprint"] = withoutFingerprint;
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QString normalise(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD).toLower();
    QString out;
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        out += c.isLetterOrNumber() ? c : QChar(' ');
    }
    out = out.simplified();
    if (out.startsWith("the "))
        out = out.mid(4);
    return out;
}

namespace {

bool lossless(const QString &format) {
    static const QStringList list = {"flac", "wav", "alac", "ape", "wv", "aif", "aiff", "dsf", "dff", "tta"};
    return list.contains(format.toLower());
}

// Negative when a is the better keeper.
int compareKeepers(const Track &a, const Track &b, QString *why) {
    auto decide = [&](int result, const QString &reason) {
        if (result && why)
            *why = reason;
        return result;
    };
    const bool onlineA = a.availability == "online", onlineB = b.availability == "online";
    if (onlineA != onlineB)
        return decide(onlineA ? -1 : 1, "its file is there");
    if (lossless(a.format) != lossless(b.format)) {
        const Track &w = lossless(a.format) ? a : b;
        return decide(lossless(a.format) ? -1 : 1, QString("lossless (%1)").arg(w.format));
    }
    if (a.bitrate != b.bitrate)
        return decide(a.bitrate > b.bitrate ? -1 : 1,
                      QString("highest bitrate (%1 kbps)").arg(qMax(a.bitrate, b.bitrate)));
    if (a.plays != b.plays)
        return decide(a.plays > b.plays ? -1 : 1, QString("most plays (%1)").arg(qMax(a.plays, b.plays)));
    if (a.rating != b.rating)
        return decide(a.rating > b.rating ? -1 : 1, "best rating");
    return decide(a.id < b.id ? -1 : a.id > b.id ? 1 : 0, "first in the library");
}

QList<Track> load(Library &library, const QList<qint64> &ids) {
    QList<Track> out;
    if (ids.isEmpty())
        return out;
    QStringList list;
    for (qint64 id : ids)
        list << QString::number(id);
    QSqlQuery q(library.db());
    // The ids come from the database, so they go in as text.
    q.exec("SELECT t.id, v.root_path, v.online, t.rel_path, t.title, t.artist, t.album, t.format, t.availability, "
           "t.bitrate, t.duration_ms, t.size, t.play_count, t.rating FROM tracks t JOIN volumes v ON v.id = t.volume_id "
           "WHERE t.id IN (" + list.join(',') + ')');
    while (q.next()) {
        Track t;
        t.id = q.value(0).toLongLong();
        if (q.value(2).toBool()) {
            const QString root = q.value(1).toString();
            t.path = (root == "/" ? QString() : root) + '/' + q.value(3).toString();
        }
        t.title = q.value(4).toString();
        t.artist = q.value(5).toString();
        t.album = q.value(6).toString();
        t.format = q.value(7).toString();
        t.availability = q.value(8).toString();
        t.bitrate = q.value(9).toInt();
        t.durationMs = q.value(10).toLongLong();
        t.size = q.value(11).toLongLong();
        t.plays = q.value(12).toInt();
        t.rating = q.value(13).isNull() ? -1 : q.value(13).toInt();
        out << t;
    }
    return out;
}

QList<quint32> unpack(const QByteArray &blob) {
    QList<quint32> out;
    out.reserve(blob.size() / 4);
    for (int k = 0; k + 4 <= blob.size(); k += 4)
        out << qFromLittleEndian<quint32>(blob.constData() + k);
    return out;
}

QByteArray pack(const QJsonArray &values) {
    QByteArray out(values.size() * 4, Qt::Uninitialized);
    for (int k = 0; k < values.size(); ++k)
        qToLittleEndian<quint32>(quint32(values[k].toDouble()), out.data() + k * 4);
    return out;
}

// A tiny union-find for grouping tracks that match pairwise.
struct Groups {
    QHash<qint64, qint64> parent;
    qint64 find(qint64 x) const {
        while (parent.value(x, x) != x)
            x = parent.value(x);
        return x;
    }
    void join(qint64 a, qint64 b) {
        parent.insert(a, parent.value(a, a));
        parent.insert(b, parent.value(b, b));
        const qint64 ra = find(a), rb = find(b);
        if (ra != rb)
            parent[ra] = rb;
    }
};

} // namespace

double similarity(const QList<quint32> &a, const QList<quint32> &b) {
    double best = 0;
    for (int offset = -3; offset <= 3; ++offset) {
        qint64 matched = 0, bits = 0;
        for (int i = 0; i < a.size(); ++i) {
            const int j = i + offset;
            if (j < 0 || j >= b.size())
                continue;
            matched += 32 - __builtin_popcount(a[i] ^ b[j]);
            bits += 32;
        }
        if (bits >= 32 * 8)
            best = qMax(best, double(matched) / bits);
    }
    return best;
}

void chooseKeeper(Group &group) {
    std::sort(group.tracks.begin(), group.tracks.end(),
              [](const Track &a, const Track &b) { return compareKeepers(a, b, nullptr) < 0; });
    for (Track &t : group.tracks)
        t.keeper = false;
    if (group.tracks.isEmpty())
        return;
    group.tracks[0].keeper = true;
    group.keeperId = group.tracks[0].id;
    group.reason.clear();
    if (group.tracks.size() > 1)
        compareKeepers(group.tracks[0], group.tracks[1], &group.reason);
}

Report find(Library &library, const Options &options) {
    Report report;
    report.by = options.by;
    QElapsedTimer timer;
    timer.start();
    const QString available = options.includeMissing ? QString("1") : QString("t.availability <> 'missing'");
    QList<QPair<QString, QList<qint64>>> found; // key -> track ids

    if (options.by == By::Tags) {
        QSqlQuery q(library.db());
        q.setForwardOnly(true);
        q.exec("SELECT t.id, t.artist, t.title, coalesce(t.duration_ms, 0) FROM tracks t WHERE "
               "coalesce(t.title, '') <> '' AND coalesce(t.artist, '') <> '' AND " + available);
        struct Row {
            qint64 id, duration;
        };
        QHash<QString, QList<Row>> byKey;
        while (q.next()) {
            const QString key = normalise(q.value(1).toString()) + " - " + normalise(q.value(2).toString());
            byKey[key] << Row{q.value(0).toLongLong(), q.value(3).toLongLong()};
        }
        for (auto it = byKey.begin(); it != byKey.end(); ++it) {
            QList<Row> &rows = it.value();
            if (rows.size() < 2)
                continue;
            if (!options.matchLength) {
                QList<qint64> ids;
                for (const Row &r : rows)
                    ids << r.id;
                found.append({it.key(), ids});
                continue;
            }
            // Lengths sorted; a gap wider than the tolerance starts a new group.
            std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.duration < b.duration; });
            QList<qint64> current{rows[0].id};
            for (int k = 1; k <= rows.size(); ++k) {
                if (k == rows.size() || rows[k].duration - rows[k - 1].duration > options.lengthToleranceMs) {
                    if (current.size() > 1)
                        found.append({it.key(), current});
                    current.clear();
                }
                if (k < rows.size())
                    current << rows[k].id;
            }
        }
    } else if (options.by == By::Content) {
        QSqlQuery q(library.db());
        q.setForwardOnly(true);
        q.exec("SELECT t.quick_hash, t.id FROM tracks t WHERE t.quick_hash IN (SELECT quick_hash FROM tracks t "
               "WHERE quick_hash IS NOT NULL AND " + available + " GROUP BY quick_hash HAVING count(*) > 1) AND " +
               available + " ORDER BY t.quick_hash, t.id");
        QString last;
        while (q.next()) {
            const QString hash = q.value(0).toString();
            if (found.isEmpty() || hash != last)
                found.append({hash, {}});
            found.last().second << q.value(1).toLongLong();
            last = hash;
        }
    } else {
        struct Print {
            qint64 id;
            double duration;
            QList<quint32> values;
        };
        QList<Print> prints;
        QSqlQuery q(library.db());
        q.setForwardOnly(true);
        q.exec("SELECT f.track_id, f.duration, f.fingerprint FROM fingerprints f JOIN tracks t ON t.id = f.track_id "
               "WHERE length(f.fingerprint) > 0 AND f.size = t.size AND f.mtime_ns = t.mtime_ns AND " + available +
               " ORDER BY f.duration");
        while (q.next())
            prints << Print{q.value(0).toLongLong(), q.value(1).toDouble(), unpack(q.value(2).toByteArray())};
        QSqlQuery missing(library.db());
        missing.exec("SELECT count(*) FROM tracks t LEFT JOIN fingerprints f ON f.track_id = t.id "
                     "WHERE t.availability = 'online' AND (f.track_id IS NULL OR f.size <> t.size OR "
                     "f.mtime_ns <> t.mtime_ns)");
        if (missing.next())
            report.withoutFingerprint = missing.value(0).toInt();
        // Two encodings of one recording share many exact fingerprint
        // values, so only prints sharing a few values in their first two
        // minutes, and within five seconds of each other in length, are
        // compared bit by bit. Values shared by very many prints (silence,
        // a common hum) say nothing and are skipped.
        QHash<quint32, QList<int>> byValue;
        for (int i = 0; i < prints.size(); ++i) {
            QSet<quint32> seen;
            for (int k = 0; k < prints[i].values.size() && k < 120 * 8; ++k)
                if (!seen.contains(prints[i].values[k])) {
                    seen.insert(prints[i].values[k]);
                    byValue[prints[i].values[k]] << i;
                }
        }
        Groups groups;
        for (int i = 0; i < prints.size(); ++i) {
            QHash<int, int> shared;
            QSet<quint32> seen;
            for (int k = 0; k < prints[i].values.size() && k < 120 * 8; ++k) {
                const quint32 v = prints[i].values[k];
                if (seen.contains(v))
                    continue;
                seen.insert(v);
                const QList<int> &bucket = byValue[v];
                if (bucket.size() > 200)
                    continue;
                for (int j : bucket)
                    if (j > i)
                        ++shared[j];
            }
            for (auto it = shared.constBegin(); it != shared.constEnd(); ++it) {
                const int j = it.key();
                if (it.value() < 3 || qAbs(prints[j].duration - prints[i].duration) > 5.0)
                    continue;
                if (similarity(prints[i].values, prints[j].values) >= options.minSimilarity)
                    groups.join(prints[i].id, prints[j].id);
            }
        }
        QHash<qint64, QList<qint64>> members;
        for (const Print &p : std::as_const(prints))
            if (groups.parent.contains(p.id))
                members[groups.find(p.id)] << p.id;
        for (auto it = members.constBegin(); it != members.constEnd(); ++it)
            if (it.value().size() > 1)
                found.append({QString("fingerprint:%1").arg(it.key()), it.value()});
    }

    for (const auto &entry : std::as_const(found)) {
        Group g;
        g.key = entry.first;
        g.tracks = load(library, entry.second);
        if (g.tracks.size() < 2)
            continue;
        chooseKeeper(g);
        report.tracks += g.tracks.size();
        report.groups << g;
    }
    std::sort(report.groups.begin(), report.groups.end(), [](const Group &a, const Group &b) {
        const Track &x = a.tracks.first(), &y = b.tracks.first();
        const int byArtist = x.artist.compare(y.artist, Qt::CaseInsensitive);
        if (byArtist)
            return byArtist < 0;
        const int byTitle = x.title.compare(y.title, Qt::CaseInsensitive);
        return byTitle ? byTitle < 0 : x.id < y.id;
    });
    report.elapsedMs = timer.elapsed();
    return report;
}

}

QJsonObject Fingerprinter::Result::toJson() const {
    QJsonObject o{{"ok", ok},           {"total", total},         {"computed", computed},
                  {"failed", failed},   {"cancelled", cancelled}, {"elapsed_ms", elapsedMs}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

Fingerprinter::Fingerprinter(Library &library, Job *job) : m_library(library), m_job(job), m_program(findProgram()) {}

QString Fingerprinter::findProgram() { return QStandardPaths::findExecutable("fpcalc"); }

Fingerprinter::Result Fingerprinter::run() {
    Result result;
    QElapsedTimer timer;
    timer.start();
    if (m_program.isEmpty()) {
        result.ok = false;
        result.error = "fpcalc is not installed (it comes with the chromaprint package)";
        return result;
    }
    struct Work {
        qint64 id, size, mtimeNs;
        QString path;
    };
    QList<Work> work;
    {
        QSqlQuery q(m_library.db());
        q.setForwardOnly(true);
        q.exec("SELECT t.id, t.size, t.mtime_ns, v.root_path, t.rel_path FROM tracks t "
               "JOIN volumes v ON v.id = t.volume_id LEFT JOIN fingerprints f ON f.track_id = t.id "
               "WHERE v.online = 1 AND t.availability = 'online' AND "
               "(f.track_id IS NULL OR f.size <> t.size OR f.mtime_ns <> t.mtime_ns) ORDER BY t.id");
        while (q.next()) {
            const QString root = q.value(3).toString();
            const QString path = (root == "/" ? QString() : root) + '/' + q.value(4).toString();
            if (Journal::insideSandbox(m_sandbox, path))
                work << Work{q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(2).toLongLong(), path};
        }
    }
    result.total = work.size();
    const qint64 jobRow = Journal::startJobRow(m_library, "fingerprint", {});
    if (m_job)
        m_job->start();

    QSqlQuery store(m_library.db());
    store.prepare("INSERT INTO fingerprints (track_id, size, mtime_ns, duration, fingerprint, computed) "
                  "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(track_id) DO UPDATE SET size = excluded.size, "
                  "mtime_ns = excluded.mtime_ns, duration = excluded.duration, "
                  "fingerprint = excluded.fingerprint, computed = excluded.computed");
    // A few fpcalc processes at once, started and collected from this
    // thread: the processes are the parallel part.
    struct Running {
        std::unique_ptr<QProcess> process;
        Work work;
    };
    std::deque<Running> running;
    int next = 0, finished = 0;
    while (finished < work.size()) {
        while (int(running.size()) < m_parallel && next < work.size() && !(m_job && m_job->isCancelled())) {
            Running r{std::make_unique<QProcess>(), work[next++]};
            r.process->start(m_program, {"-raw", "-json", "-length", "120", r.work.path});
            running.push_back(std::move(r));
        }
        if (running.empty())
            break; // cancelled
        Running r = std::move(running.front());
        running.pop_front();
        const bool ok = r.process->waitForFinished(180000) && r.process->exitStatus() == QProcess::NormalExit &&
                        r.process->exitCode() == 0;
        const QJsonObject o = QJsonDocument::fromJson(r.process->readAllStandardOutput()).object();
        const QJsonArray values = o.value("fingerprint").toArray();
        if (!ok || values.isEmpty())
            ++result.failed;
        else
            ++result.computed;
        // A failure is stored too (empty), so the file isn't tried again until it changes.
        store.addBindValue(r.work.id);
        store.addBindValue(r.work.size);
        store.addBindValue(r.work.mtimeNs);
        store.addBindValue(o.value("duration").toDouble());
        store.addBindValue(ok && !values.isEmpty() ? Duplicates::pack(values) : QByteArray(""));
        store.addBindValue(QDateTime::currentSecsSinceEpoch());
        store.exec();
        ++finished;
        if (m_job)
            m_job->setProgress("Fingerprinting", finished, work.size());
    }
    result.cancelled = m_job && m_job->isCancelled();
    result.elapsedMs = timer.elapsed();
    Journal::finishJobRow(m_library, jobRow, result.ok, result.cancelled, result.toJson(), {});
    if (m_job)
        m_job->finish();
    return result;
}
