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
    // MusicBrainz IDs. mbTrackId is the recording ID (Picard's
    // MUSICBRAINZ_TRACKID), not the release track.
    QString mbTrackId, mbReleaseId, mbArtistId, mbReleaseGroupId, mbAlbumArtistId;

    // What the file says about itself beyond the text tags. tagRating is 0 to
    // 100 like the library's rating, -1 when the file has none; tagPlayCount
    // is -1 when absent. They only fill empty library values.
    int tagRating = -1, tagPlayCount = -1;
    bool hasEmbeddedArt = false, hasLyrics = false;

    // A hash of a slice of the file's bytes, for recognising a moved file.
    QString quickHash;

    qint64 durationMs = 0;
    int bitrate = 0, sampleRate = 0, channels = 0;

    // NaN when the tag is absent.
    double rgTrackGain, rgTrackPeak, rgAlbumGain, rgAlbumPeak;

    QString contentType = "music";

    TrackTags();
};

namespace TagReader {

// Which fields read() fills. Stored per track as tracks.tag_version, so a
// scan rereads rows that an older reader filled, once.
constexpr int version = 1;

// Reads tags and audio properties with TagLib. Safe to call from several
// threads at once: each call opens its own file object.
TrackTags read(const QString &path);

// The embedded front cover (or the first picture when none is marked as
// the front), or empty when the file has none.
QByteArray embeddedPicture(const QString &path, QString *mimeType = nullptr);

// SHA-1 of 64 KiB from the middle of the file, plus its size, as 16 hex
// digits. The middle avoids the tags at either end, so the hash holds for a
// moved file whose tags were also tidied in place. Empty if unreadable.
QString quickHash(const QString &path);

// Ratings in the conventions players write, as 0 to 100 (NULL is -1):
// POPM 0..255 (Windows Media Player and MediaMonkey steps), FMPS 0.0..1.0,
// and a plain RATING of 0..5 stars or 0..100.
int ratingFromPopm(int popm);
int ratingFromText(const QString &key, const QString &value);

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
