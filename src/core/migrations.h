#pragma once

#include <QList>
#include <QStringList>

// One step of the schema. Steps are never edited once released: a change to
// the schema is a new step, so every old database upgrades the same way.
struct Migration {
    int version;
    const char *description;
    QStringList statements;
    // True when the step rebuilds a table that others reference. It then
    // runs with foreign keys off (dropping the old table would otherwise
    // cascade into its children), and PRAGMA foreign_key_check must come back
    // empty before it commits. SQLite's documented way to change a table.
    bool rebuildsTables = false;
};

const QList<Migration> &migrations();
