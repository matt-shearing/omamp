#include "tagreader.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

#include <QCryptographicHash>
#include <climits>

#include <taglib/aifffile.h>
#include <taglib/audioproperties.h>
#include <taglib/dsffile.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/popularimeterframe.h>
#include <taglib/tdebuglistener.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/unknownframe.h>
#include <taglib/wavfile.h>

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

static void muteTagLib() {
    static QuietListener quiet;
    static const bool muted = (TagLib::setDebugListener(&quiet), true);
    Q_UNUSED(muted);
}

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

int ratingFromPopm(int popm) {
    if (popm <= 0)
        return -1; // 0 is "unrated" in POPM
    // MediaMonkey writes these for half stars.
    switch (popm) {
    case 13: return 10;
    case 54: return 30;
    case 118: return 50;
    case 186: return 70;
    case 242: return 90;
    }
    // Windows Media Player writes 1, 64, 128, 196 and 255; others write
    // something near. These ranges are the ones most players read back.
    if (popm < 32) return 20;
    if (popm < 96) return 40;
    if (popm < 160) return 60;
    if (popm < 224) return 80;
    return 100;
}

int ratingFromText(const QString &key, const QString &value) {
    bool ok = false;
    const double v = value.trimmed().toDouble(&ok);
    if (!ok || v < 0)
        return -1;
    if (key.compare("FMPS_RATING", Qt::CaseInsensitive) == 0)
        return v <= 1.0 ? qRound(v * 100) : -1;
    // A plain RATING: stars (0..5), a percentage, or a POPM byte. Zero reads
    // as unrated, since that is what most writers mean by it.
    if (v == 0)
        return -1;
    if (v <= 5)
        return qRound(v * 20);
    if (v <= 100)
        return qRound(v);
    if (v <= 255)
        return ratingFromPopm(qRound(v));
    return -1;
}

QString quickHash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = file.size();
    constexpr qint64 slice = 64 * 1024;
    const qint64 offset = qMax<qint64>(0, size / 2 - slice / 2);
    if (!file.seek(offset))
        return {};
    const QByteArray bytes = file.read(slice);
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArray::number(size));
    hash.addData(bytes);
    return QString::fromLatin1(hash.result().toHex().left(16));
}

// The ID3v2 tag of any format that can carry one, for the frames TagLib's
// property map leaves out (POPM, PCNT).
static TagLib::ID3v2::Tag *id3v2Of(TagLib::File *file) {
    if (auto *mpeg = dynamic_cast<TagLib::MPEG::File *>(file))
        return mpeg->hasID3v2Tag() ? mpeg->ID3v2Tag() : nullptr;
    if (auto *flac = dynamic_cast<TagLib::FLAC::File *>(file))
        return flac->hasID3v2Tag() ? flac->ID3v2Tag() : nullptr;
    if (auto *wav = dynamic_cast<TagLib::RIFF::WAV::File *>(file))
        return wav->hasID3v2Tag() ? wav->ID3v2Tag() : nullptr;
    if (auto *aiff = dynamic_cast<TagLib::RIFF::AIFF::File *>(file))
        return aiff->hasID3v2Tag() ? aiff->tag() : nullptr;
    if (auto *dsf = dynamic_cast<TagLib::DSF::File *>(file))
        return dsf->tag();
    return nullptr;
}

static void readPopularity(TagLib::ID3v2::Tag *tag, TrackTags &t) {
    if (!tag)
        return;
    const auto &frames = tag->frameListMap();
    const auto popm = frames.find("POPM");
    if (popm != frames.end()) {
        for (TagLib::ID3v2::Frame *frame : popm->second) {
            auto *p = dynamic_cast<TagLib::ID3v2::PopularimeterFrame *>(frame);
            if (!p)
                continue;
            if (t.tagRating < 0)
                t.tagRating = ratingFromPopm(p->rating());
            if (int(p->counter()) > t.tagPlayCount)
                t.tagPlayCount = int(p->counter());
        }
    }
    // PCNT has no class of its own in TagLib: a big-endian counter of at
    // least four bytes.
    const auto pcnt = frames.find("PCNT");
    if (pcnt != frames.end() && !pcnt->second.isEmpty()) {
        if (auto *u = dynamic_cast<TagLib::ID3v2::UnknownFrame *>(pcnt->second.front())) {
            const TagLib::ByteVector data = u->data();
            if (data.size() >= 4) {
                const qint64 count = qint64(data.toLongLong(true));
                if (count >= 0 && count < INT_MAX && count > t.tagPlayCount)
                    t.tagPlayCount = int(count);
            }
        }
    }
}

QByteArray embeddedPicture(const QString &path, QString *mimeType) {
    muteTagLib();
    const QByteArray encoded = QFile::encodeName(path);
    TagLib::FileStream stream(encoded.constData(), true);
    TagLib::FileRef file(&stream, false);
    if (file.isNull())
        return {};
    const TagLib::List<TagLib::VariantMap> pictures = file.complexProperties("PICTURE");
    const TagLib::VariantMap *chosen = nullptr;
    for (const TagLib::VariantMap &p : pictures) {
        if (!chosen)
            chosen = &p;
        if (p.value("pictureType").toString() == "Front Cover") {
            chosen = &p;
            break;
        }
    }
    if (!chosen)
        return {};
    const TagLib::ByteVector data = chosen->value("data").toByteVector();
    if (mimeType)
        *mimeType = qs(chosen->value("mimeType").toString());
    return QByteArray(data.data(), int(data.size()));
}

TrackTags read(const QString &path) {
    TrackTags t;
    muteTagLib();
    const QByteArray encoded = QFile::encodeName(path);
    // Read-only: TagLib otherwise opens files for writing when it can, which
    // the folder watcher sees as a change (and a NAS as a write). Average
    // style reads the header and, for VBR MP3s, the Xing frame, without
    // scanning the whole file. The stream must outlive the FileRef.
    TagLib::FileStream stream(encoded.constData(), true);
    if (!stream.isOpen()) {
        t.error = "Unreadable file";
        return t;
    }
    TagLib::FileRef file(&stream, true, TagLib::AudioProperties::Average);
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

    t.mbReleaseGroupId = first(map, "MUSICBRAINZ_RELEASEGROUPID");
    t.mbAlbumArtistId = first(map, "MUSICBRAINZ_ALBUMARTISTID");

    // Ratings and play counts: text tags first (Vorbis comments, TXXX, MP4
    // freeform), then ID3v2's POPM and PCNT frames.
    for (const char *key : {"FMPS_RATING", "RATING"}) {
        const QString value = first(map, key);
        if (t.tagRating < 0 && !value.isEmpty())
            t.tagRating = ratingFromText(key, value);
    }
    for (const char *key : {"FMPS_PLAYCOUNT", "PLAYCOUNT", "PLAY_COUNT"}) {
        bool ok = false;
        const double count = first(map, key).toDouble(&ok);
        if (ok && count >= 0 && count < INT_MAX && t.tagPlayCount < 0)
            t.tagPlayCount = int(count);
    }
    readPopularity(id3v2Of(file.file()), t);

    // USLT becomes LYRICS (or LYRICS:<description>) in the property map, as
    // do Vorbis LYRICS/UNSYNCEDLYRICS and MP4's ©lyr.
    for (const auto &entry : map) {
        const TagLib::String &key = entry.first;
        if ((key.startsWith("LYRICS") || key == "UNSYNCEDLYRICS") && !entry.second.isEmpty() &&
            !entry.second.front().isEmpty()) {
            t.hasLyrics = true;
            break;
        }
    }
    t.hasEmbeddedArt = file.complexPropertyKeys().contains("PICTURE");

    t.contentType = guessContentType(QFileInfo(path).suffix().toLower(), t.genres);
    return t;
}

}
