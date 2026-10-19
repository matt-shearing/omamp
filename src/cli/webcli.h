#pragma once

#include <QStringList>
#include <QTextStream>

class VolumeResolver;

// The Phase 4 commands: identify, fetch-art, lyrics, scrobble and net. They
// parse their own options, so the web's flags stay out of the other
// commands' way. Same rules as every command: --json, results on stdout,
// errors on stderr, exit 0 or 1.
namespace WebCli {

bool isCommand(const QString &first);
int run(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver = nullptr);
extern const char *usage;

}
