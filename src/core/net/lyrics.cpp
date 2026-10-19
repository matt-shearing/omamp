#include "lyrics.h"

#include "library.h"
#include "textmatch.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>

#include <taglib/fileref.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/synchronizedlyricsframe.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>

namespace Lyrics {

namespace {

QString qs(const TagLib::String &s) { return QString::fromStdString(s.to8Bit(true)); }

QString cleanText(QString text) {
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    return text.trimmed();
}

QVariant orNull(const QString &s) { return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s); }

} // namespace

// --- LRC ------------------------------------------------------------------

int Parsed::lineAt(qint64 positionMs) const {
    int found = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].timeMs < 0)
            continue;
        if (lines[i].timeMs <= positionMs)
            found = i;
        else
            break;
    }
    return found;
}

QString Parsed::plainText() const {
    QStringList out;
    for (const Line &l : lines)
        out << l.text;
    return out.join('\n').trimmed();
}

QString formatStamp(qint64 ms) {
    ms = qMax<qint64>(0, ms);
    return QString("[%1:%2.%3]")
        .arg(ms / 60000, 2, 10, QChar('0'))
        .arg((ms / 1000) % 60, 2, 10, QChar('0'))
        .arg((ms % 1000) / 10, 2, 10, QChar('0'));
}

Parsed parseLrc(const QString &textIn) {
    Parsed p;
    const QString text = cleanText(textIn);
    if (text.isEmpty())
        return p;
    static const QRegularExpression stamp(R"(^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])");
    static const QRegularExpression idTag(R"(^\[([A-Za-z#]+):(.*)\]\s*$)");
    static const QRegularExpression wordStamp(R"(<\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?>)");

    struct Raw {
        qint64 time;
        QString text;
        int order;
    };
    QList<Raw> timed;
    QStringList untimed;
    int order = 0;
    for (const QString &rawLine : text.split('\n')) {
        QString line = rawLine.trimmed();
        QList<qint64> times;
        while (true) {
            const auto m = stamp.match(line);
            if (!m.hasMatch())
                break;
            const qint64 minutes = m.captured(1).toLongLong();
            const qint64 seconds = m.captured(2).toLongLong();
            const QString fraction = m.captured(3);
            qint64 ms = 0;
            if (fraction.size() == 1)
                ms = fraction.toLongLong() * 100;
            else if (fraction.size() == 2)
                ms = fraction.toLongLong() * 10;
            else if (fraction.size() == 3)
                ms = fraction.toLongLong();
            times << minutes * 60000 + seconds * 1000 + ms;
            line = line.mid(m.capturedLength());
        }
        if (times.isEmpty()) {
            const auto tag = idTag.match(line);
            if (tag.hasMatch()) {
                const QString key = tag.captured(1).toLower();
                p.tags.insert(key, tag.captured(2).trimmed());
                if (key == "offset")
                    p.offsetMs = tag.captured(2).trimmed().toLongLong();
                continue;
            }
            untimed << rawLine.trimmed();
            continue;
        }
        line.remove(wordStamp);
        line = line.trimmed();
        for (qint64 t : std::as_const(times))
            timed.append({t, line, order++});
    }
    if (timed.isEmpty()) {
        // Plain lyrics: keep the lines, blank ones included, as written.
        while (!untimed.isEmpty() && untimed.last().isEmpty())
            untimed.removeLast();
        for (const QString &l : std::as_const(untimed))
            p.lines.append({-1, l});
        return p;
    }
    std::stable_sort(timed.begin(), timed.end(), [](const Raw &a, const Raw &b) {
        return a.time != b.time ? a.time < b.time : a.order < b.order;
    });
    // A positive offset shows the lyrics sooner (the LRC convention).
    for (const Raw &r : std::as_const(timed))
        p.lines.append({qMax<qint64>(0, r.time - p.offsetMs), r.text});
    p.synced = true;
    return p;
}

QString toLrc(const Parsed &parsed) {
    QStringList out;
    for (auto it = parsed.tags.constBegin(); it != parsed.tags.constEnd(); ++it)
        if (it.key() != "offset")
            out << QString("[%1:%2]").arg(it.key(), it.value());
    for (const Line &l : parsed.lines)
        out << (l.timeMs >= 0 ? formatStamp(l.timeMs) + l.text : l.text);
    return out.join('\n');
}

// --- Entries --------------------------------------------------------------

QJsonObject Entry::toJson() const {
    QJsonObject o{{"track_id", trackId},       {"source", source},
                  {"found", found()},          {"instrumental", instrumental},
                  {"has_synced", !synced.isEmpty()}, {"plain", plain},
                  {"synced", synced}};
    if (lrclibId)
        o["lrclib_id"] = lrclibId;
    if (fetched)
        o["fetched"] = fetched;
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QString sidecarPath(const QString &audioPath) {
    const QFileInfo info(audioPath);
    return info.absolutePath() + '/' + info.completeBaseName() + ".lrc";
}

Entry local(const QString &audioPath) {
    Entry e;
    if (audioPath.isEmpty())
        return e;
    QFile sidecar(sidecarPath(audioPath));
    if (sidecar.open(QIODevice::ReadOnly)) {
        const QString text = cleanText(QString::fromUtf8(sidecar.readAll()));
        const Parsed parsed = parseLrc(text);
        if (!parsed.isEmpty()) {
            e.source = "sidecar";
            if (parsed.synced)
                e.synced = text;
            e.plain = parsed.plainText();
            return e;
        }
    }

    const QByteArray encoded = QFile::encodeName(audioPath);
    TagLib::FileStream stream(encoded.constData(), true);
    if (!stream.isOpen())
        return e;
    TagLib::FileRef file(&stream, false);
    if (file.isNull())
        return e;
    // SYLT, in milliseconds: a synced lyrics frame in an MP3.
    if (auto *mpeg = dynamic_cast<TagLib::MPEG::File *>(file.file()); mpeg && mpeg->hasID3v2Tag()) {
        for (TagLib::ID3v2::Frame *frame : mpeg->ID3v2Tag()->frameList("SYLT")) {
            auto *sylt = dynamic_cast<TagLib::ID3v2::SynchronizedLyricsFrame *>(frame);
            if (!sylt || sylt->timestampFormat() != TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds)
                continue;
            Parsed parsed;
            for (const auto &t : sylt->synchedText())
                parsed.lines.append({qint64(t.time), qs(t.text).trimmed()});
            if (parsed.lines.isEmpty())
                continue;
            parsed.synced = true;
            e.source = "embedded";
            e.synced = toLrc(parsed);
            e.plain = parsed.plainText();
            break;
        }
    }
    QString text;
    const TagLib::PropertyMap map = file.properties();
    for (const auto &entry : map) {
        if ((entry.first == "LYRICS" || entry.first.startsWith("LYRICS:") || entry.first == "UNSYNCEDLYRICS") &&
            !entry.second.isEmpty()) {
            text = cleanText(qs(entry.second.front()));
            if (!text.isEmpty())
                break;
        }
    }
    if (!text.isEmpty()) {
        const Parsed parsed = parseLrc(text);
        e.source = "embedded";
        // Some taggers store LRC text in the plain lyrics field.
        if (parsed.synced && e.synced.isEmpty())
            e.synced = text;
        e.plain = parsed.synced ? parsed.plainText() : text;
    }
    return e;
}

Entry cached(Library &library, qint64 trackId) {
    Entry e;
    e.trackId = trackId;
    QSqlQuery q(library.db());
    q.prepare("SELECT plain, synced, source, lrclib_id, instrumental, fetched FROM lyrics WHERE track_id = ?");
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        e.plain = q.value(0).toString();
        e.synced = q.value(1).toString();
        e.source = q.value(2).toString();
        e.lrclibId = q.value(3).toLongLong();
        e.instrumental = q.value(4).toBool();
        e.fetched = q.value(5).toLongLong();
    }
    return e;
}

bool store(Library &library, const Entry &e) {
    QSqlQuery q(library.db());
    q.prepare("INSERT INTO lyrics (track_id, plain, synced, source, lrclib_id, instrumental, fetched) "
              "VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT(track_id) DO UPDATE SET plain = excluded.plain, "
              "synced = excluded.synced, source = excluded.source, lrclib_id = excluded.lrclib_id, "
              "instrumental = excluded.instrumental, fetched = excluded.fetched");
    q.addBindValue(e.trackId);
    q.addBindValue(orNull(e.plain));
    q.addBindValue(orNull(e.synced));
    q.addBindValue(e.source.isEmpty() ? QString("none") : e.source);
    q.addBindValue(e.lrclibId ? QVariant(e.lrclibId) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(e.instrumental ? 1 : 0);
    q.addBindValue(e.fetched ? e.fetched : QDateTime::currentSecsSinceEpoch());
    return q.exec();
}

Query queryFor(Library &library, qint64 trackId) {
    Query query;
    QSqlQuery q(library.db());
    q.prepare("SELECT artist, title, album, duration_ms FROM tracks WHERE id = ?");
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        // "A; B" is Omamp's multi-value form; LRCLIB credits "A, B".
        query.artist = q.value(0).toString().replace("; ", ", ");
        query.title = q.value(1).toString();
        query.album = q.value(2).toString();
        query.durationMs = q.value(3).toLongLong();
    }
    return query;
}

namespace {

Entry fromLrclib(const QJsonObject &o) {
    Entry e;
    e.source = "lrclib";
    e.lrclibId = qint64(o.value("id").toDouble());
    e.instrumental = o.value("instrumental").toBool();
    e.plain = cleanText(o.value("plainLyrics").toString());
    e.synced = cleanText(o.value("syncedLyrics").toString());
    if (e.plain.isEmpty() && !e.synced.isEmpty())
        e.plain = parseLrc(e.synced).plainText();
    e.fetched = QDateTime::currentSecsSinceEpoch();
    return e;
}

} // namespace

Entry fetchLrclib(Net::HttpClient &http, const Query &query) {
    Entry e;
    if (query.title.isEmpty() || query.artist.isEmpty()) {
        e.error = "LRCLIB needs a title and an artist";
        return e;
    }
    const QString base = Net::baseUrl(Net::Service::Lrclib, http.library());
    const int seconds = int(qRound(query.durationMs / 1000.0));
    QList<QPair<QString, QString>> params = {{"artist_name", query.artist}, {"track_name", query.title}};
    if (!query.album.isEmpty())
        params.append(QPair<QString, QString>("album_name", query.album));
    if (seconds > 0)
        params.append(QPair<QString, QString>("duration", QString::number(seconds)));
    constexpr int cacheSeconds = 7 * 86400;
    const Net::Response got = http.get(Net::Service::Lrclib, Net::makeUrl(base, "/get", params), cacheSeconds);
    if (got.ok() && got.json().isObject())
        return fromLrclib(got.json().object());
    if (!got.notFound()) {
        e.error = got.offline ? got.error : QString("LRCLIB: %1").arg(got.error);
        return e;
    }

    // Nothing exact: search, and keep the best by length and words.
    const Net::Response found = http.get(
        Net::Service::Lrclib,
        Net::makeUrl(base, "/search", {{"track_name", query.title}, {"artist_name", query.artist}}), cacheSeconds);
    if (!found.ok()) {
        e.error = QString("LRCLIB: %1").arg(found.error);
        return e;
    }
    double bestScore = 0;
    QJsonObject best;
    for (const QJsonValue &v : found.json().array()) {
        const QJsonObject o = v.toObject();
        const qint64 lengthMs = qint64(o.value("duration").toDouble() * 1000);
        if (query.durationMs > 0 && lengthMs > 0 && qAbs(lengthMs - query.durationMs) > 5000)
            continue;
        double score = 0.45 * TextMatch::similarity(o.value("trackName").toString(), query.title) +
                       0.35 * TextMatch::similarity(o.value("artistName").toString(), query.artist) +
                       0.20 * TextMatch::durationSimilarity(lengthMs, query.durationMs);
        if (!o.value("syncedLyrics").toString().isEmpty())
            score += 0.05; // synced lyrics are worth more to the lyrics view
        if (score > bestScore) {
            bestScore = score;
            best = o;
        }
    }
    if (best.isEmpty() || bestScore < 0.7) {
        e.source = "none";
        e.fetched = QDateTime::currentSecsSinceEpoch();
        e.error = "LRCLIB has no lyrics for this track";
        return e;
    }
    return fromLrclib(best);
}

Entry forTrack(Library &library, Net::HttpClient *http, qint64 trackId, const Options &options) {
    const QString path = library.absolutePath(trackId);
    Entry e = local(path);
    e.trackId = trackId;
    if (e.found() && !options.refresh)
        return e;

    Entry c = cached(library, trackId);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const bool cachedAnswer = !c.source.isEmpty();
    const bool staleNone = c.source == "none" && now - c.fetched > qint64(options.retryNotFoundDays) * 86400;
    if (cachedAnswer && !options.refresh && !staleNone) {
        if (!c.found())
            c.error = "LRCLIB had no lyrics for this track when last asked";
        return c;
    }
    if (!options.fetch || !http) {
        if (cachedAnswer)
            return c;
        e.error = "No lyrics on disk or in the cache";
        return e;
    }
    Entry f = fetchLrclib(*http, queryFor(library, trackId));
    f.trackId = trackId;
    if (f.source == "lrclib" || f.source == "none")
        store(library, f);
    else if (cachedAnswer)
        return c; // offline or an error: what we had is still the best answer
    return f;
}

WriteResult write(Library &library, const QList<Entry> &entries, const QString &mode, bool syncedInTag, bool dryRun,
                  Job *job, const QString &sandbox) {
    WriteResult result;
    if (mode != "lrc" && mode != "tag") {
        result.error = QString("Lyrics can be written as \"lrc\" (a .lrc file beside the track) or \"tag\" (inside "
                               "the file), not \"%1\"")
                           .arg(mode);
        return result;
    }
    QList<TagEdit> edits;
    for (const Entry &e : entries) {
        if (mode == "lrc") {
            // An .lrc file holds timed lines; plain words belong in the tag.
            if (e.synced.isEmpty() || e.source == "sidecar") {
                result.skipped << e.trackId;
                continue;
            }
            edits << TagEdit{e.trackId, "lrcfile", e.synced, {}, {}};
        } else {
            const QString text = syncedInTag && !e.synced.isEmpty() ? e.synced : e.plain;
            if (text.isEmpty() || e.source == "embedded") {
                result.skipped << e.trackId;
                continue;
            }
            edits << TagEdit{e.trackId, "lyrics", text, {}, {}};
        }
    }
    if (edits.isEmpty())
        return result;
    TagWriter writer(library, job);
    writer.setSandbox(sandbox);
    TagWriteOptions options;
    options.dryRun = dryRun;
    options.source = "lyrics";
    result.tags = writer.apply(edits, options);
    return result;
}

}
