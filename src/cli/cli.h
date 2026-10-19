#pragma once

#include <QStringList>
#include <QTextStream>

class VolumeResolver;

// `omamp scan`, `omamp query`, `omamp stats`: the library without a window,
// for scripts and agents. Every command takes --json, prints results on
// stdout and errors on stderr, and exits 0 or 1.
namespace Cli {

// True when the first argument names a command. Anything else (no argument,
// files, folders) opens the window as before; `omamp open <file>` opens a
// file whose name happens to be a command word.
bool isCommand(const QString &first);

// Runs a command. arguments excludes the program name. The streams and the
// volume resolver are injectable so tests can run commands in-process.
int run(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver = nullptr);

// `omamp volume`, `omamp nas` and `omamp remote` (clinas.cpp); run() hands
// these over.
int runNas(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver = nullptr);
// What --password-stdin and --api-key-stdin read, for tests.
void setStdinForTesting(const QString &text);

}
