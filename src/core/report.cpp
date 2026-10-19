#include "report.h"

#include "library.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QLocale>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace Report {

namespace {

// Each column's SQL over `tracks t JOIN volumes v`, in columns() order.
struct Def {
    const char *id, *title, *sql;
};

const Def defs[] = {
    {"title", "Title", "t.title"},
    {"artist", "Artist", "t.artist"},
    {"albumartist", "Album Artist", "t.album_artist"},
    {"album", "Album", "t.album"},
    {"disc", "Disc", "t.disc_no"},
    {"track", "Track", "t.track_no"},
    {"year", "Year", "t.year"},
    {"genre", "Genre", "t.genre"},
    {"composer", "Composer", "t.composer"},
    {"length", "Length", "t.duration_ms"},
    {"rating", "Rating", "t.rating"},
    {"plays", "Plays", "t.play_count"},
    {"lastplayed", "Last Played", "t.last_played"},
    {"added", "Added", "t.added"},
    {"format", "Format", "t.format"},
    {"bitrate", "Bitrate", "t.bitrate"},
    {"size", "Size", "t.size"},
    {"path", "Path", "v.root_path || '/' || t.rel_path"},
};

const Def *defFor(const QString &id) {
    for (const Def &d : defs)
        if (id == QLatin1String(d.id))
            return &d;
    return nullptr;
}

QString duration(qint64 ms) {
    const qint64 s = (ms + 500) / 1000;
    if (s >= 3600)
        return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString cell(const QString &id, const QVariant &v) {
    if (v.isNull())
        return {};
    if (id == "length")
        return duration(v.toLongLong());
    if (id == "rating") // 0..100 to stars, as the window shows them
        return QString::number(v.toInt() / 20.0, 'g', 2);
    if (id == "lastplayed" || id == "added")
        return v.toLongLong() > 0 ? QDateTime::fromSecsSinceEpoch(v.toLongLong()).toString("yyyy-MM-dd HH:mm") : QString();
    if ((id == "disc" || id == "track" || id == "year" || id == "bitrate") && v.toLongLong() == 0)
        return {};
    return v.toString();
}

QString csvField(const QString &s) {
    // Spreadsheets run a cell starting with = + - @ as a formula; a leading
    // apostrophe keeps it text (OWASP's advice for CSV injection).
    QString v = s;
    if (!v.isEmpty() && QStringLiteral("=+-@\t\r").contains(v.front()) && !(v.front() == '-' && v.size() > 1 && v[1].isDigit()))
        v.prepend('\'');
    if (v.contains(',') || v.contains('"') || v.contains('\n') || v.contains('\r'))
        return '"' + QString(v).replace('"', "\"\"") + '"';
    return v;
}

QString bytesText(qint64 b) {
    if (b >= 1024LL * 1024 * 1024)
        return QString::number(b / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
    return QString::number(b / (1024.0 * 1024), 'f', 1) + " MB";
}

const char *const htmlStyle = R"(
:root { --bg: #fdfdfc; --fg: #1d1d1b; --muted: #6b6b66; --line: #e3e3de; --stripe: #f4f4f1; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #161615; --fg: #e8e8e3; --muted: #9a9a93; --line: #2e2e2b; --stripe: #1e1e1c; }
}
body { background: var(--bg); color: var(--fg); font: 14px/1.4 system-ui, sans-serif; margin: 24px; }
h1 { font-size: 20px; margin: 0 0 4px; }
p.summary { color: var(--muted); margin: 0 0 16px; }
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; padding: 4px 10px 4px 0; border-bottom: 1px solid var(--line); vertical-align: top; }
th { position: sticky; top: 0; background: var(--bg); font-weight: 600; }
tbody tr:nth-child(even) { background: var(--stripe); }
td.n, th.n { text-align: right; font-variant-numeric: tabular-nums; }
td.path { color: var(--muted); word-break: break-all; }
@media print { th { position: static; } body { margin: 0; } }
)";

bool numeric(const QString &id) {
    return id == "disc" || id == "track" || id == "year" || id == "length" || id == "rating" || id == "plays" ||
           id == "bitrate" || id == "size";
}

} // namespace

QList<Column> columns() {
    QList<Column> out;
    for (const Def &d : defs)
        out << Column{d.id, d.title};
    return out;
}

QStringList defaultColumns() { return {"title", "artist", "album", "track", "year", "genre", "length", "rating"}; }

Output render(Library &library, const QList<qint64> &trackIds, const Options &options) {
    Output out;
    const QString format = options.format.toLower();
    if (format != "csv" && format != "html") {
        out.error = QString("A report is csv or html, not \"%1\"").arg(options.format);
        return out;
    }
    const QStringList ids = options.columns.isEmpty() ? defaultColumns() : options.columns;
    QStringList select = {"t.id", "t.duration_ms", "t.size"};
    for (const QString &id : ids) {
        const Def *d = defFor(id.trimmed().toLower());
        if (!d) {
            QStringList known;
            for (const Def &k : defs)
                known << k.id;
            out.error = QString("No report column \"%1\"; the columns are %2").arg(id, known.join(", "));
            return out;
        }
        select << d->sql;
    }

    // Rows by id, then in the order asked for.
    QHash<qint64, QStringList> rows;
    QHash<qint64, QPair<qint64, qint64>> sizes;
    for (qsizetype from = 0; from < trackIds.size(); from += 500) {
        QStringList chunk;
        for (qsizetype k = from; k < std::min<qsizetype>(from + 500, trackIds.size()); ++k)
            chunk << QString::number(trackIds[k]);
        QSqlQuery q(library.db());
        // The ids are integers from the database, so they go in as text.
        if (!q.exec("SELECT " + select.join(", ") +
                    " FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id IN (" + chunk.join(',') + ")")) {
            out.error = q.lastError().text();
            return out;
        }
        while (q.next()) {
            QStringList cells;
            for (int c = 0; c < ids.size(); ++c)
                cells << cell(ids[c].trimmed().toLower(), q.value(3 + c));
            rows.insert(q.value(0).toLongLong(), cells);
            sizes.insert(q.value(0).toLongLong(), {q.value(1).toLongLong(), q.value(2).toLongLong()});
        }
    }

    QStringList titles;
    for (const QString &id : ids)
        titles << defFor(id.trimmed().toLower())->title;

    if (format == "csv") {
        QString text;
        QStringList head;
        for (const QString &t : titles)
            head << csvField(t);
        text += head.join(',') + "\r\n";
        for (qint64 id : trackIds) {
            if (!rows.contains(id))
                continue;
            QStringList line;
            for (const QString &c : rows.value(id))
                line << csvField(c);
            text += line.join(',') + "\r\n";
            ++out.tracks;
            out.durationMs += sizes.value(id).first;
            out.bytes += sizes.value(id).second;
        }
        out.data = text.toUtf8();
        return out;
    }

    QString body;
    for (qint64 id : trackIds) {
        if (!rows.contains(id))
            continue;
        const QStringList cells = rows.value(id);
        body += "<tr>";
        for (int c = 0; c < cells.size(); ++c) {
            const QString cid = ids[c].trimmed().toLower();
            const QString cls = numeric(cid) ? " class=\"n\"" : cid == "path" ? " class=\"path\"" : QString();
            body += "<td" + cls + ">" + cells[c].toHtmlEscaped() + "</td>";
        }
        body += "</tr>\n";
        ++out.tracks;
        out.durationMs += sizes.value(id).first;
        out.bytes += sizes.value(id).second;
    }
    QString head;
    for (int c = 0; c < titles.size(); ++c)
        head += QString("<th%1>%2</th>").arg(numeric(ids[c].trimmed().toLower()) ? " class=\"n\"" : "", titles[c].toHtmlEscaped());
    const QString title = options.title.isEmpty() ? QString("Tracks") : options.title;
    const QString summary = QString("%1 %2 · %3 · %4 · made by Omamp on %5")
                                .arg(out.tracks)
                                .arg(out.tracks == 1 ? "track" : "tracks")
                                .arg(duration(out.durationMs), bytesText(out.bytes),
                                     QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"));
    const QString page = QString("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                                 "<title>%1</title>\n<style>%2</style>\n</head>\n<body>\n<h1>%1</h1>\n"
                                 "<p class=\"summary\">%3</p>\n<table>\n<thead><tr>%4</tr></thead>\n<tbody>\n%5"
                                 "</tbody>\n</table>\n</body>\n</html>\n")
                             .arg(title.toHtmlEscaped(), QString::fromLatin1(htmlStyle), summary.toHtmlEscaped(), head, body);
    out.data = page.toUtf8();
    return out;
}

Output write(Library &library, const QList<qint64> &trackIds, const Options &options, const QString &file) {
    Output out = render(library, trackIds, options);
    if (!out.ok())
        return out;
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly) || f.write(out.data) != out.data.size() || !f.commit())
        out.error = QString("Couldn't write %1: %2").arg(file, f.errorString());
    return out;
}

}
