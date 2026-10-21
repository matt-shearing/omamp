#pragma once

#include <QString>
#include <QUrl>

// How the engines treat network streams (a Navidrome or other Subsonic
// server's stream URLs). The defaults suit a real server; the tests shorten
// the timeouts so a stalled fixture server fails in a second, not fifteen.
namespace Streaming {

struct Options {
    // A read or connect that waits this long fails, so a dropped stream
    // ends the track instead of hanging the decoder.
    int rwTimeoutMs = 15000;
    // FFmpeg reconnects a dropped connection (with a Range request where
    // the server allows it), waiting 0, 1, 2, 4 ... s between tries; it
    // gives up once a wait would pass this.
    int reconnectDelayMaxS = 4;
    // Read ahead on a thread of FFmpeg's own ("async:" protocol, 4 MB), so
    // a network hiccup is absorbed there rather than in the half-second
    // ring between decoder and sound card.
    bool readAhead = true;
};

Options options();
void setOptions(const Options &options);

// Whether a queue entry is a live radio station (radio://<key>): no length,
// no seeking, no read-ahead (so FFmpeg's HTTP layer can hand over the
// station's ICY titles), and reconnected when it drops.
inline bool isLive(const QString &path) { return path.startsWith(QLatin1String("radio://")); }

// "Thrum/<version>".
QString userAgent();

// What FFmpeg is asked to open for a location: the local path, or the URL
// (behind "async:" when reading ahead).
QByteArray ffmpegLocation(const QString &path, const QUrl &url);

} // namespace Streaming
