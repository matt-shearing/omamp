#include "tagreader.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tdebuglistener.h>
#include <taglib/tpropertymap.h>

TrackTags::TrackTags() : rgTrackGain(NAN), rgTrackPeak(NAN), rgAlbumGain(NAN), rgAlbumPeak(NAN) {}

namespace TagReader {

const QStringList &audioExtensions() {
    static const QStringList list = {"mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "mp4", "aac", "alac",
                                     "wav", "aif", "aiff", "ape", "wv", "wma", "asf", "mpc", "dsf", "dff",
                                     "mka", "spx", "tta"};
    return list;
}

bool isAudio(const QString &fileName) {
    const int dot = fileName.lastIndexOf('.');
    return dot > 0 && audioExtensions().contains(fileName.mid(dot + 1).toLower());
}

QStringList splitValues(const QStringList &values) {
    QStringList out;
    for (const QString &value : values) {
        for (const QString &part : value.split(';')) {
            const QString clean = part.trimmed();
            if (!clean.isEmpty() && !out.contains(clean, Qt::CaseInsensitive))
                out << clean;
        }
    }
    return out;
}

void parsePair(const QString &text, int *number, int *total) {
    const QStringList parts = text.trimmed().split('/');
    *number = parts.value(0).trimmed().toInt();
    *total = parts.size() > 1 ? parts.value(1).trimmed().toInt() : 0;
}

int parseYear(const QString &text) {
    static const QRegularExpression year("(\\d{4})");
    const auto match = year.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

double parseGain(const QString &text) {
    static const QRegularExpression number("[-+]?\\d+(\\.\\d+)?");
    const auto match = number.match(text);
    return match.hasMatch() ? match.captured(0).toDouble() : NAN;
}

QString guessContentType(const QString &extension, const QStringList &genres) {
    if (extension == "m4b")
        return "audiobook";
    for (const QString &genre : genres) {
        const QString g = genre.toLower();
        if (g.contains("audiobook") || g.contains("audio book") || g == "spoken word")
            return "audiobook";
        if (g.contains("podcast"))
            return "podcast";
        if (g == "classical" || g.startsWith("classical"))
            return "classical";
    }
    return "music";
}

// TagLib prints every parse problem to stderr. A library scan meets plenty
// of odd files; the scanner reports them itself, so TagLib's chatter is muted.
class QuietListener : public TagLib::DebugListener {
public:
    void printMessage(const TagLib::String &) override {}
};

static QString qs(const TagLib::String &s) { return QString::fromStdString(s.to8Bit(true)); }

static QStringList list(const TagLib::PropertyMap &map, const char *key) {
    QStringList out;
    const auto it = map.find(key);
    if (it != map.end())
        for (const TagLib::String &s : it->second)
            out << qs(s);
    return out;
}

static QString first(const TagLib::PropertyMap &map, const char *key) {
    const auto it = map.find(key);
    return it != map.end() && !it->second.isEmpty() ? qs(it->second.front()).trimmed() : QString();
}

TrackTags read(const QString &path) {
    TrackTags t;
    static QuietListener quiet;
    static const bool muted = (TagLib::setDebugListener(&quiet), true);
    Q_UNUSED(muted);
    const QByteArray encoded = QFile::encodeName(path);
    // Average style reads the header and, for VBR MP3s, the Xing frame,
    // without scanning the whole file.
    TagLib::FileRef file(encoded.constData(), true, TagLib::AudioProperties::Average);
    if (file.isNull()) {
        t.error = "Unreadable or unsupported file";
        return t;
    }
    if (TagLib::AudioProperties *audio = file.audioProperties()) {
        t.durationMs = audio->lengthInMilliseconds();
        t.bitrate = audio->bitrate();
        t.sampleRate = audio->sampleRate();
        t.channels = audio->channels();
    }
    // TagLib accepts some files with the right extension and no audio at all
    // (an MP3 with no frame); a file with no sample rate is not playable.
    if (t.sampleRate <= 0) {
        t.error = "No audio found in the file";
        return t;
    }
    t.ok = true;

    const TagLib::PropertyMap map = file.properties();
    t.title = first(map, "TITLE");
    t.album = first(map, "ALBUM");
    t.artists = splitValues(list(map, "ARTIST"));
    t.albumArtists = splitValues(list(map, "ALBUMARTIST"));
    t.composers = splitValues(list(map, "COMPOSER"));
    t.genres = splitValues(list(map, "GENRE"));
    t.comment = first(map, "COMMENT");
    t.grouping = first(map, "GROUPING");
    if (t.grouping.isEmpty())
        t.grouping = first(map, "CONTENTGROUP");
    t.publisher = first(map, "LABEL");
    if (t.publisher.isEmpty())
        t.publisher = first(map, "PUBLISHER");
    t.isrc = first(map, "ISRC");
    t.year = parseYear(first(map, "DATE"));
    if (!t.year)
        t.year = parseYear(first(map, "YEAR"));
    t.originalYear = parseYear(first(map, "ORIGINALDATE"));
    if (!t.originalYear)
        t.originalYear = parseYear(first(map, "ORIGINALYEAR"));
    parsePair(first(map, "TRACKNUMBER"), &t.trackNo, &t.trackTotal);
    parsePair(first(map, "DISCNUMBER"), &t.discNo, &t.discTotal);
    if (!t.trackTotal)
        t.trackTotal = first(map, "TRACKTOTAL").toInt() ?: first(map, "TOTALTRACKS").toInt();
    if (!t.discTotal)
        t.discTotal = first(map, "DISCTOTAL").toInt() ?: first(map, "TOTALDISCS").toInt();
    t.bpm = qRound(first(map, "BPM").toDouble());
    const QString compilation = first(map, "COMPILATION");
    t.compilation = compilation == "1" || compilation.compare("true", Qt::CaseInsensitive) == 0;
    t.mbTrackId = first(map, "MUSICBRAINZ_TRACKID");
    t.mbReleaseId = first(map, "MUSICBRAINZ_ALBUMID");
    t.mbArtistId = first(map, "MUSICBRAINZ_ARTISTID");
    t.rgTrackGain = parseGain(first(map, "REPLAYGAIN_TRACK_GAIN"));
    t.rgTrackPeak = parseGain(first(map, "REPLAYGAIN_TRACK_PEAK"));
    t.rgAlbumGain = parseGain(first(map, "REPLAYGAIN_ALBUM_GAIN"));
    t.rgAlbumPeak = parseGain(first(map, "REPLAYGAIN_ALBUM_PEAK"));

    t.contentType = guessContentType(QFileInfo(path).suffix().toLower(), t.genres);
    return t;
}

}
