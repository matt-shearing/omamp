#pragma once

// Thrum was called Omamp until 0.4.0. On the first start after the rename,
// this moves Omamp's folders to Thrum's, once:
//
//   <config>/omamp -> <config>/thrum   (state.json, window.conf, spotify.conf,
//                                       credentials.json; omamp.conf becomes
//                                       thrum.conf)
//   <data>/omamp   -> <data>/thrum     (library.db with its -wal, -shm and
//                                       .bak files)
//   <cache>/omamp  -> <cache>/thrum    (artwork, radio and Spotify pictures,
//                                       the web cache), so nothing is fetched
//                                       again
//
// where <config>, <data> and <cache> follow XDG_CONFIG_HOME, XDG_DATA_HOME
// and XDG_CACHE_HOME the way the rest of the app does (QStandardPaths). Each
// folder is renamed in one step, so a library database always moves together
// with its -wal and -shm files. When a Thrum folder already exists it is kept
// and the Omamp one is left alone. When nothing of Omamp's is there, nothing
// happens.
//
// The library stores absolute paths into the artwork and radio caches; those
// that point into the old cache folder are rewritten to the new one.
//
// Keyring entries (application=omamp) are not touched here: Secrets::lookup
// reads the old entry when the new one is missing and copies it over.

#include <QString>
#include <QStringList>

namespace RenameMigration {

struct Roots {
    QString config; // e.g. ~/.config
    QString data;   // e.g. ~/.local/share
    QString cache;  // e.g. ~/.cache
};

// The parents QStandardPaths gives this process.
Roots defaultRoots();

struct Result {
    QStringList log;      // one line per thing done or left alone
    bool moved = false;   // at least one folder was moved
    bool blocked = false; // Omamp is still running, so nothing was moved
    QString blockedWhy;
};

// Moves what needs moving under roots. omampRunning, when set, says whether
// an Omamp process still has the files open; if it does and there is
// anything to move, nothing is moved and blocked is set.
Result migrate(const Roots &roots, bool omampRunning = false);

// The process id of a running Omamp ("omamp" in /proc/<pid>/comm), or 0.
qint64 runningOmampPid();

// Whether anything under roots still needs moving.
bool pending(const Roots &roots);

// For main(): migrate the default roots, logging each line to stderr. Returns
// false when Omamp is still running and the move had to wait; the message
// says what to do.
bool runAtStartup(QString *message);

} // namespace RenameMigration
