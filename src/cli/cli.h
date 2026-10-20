#pragma once

#include <QStringList>
#include <QTextStream>
#include <functional>

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
// `omamp sync` and `omamp report` (clisync.cpp).
int runSync(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver = nullptr);
// `omamp spotify` (clispotify.cpp).
int runSpotify(const QStringList &arguments, QTextStream &out, QTextStream &err);
extern const char *spotifyUsage;
// The name, artists, album and length of Spotify tracks named by
// spotify://track/<id> paths, for `omamp playlist add`. Asks spotify_player
// only when Spotify is switched on in Omamp and spotify_player is signed in
// and running; otherwise (and for any it can't find) the result holds just
// the path. *note says why names are missing, when they are.
struct SpotifyTrackInfo {
    QString path, title, artist, album, albumId;
    qint64 durationMs = 0;
};
QList<SpotifyTrackInfo> lookUpSpotifyTracks(const QStringList &paths, QString *note);
// `omamp radio` (cliradio.cpp).
int runRadio(const QStringList &arguments, QTextStream &out, QTextStream &err);
// How `omamp radio play` reaches the player: hands radio://<key> to the
// running Omamp over MPRIS, or starts it. Tests put a fake in its place.
// how says which ("running", "started") or why it failed.
using RadioLauncher = std::function<bool(const QString &uri, QString *how)>;
void setRadioLauncher(RadioLauncher launcher);
// What --password-stdin and --api-key-stdin read, for tests.
void setStdinForTesting(const QString &text);

}
