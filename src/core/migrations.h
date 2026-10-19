#pragma once

#include <QList>
#include <QStringList>

// One step of the schema. Steps are never edited once released: a change to
// the schema is a new step, so every old database upgrades the same way.
struct Migration {
    int version;
    const char *description;
    QStringList statements;
};

const QList<Migration> &migrations();
