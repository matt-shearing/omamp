#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

class Library;

// Reports: a list of tracks written out as CSV (for a spreadsheet) or as a
// self-contained HTML page (to read, print or send). MediaMonkey's "Export
// to CSV/HTML" reports, for any list Thrum can show: a query, a playlist, an
// auto-playlist, a collection or whatever the window is showing.
namespace Report {

struct Column {
    QString id, title;
};

// Every column a report can have, in a sensible order.
QList<Column> columns();
// title, artist, album, track, year, genre, length, rating.
QStringList defaultColumns();

struct Options {
    QString format = "csv"; // csv or html
    QStringList columns;    // empty: defaultColumns()
    QString title;          // the HTML page's heading; the CSV ignores it
};

struct Output {
    QByteArray data;
    int tracks = 0;
    qint64 durationMs = 0, bytes = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// The report for these tracks, in this order.
Output render(Library &library, const QList<qint64> &trackIds, const Options &options);
// Renders and writes it (beside the file, then renamed into place).
Output write(Library &library, const QList<qint64> &trackIds, const Options &options, const QString &file);

}
