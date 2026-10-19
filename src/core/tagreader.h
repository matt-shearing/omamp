#pragma once

#include <QString>
#include <QStringList>

// Everything the scanner reads from one file. Multi-value fields keep each
// value separately; the joined text ("A; B") is what the track row shows.
struct TrackTags {
    bool ok = false;
    QString error;

    QString title, album, comment, grouping, publisher, isrc;
    QStringList artists, albumArtists, composers, genres;
    int year = 0, originalYear = 0;
    int trackNo = 0, trackTotal = 0, discNo = 0, discTotal = 0;
    int bpm = 0;
    bool compilation = false;
    QString mbTrackId, mbReleaseId, mbArtistId;

    qint64 durationMs = 0;
    int bitrate = 0, sampleRate = 0, channels = 0;

    // NaN when the tag is absent.
    double rgTrackGain, rgTrackPeak, rgAlbumGain, rgAlbumPeak;

    QString contentType = "music";

    TrackTags();
};

namespace TagReader {

// Reads tags and audio properties with TagLib. Safe to call from several
// threads at once: each call opens its own file object.
TrackTags read(const QString &path);

// The extensions the scanner considers audio, lower case.
const QStringList &audioExtensions();
bool isAudio(const QString &fileName);

// "A; B" and "A;B" become two values, the separator MediaMonkey writes.
// Ampersands and "feat." are left alone: they are part of names like
// "Simon & Garfunkel".
QStringList splitValues(const QStringList &values);

// "3/12" gives 3 and 12; "03" gives 3 and 0.
void parsePair(const QString &text, int *number, int *total);
int parseYear(const QString &text);
double parseGain(const QString &text);

// Music, audiobook, podcast or classical, guessed as MediaMonkey does from
// the extension and the genre.
QString guessContentType(const QString &extension, const QStringList &genres);

}
