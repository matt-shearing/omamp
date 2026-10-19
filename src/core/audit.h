#pragma once

#include "query.h"

#include <QJsonObject>
#include <QList>
#include <QString>

class Library;

// Files to Edit: built-in checks for tracks whose tags or files need work,
// MediaMonkey's "Files to Edit" nodes. Each check is either a query in the
// query language or, where the language can't say it (an album whose
// tracks disagree), a SQL condition over `tracks t`. Both compile to the
// same thing, a Query::Compiled, so the window's track list can show any
// check as it shows a search.
namespace Audits {

struct Audit {
    QString id;          // "unknown-artist"
    QString title;       // "Unknown artist", for the tree
    QString group;       // "Unknown", "Albums", "Files": a tree folder
    QString description; // one sentence for a tooltip
    QString query;       // query-language text, or empty when sql is used
    QString sql;         // a condition over `tracks t`, when query is empty
    QJsonObject toJson() const;
};

// Every check, in the order the tree shows them.
const QList<Audit> &audits();
const Audit *audit(const QString &id);

// The check as a condition the track list can use, ordered as the library.
Query::Compiled compile(const Audit &audit);

// How many tracks a check finds, and their ids (in library order, up to
// limit; 0 means all).
qint64 count(Library &library, const QString &id, QString *error = nullptr);
QList<qint64> ids(Library &library, const QString &id, int limit = 0, QString *error = nullptr);

struct Count {
    Audit audit;
    qint64 count = 0;
};
// Every check with its count: the numbers beside the tree nodes.
QList<Count> counts(Library &library);

}
