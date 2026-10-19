#include "tagwriter.h"

#include "job.h"
#include "library.h"
#include "scanner.h"
#include "tagreader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <cmath>

#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2header.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/oggfile.h>
#include <taglib/popularimeterframe.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const QStringList multiValueFields = {"artist", "albumartist", "genre", "composer"};
const QStringList numberFields = {"year", "originalyear", "track", "tracktotal", "disc", "disctotal", "bpm"};
const QStringList customFields = {"custom1", "custom2", "custom3", "custom4", "custom5"};

TagLib::String ts(const QString &s) { return TagLib::String(s.toStdString(), TagLib::String::UTF8); }
QString qs(const TagLib::String &s) { return QString::fromStdString(s.to8Bit(true)); }

QString firstValue(const TagLib::PropertyMap &map, const char *key) {
    const auto it = map.find(key);
    return it != map.end() && !it->second.isEmpty() ? qs(it->second.front()).trimmed() : QString();
}

QStringList allValues(const TagLib::PropertyMap &map, const char *key) {
    QStringList out;
    const auto it = map.find(key);
    if (it != map.end())
        for (const TagLib::String &s : it->second)
            out << qs(s);
    return out;
}

QString numberText(int n) { return n > 0 ? QString::number(n) : QString(); }

// Every value is compared and journalled in one form: multi-value fields
// split and joined with "; ", numbers without leading zeros, 0 as empty
// (except a rating, where 0 stars is a rating), text trimmed.
QString normalise(const QString &field, const QString &value) {
    if (multiValueFields.contains(field))
        return TagReader::splitValues({value}).join("; ");
    if (numberFields.contains(field)) {
        const int n = field == "bpm" ? qRound(value.trimmed().toDouble()) : value.trimmed().toInt();
        return numberText(n);
    }
    if (field == "rating") {
        if (value.trimmed().isEmpty())
            return {};
        return QString::number(qBound(0, value.trimmed().toInt(), 100));
    }
    if (field == "compilation") {
        const QString v = value.trimmed().toLower();
        return v == "1" || v == "true" || v == "yes" ? QString("1") : QString();
    }
    if (field == "lyrics") {
        QString v = value;
        v.replace("\r\n", "\n");
        return v.trimmed();
    }
    if (field == "artwork")
        return value.trimmed();
    return value.trimmed();
}

QString pictureJson(const QByteArray &data, QString mime) {
    if (data.isEmpty())
        return {};
    if (mime.isEmpty())
        mime = QMimeDatabase().mimeTypeForData(data).name();
    const QJsonArray list{QJsonObject{{"type", "Front Cover"},
                                      {"mime", mime},
                                      {"description", ""},
                                      {"data", QString::fromLatin1(data.toBase64())}}};
    return QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact));
}

// The picture bytes in a JSON list, for comparing two lists.
QList<QByteArray> pictureData(const QString &json) {
    QList<QByteArray> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json.toUtf8()).array())
        out << QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1());
    return out;
}

// What the journal keeps as the new value of a picture: its hash, not its
// bytes. The old value keeps the bytes, which is what an undo needs.
QString pictureSummary(const QString &json) {
    const QList<QByteArray> data = pictureData(json);
    if (data.isEmpty())
        return {};
    QStringList parts;
    for (const QByteArray &d : data)
        parts << QString("sha1:%1 %2 bytes")
                     .arg(QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha1).toHex().left(16)))
                     .arg(d.size());
    return parts.join(", ");
}

bool sameValue(const QString &field, const QString &a, const QString &b) {
    if (field == "artwork")
        return pictureData(a) == pictureData(b);
    if (field == "rating" && !a.isEmpty() && !b.isEmpty()) // files keep ratings to the nearest half star
        return qAbs(a.toInt() - b.toInt()) < 10 && qRound(a.toInt() / 10.0) == qRound(b.toInt() / 10.0);
    return a == b;
}

// POPM bytes for a rating, the steps MediaMonkey writes (and Windows Media
// Player reads), so TagReader::ratingFromPopm() gives the same rating back.
int popmFor(int rating) {
    switch (qBound(0, qRound(rating / 10.0), 10)) {
    case 0: return 0;
    case 1: return 13;
    case 2: return 1;
    case 3: return 54;
    case 4: return 64;
    case 5: return 118;
    case 6: return 128;
    case 7: return 186;
    case 8: return 196;
    case 9: return 242;
    default: return 255;
    }
}

struct FileStat {
    bool ok = false;
    qint64 size = 0, mtimeNs = 0;
};

FileStat statOf(const QString &path) {
    struct stat st {};
    FileStat s;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return s;
    s.ok = true;
    s.size = st.st_size;
    s.mtimeNs = qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    return s;
}

void syncPath(const QString &path, bool directory) {
    const int fd = ::open(QFile::encodeName(path).constData(), directory ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

QString temporaryBeside(const QString &path) {
    const QFileInfo info(path);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const QString candidate = info.absolutePath() + QString("/.omamp-%1-").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')) +
                                  info.fileName();
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

// Writes the changed fields into a file with TagLib. merged holds every
// field's final value (the file's own values with the changes applied), so
// a track number and its total can be written together.
bool writeTags(const QString &path, const QMap<QString, QString> &changes, const QMap<QString, QString> &merged,
               bool writeRating, const QString &popmEmail, QString *error) {
    TagLib::FileRef file(QFile::encodeName(path).constData(), false);
    if (file.isNull() || !file.file() || !file.file()->isOpen() || file.file()->readOnly()) {
        *error = "TagLib can't open the file for writing";
        return false;
    }
    auto *mpeg = dynamic_cast<TagLib::MPEG::File *>(file.file());
    const bool xiph = dynamic_cast<TagLib::FLAC::File *>(file.file()) || dynamic_cast<TagLib::Ogg::File *>(file.file());
    // An MP3 keeps its ID3v2 version: Windows players still read only 2.3.
    const bool id3v23 = mpeg && mpeg->hasID3v2Tag() && mpeg->ID3v2Tag()->header()->majorVersion() == 3;

    TagLib::PropertyMap map = file.properties();
    QStringList setKeys;
    auto set = [&](const char *key, const QString &value, bool multi) {
        map.erase(key);
        if (value.isEmpty())
            return;
        TagLib::StringList list;
        // ID3v2 gets one value with MediaMonkey's "; " (2.3 would otherwise
        // join them with '/'); Vorbis comments and MP4 keep separate values.
        if (multi && !mpeg)
            for (const QString &v : value.split("; ", Qt::SkipEmptyParts))
                list.append(ts(v));
        else
            list.append(ts(value));
        map.insert(key, list);
        setKeys << key;
    };

    static const QMap<QString, const char *> simple = {
        {"title", "TITLE"},       {"artist", "ARTIST"},           {"albumartist", "ALBUMARTIST"},
        {"album", "ALBUM"},       {"genre", "GENRE"},             {"composer", "COMPOSER"},
        {"year", "DATE"},         {"originalyear", "ORIGINALDATE"}, {"comment", "COMMENT"},
        {"bpm", "BPM"},           {"compilation", "COMPILATION"},
    };
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        if (simple.contains(it.key()))
            set(simple.value(it.key()), it.value(), multiValueFields.contains(it.key()));
    }
    auto pair = [&](const char *numberKey, const char *totalKey, const char *altTotal, const QString &n,
                    const QString &total) {
        map.erase(altTotal);
        if (xiph) {
            set(numberKey, n, false);
            set(totalKey, total, false);
        } else {
            map.erase(totalKey);
            set(numberKey, n.isEmpty() ? QString() : total.isEmpty() ? n : n + '/' + total, false);
        }
    };
    if (changes.contains("track") || changes.contains("tracktotal"))
        pair("TRACKNUMBER", "TRACKTOTAL", "TOTALTRACKS", merged.value("track"), merged.value("tracktotal"));
    if (changes.contains("disc") || changes.contains("disctotal"))
        pair("DISCNUMBER", "DISCTOTAL", "TOTALDISCS", merged.value("disc"), merged.value("disctotal"));
    if (changes.contains("lyrics")) {
        QList<TagLib::String> old;
        for (const auto &entry : map)
            if (entry.first.startsWith("LYRICS") || entry.first == "UNSYNCEDLYRICS")
                old.append(entry.first);
        for (const TagLib::String &key : old)
            map.erase(key);
        set("LYRICS", changes.value("lyrics"), false);
    }
    if (writeRating && changes.contains("rating")) {
        map.erase("RATING");
        map.erase("FMPS_RATING");
        const QString r = changes.value("rating");
        if (!mpeg && !r.isEmpty())
            set("FMPS_RATING", QString::number(r.toInt() / 100.0, 'g', 3), false);
    }

    const TagLib::PropertyMap refused = file.setProperties(map);
    for (const QString &key : std::as_const(setKeys)) {
        if (refused.contains(ts(key))) {
            *error = QString("This kind of file can't hold %1").arg(key);
            return false;
        }
    }

    if (writeRating && changes.contains("rating") && mpeg) {
        const QString r = changes.value("rating");
        TagLib::ID3v2::Tag *tag = mpeg->ID3v2Tag(true);
        const int popm = r.isEmpty() ? 0 : popmFor(r.toInt());
        const TagLib::ID3v2::FrameList frames = tag->frameList("POPM");
        bool found = false;
        for (TagLib::ID3v2::Frame *frame : frames) {
            if (auto *p = dynamic_cast<TagLib::ID3v2::PopularimeterFrame *>(frame)) {
                p->setRating(popm);
                found = true;
            }
        }
        if (!found && popm > 0) {
            auto *p = new TagLib::ID3v2::PopularimeterFrame;
            p->setEmail(ts(popmEmail));
            p->setRating(popm);
            tag->addFrame(p);
        }
    }

    if (changes.contains("artwork")) {
        TagLib::List<TagLib::VariantMap> pictures;
        const QJsonArray list = QJsonDocument::fromJson(changes.value("artwork").toUtf8()).array();
        for (const QJsonValue &v : list) {
            const QJsonObject o = v.toObject();
            const QByteArray data = QByteArray::fromBase64(o.value("data").toString().toLatin1());
            TagLib::VariantMap p;
            p.insert("data", TagLib::ByteVector(data.constData(), uint(data.size())));
            p.insert("mimeType", ts(o.value("mime").toString()));
            p.insert("description", ts(o.value("description").toString()));
            p.insert("pictureType", ts(o.value("type").toString("Front Cover")));
            pictures.append(p);
        }
        if (!file.setComplexProperties("PICTURE", pictures) && !pictures.isEmpty()) {
            *error = "This kind of file can't hold pictures";
            return false;
        }
    }

    bool saved;
    if (mpeg) {
        int tags = TagLib::MPEG::File::ID3v2;
        if (mpeg->hasID3v1Tag())
            tags |= TagLib::MPEG::File::ID3v1;
        if (mpeg->hasAPETag())
            tags |= TagLib::MPEG::File::APE;
        saved = mpeg->save(tags, TagLib::File::StripNone, id3v23 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4,
                           TagLib::File::DoNotDuplicate);
    } else {
        saved = file.save();
    }
    if (!saved)
        *error = "TagLib could not save the file";
    return saved;
}

} // namespace

QJsonObject TagFileResult::toJson() const {
    QJsonArray list;
    for (const TagFieldChange &c : changes) {
        const bool picture = c.field == "artwork";
        list << QJsonObject{{"field", c.field},
                            {"old", picture ? pictureSummary(c.oldValue) : c.oldValue},
                            {"new", picture ? pictureSummary(c.newValue) : c.newValue}};
    }
    QJsonObject o{{"track_id", trackId}, {"path", path}, {"ok", ok}, {"file_written", fileWritten}, {"changes", list}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QJsonObject TagBatchResult::toJson() const {
    QJsonArray list;
    for (const TagFileResult &f : files)
        list << f.toJson();
    QJsonObject o{{"ok", ok},           {"batch", batch.isEmpty() ? QJsonValue() : QJsonValue(batch)},
                  {"dry_run", dryRun},  {"cancelled", cancelled},
                  {"tracks", tracks},   {"changed", changed},
                  {"unchanged", unchanged}, {"failed", failed},
                  {"elapsed_ms", elapsedMs}, {"files", list}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

TagWriter::TagWriter(Library &library, Job *job) : m_library(library), m_job(job) {}

const QStringList &TagWriter::fields() {
    static const QStringList list = {"title",  "artist",      "albumartist", "album",   "genre",   "composer",
                                     "year",   "originalyear", "track",      "tracktotal", "disc", "disctotal",
                                     "comment", "bpm",        "compilation", "lyrics",  "rating",  "artwork",
                                     "custom1", "custom2",    "custom3",     "custom4", "custom5"};
    return list;
}

QString TagWriter::canonicalField(const QString &name) {
    QString key = name.trimmed().toLower();
    key.remove(' ');
    key.remove('_');
    key.remove('-');
    if (fields().contains(key))
        return key;
    static const QHash<QString, QString> aliases = {
        {"aa", "albumartist"},    {"date", "year"},           {"originaldate", "originalyear"},
        {"tracknumber", "track"}, {"track#", "track"},       {"tn", "track"},
        {"totaltracks", "tracktotal"}, {"discnumber", "disc"}, {"disc#", "disc"},
        {"totaldiscs", "disctotal"}, {"tempo", "bpm"},       {"lyric", "lyrics"},
        {"unsyncedlyrics", "lyrics"}, {"picture", "artwork"}, {"cover", "artwork"},
        {"art", "artwork"},       {"ti", "title"},           {"ar", "artist"},
        {"al", "album"},          {"ge", "genre"},           {"co", "composer"},
    };
    return aliases.value(key);
}

bool TagWriter::isDatabaseOnly(const QString &field) { return customFields.contains(field); }

QString TagWriter::newBatchId(const QString &kind) {
    return QString("%1-%2-%3")
        .arg(kind, QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss"))
        .arg(QRandomGenerator::global()->bounded(0x10000), 4, 16, QChar('0'));
}

QMap<QString, QString> TagWriter::readFile(const QString &path, QString *error) {
    QMap<QString, QString> v;
    const QByteArray encoded = QFile::encodeName(path);
    TagLib::FileStream stream(encoded.constData(), true);
    if (!stream.isOpen()) {
        if (error)
            *error = "Can't open the file";
        return v;
    }
    TagLib::FileRef file(&stream, false);
    if (file.isNull()) {
        if (error)
            *error = "TagLib can't read this file";
        return v;
    }
    const TagLib::PropertyMap map = file.properties();
    v["title"] = firstValue(map, "TITLE");
    v["artist"] = TagReader::splitValues(allValues(map, "ARTIST")).join("; ");
    v["albumartist"] = TagReader::splitValues(allValues(map, "ALBUMARTIST")).join("; ");
    v["album"] = firstValue(map, "ALBUM");
    v["genre"] = TagReader::splitValues(allValues(map, "GENRE")).join("; ");
    v["composer"] = TagReader::splitValues(allValues(map, "COMPOSER")).join("; ");
    v["year"] = numberText(TagReader::parseYear(firstValue(map, "DATE")));
    v["originalyear"] = numberText(TagReader::parseYear(firstValue(map, "ORIGINALDATE")));
    int n = 0, total = 0;
    TagReader::parsePair(firstValue(map, "TRACKNUMBER"), &n, &total);
    if (!total)
        total = firstValue(map, "TRACKTOTAL").toInt() ?: firstValue(map, "TOTALTRACKS").toInt();
    v["track"] = numberText(n);
    v["tracktotal"] = numberText(total);
    n = total = 0;
    TagReader::parsePair(firstValue(map, "DISCNUMBER"), &n, &total);
    if (!total)
        total = firstValue(map, "DISCTOTAL").toInt() ?: firstValue(map, "TOTALDISCS").toInt();
    v["disc"] = numberText(n);
    v["disctotal"] = numberText(total);
    v["comment"] = firstValue(map, "COMMENT");
    v["bpm"] = numberText(qRound(firstValue(map, "BPM").toDouble()));
    v["compilation"] = normalise("compilation", firstValue(map, "COMPILATION"));
    QString lyrics = firstValue(map, "LYRICS");
    if (lyrics.isEmpty()) {
        for (const auto &entry : map) {
            if ((entry.first.startsWith("LYRICS") || entry.first == "UNSYNCEDLYRICS") && !entry.second.isEmpty()) {
                lyrics = qs(entry.second.front());
                break;
            }
        }
    }
    v["lyrics"] = normalise("lyrics", lyrics);
    QJsonArray pictures;
    for (const TagLib::VariantMap &p : file.complexProperties("PICTURE")) {
        const TagLib::ByteVector data = p.value("data").toByteVector();
        pictures << QJsonObject{{"type", qs(p.value("pictureType").toString())},
                                {"mime", qs(p.value("mimeType").toString())},
                                {"description", qs(p.value("description").toString())},
                                {"data", QString::fromLatin1(QByteArray(data.data(), int(data.size())).toBase64())}};
    }
    v["artwork"] = pictures.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(pictures).toJson(QJsonDocument::Compact));
    return v;
}

QMap<QString, QString> TagWriter::currentValues(qint64 trackId, QString *error) {
    QMap<QString, QString> v;
    const QString path = m_library.absolutePath(trackId);
    if (!path.isEmpty())
        v = readFile(path, error);
    else if (error)
        *error = "The track is offline";
    QSqlQuery q(m_library.db());
    q.prepare("SELECT rating, custom1, custom2, custom3, custom4, custom5 FROM tracks WHERE id = ?");
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        v["rating"] = q.value(0).isNull() ? QString() : q.value(0).toString();
        for (int k = 0; k < 5; ++k)
            v[customFields[k]] = q.value(k + 1).toString();
    }
    return v;
}

TagBatchResult TagWriter::apply(const QList<TagEdit> &editsIn, const TagWriteOptions &options) {
    TagBatchResult result;
    result.dryRun = options.dryRun;
    QElapsedTimer timer;
    timer.start();

    // Canonical field names, edits grouped by track in the order given.
    QList<qint64> order;
    QHash<qint64, QList<TagEdit>> byTrack;
    for (TagEdit e : editsIn) {
        const QString field = canonicalField(e.field);
        if (field.isEmpty()) {
            result.ok = false;
            result.error = QString("Unknown field \"%1\". Fields: %2").arg(e.field, fields().join(", "));
            return result;
        }
        e.field = field;
        if (!byTrack.contains(e.trackId))
            order << e.trackId;
        byTrack[e.trackId] << e;
    }

    const bool writeRatings = options.writeRatingsToFiles >= 0
                                  ? options.writeRatingsToFiles > 0
                                  : QStringList{"1", "true", "yes"}.contains(
                                        m_library.setting("tags.write_ratings", "0").toString().toLower());
    const QString popmEmail = !options.popmEmail.isEmpty()
                                  ? options.popmEmail
                                  : m_library.setting("tags.popm_email", "no@email").toString();
    const QString batch = options.dryRun ? QString()
                                         : newBatchId(options.source.startsWith("undo") ? "undo" : "tag");
    const qint64 jobRow = options.dryRun ? 0 : Journal::startJobRow(m_library, "tag", {batch});
    if (m_job)
        m_job->start();

    bool journalled = false;
    for (int k = 0; k < order.size(); ++k) {
        if (m_job && m_job->isCancelled()) {
            result.cancelled = true;
            break;
        }
        TagFileResult r = writeOne(order[k], byTrack.value(order[k]), options, batch, writeRatings, popmEmail);
        ++result.tracks;
        if (!r.ok) {
            ++result.failed;
            result.ok = false;
        } else if (r.changes.isEmpty()) {
            ++result.unchanged;
        } else {
            ++result.changed;
            journalled = journalled || !options.dryRun;
        }
        result.files << r;
        if (m_job)
            m_job->setProgress(options.dryRun ? "Checking tags" : "Writing tags", k + 1, order.size());
    }
    if (journalled)
        result.batch = batch;
    result.elapsedMs = timer.elapsed();
    if (jobRow)
        Journal::finishJobRow(m_library, jobRow, result.ok, result.cancelled, result.toJson(),
                              result.ok ? QString() : QString("%1 files failed").arg(result.failed));
    if (m_job) {
        if (result.ok)
            m_job->finish();
        else
            m_job->fail(QString("%1 of %2 files failed").arg(result.failed).arg(result.tracks));
    }
    return result;
}

TagFileResult TagWriter::writeOne(qint64 trackId, const QList<TagEdit> &edits, const TagWriteOptions &options,
                                  const QString &batch, bool writeRatings, const QString &popmEmail) {
    TagFileResult r;
    r.trackId = trackId;

    // The new values, last edit of a field winning.
    QMap<QString, QString> wanted;
    for (const TagEdit &e : edits) {
        QString value = e.value;
        if (e.field == "artwork")
            value = e.picture.isEmpty() ? (e.value.trimmed().startsWith('[') ? e.value : QString())
                                        : pictureJson(e.picture, e.pictureMime);
        wanted[e.field] = normalise(e.field, value);
    }
    auto inFile = [&](const QString &field) {
        return !isDatabaseOnly(field) && (field != "rating" || writeRatings);
    };
    bool needsFile = false;
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it)
        needsFile = needsFile || inFile(it.key());

    QSqlQuery exists(m_library.db());
    exists.prepare("SELECT rating, custom1, custom2, custom3, custom4, custom5 FROM tracks WHERE id = ?");
    exists.addBindValue(trackId);
    if (!exists.exec() || !exists.next()) {
        r.ok = false;
        r.error = "Not in the library";
        return r;
    }
    QMap<QString, QString> current;
    current["rating"] = exists.value(0).isNull() ? QString() : exists.value(0).toString();
    for (int k = 0; k < 5; ++k)
        current[customFields[k]] = exists.value(k + 1).toString();
    exists.finish();

    r.path = m_library.absolutePath(trackId);
    if (needsFile) {
        if (r.path.isEmpty()) {
            r.ok = false;
            r.error = "The track is offline";
            return r;
        }
        if (!Journal::insideSandbox(m_sandbox, r.path)) {
            r.ok = false;
            r.error = "Outside the allowed folder";
            return r;
        }
        QString error;
        const QMap<QString, QString> file = readFile(r.path, &error);
        if (!error.isEmpty()) {
            r.ok = false;
            r.error = error;
            return r;
        }
        for (auto it = file.constBegin(); it != file.constEnd(); ++it)
            current[it.key()] = it.value();
    }

    QMap<QString, QString> fileChanges;
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        const QString old = current.value(it.key());
        if (sameValue(it.key(), old, it.value()))
            continue;
        r.changes << TagFieldChange{it.key(), old, it.value()};
        if (inFile(it.key()))
            fileChanges[it.key()] = it.value();
    }
    if (r.changes.isEmpty() || options.dryRun)
        return r;

    FileStat after;
    TrackTags reread;
    if (!fileChanges.isEmpty()) {
        const FileStat before = statOf(r.path);
        const TrackTags original = TagReader::read(r.path);
        if (!before.ok || !original.ok) {
            r.ok = false;
            r.error = original.error.isEmpty() ? QString("Can't read the file") : original.error;
            return r;
        }
        const QString temporary = temporaryBeside(r.path);
        if (temporary.isEmpty() || !Journal::insideSandbox(m_sandbox, temporary)) {
            r.ok = false;
            r.error = "No room for a temporary file beside the original";
            return r;
        }
        auto removeTemporary = qScopeGuard([&] { QFile::remove(temporary); });
        if (!QFile::copy(r.path, temporary)) {
            r.ok = false;
            r.error = "Can't copy the file to write it safely (is the folder writable?)";
            return r;
        }
        QFile::setPermissions(temporary, QFile::permissions(r.path) | QFile::WriteOwner);
        QMap<QString, QString> merged = current;
        for (auto it = fileChanges.constBegin(); it != fileChanges.constEnd(); ++it)
            merged[it.key()] = it.value();
        QString error;
        if (!writeTags(temporary, fileChanges, merged, writeRatings, popmEmail, &error)) {
            r.ok = false;
            r.error = error;
            return r;
        }
        syncPath(temporary, false);

        // Read the copy back: every field must say what was written, and the
        // audio must still be there.
        const QMap<QString, QString> check = readFile(temporary, &error);
        const TrackTags checkTags = TagReader::read(temporary);
        if (!error.isEmpty() || !checkTags.ok || qAbs(checkTags.durationMs - original.durationMs) > 100) {
            r.ok = false;
            r.error = "The written copy did not read back correctly; the original is unchanged";
            return r;
        }
        for (auto it = fileChanges.constBegin(); it != fileChanges.constEnd(); ++it) {
            const QString got = it.key() == "rating"
                                    ? (checkTags.tagRating < 0 ? QString() : QString::number(checkTags.tagRating))
                                    : check.value(it.key());
            if (!sameValue(it.key(), got, it.value())) {
                r.ok = false;
                r.error = QString("%1 read back as \"%2\", not \"%3\"; the original is unchanged")
                              .arg(it.key(), it.key() == "artwork" ? pictureSummary(got) : got,
                                   it.key() == "artwork" ? pictureSummary(it.value()) : it.value());
                return r;
            }
        }
        if (m_beforeReplace && !m_beforeReplace(temporary, r.path)) {
            r.ok = false;
            r.error = "Stopped before replacing the original; the original is unchanged";
            return r;
        }
        const FileStat now = statOf(r.path);
        if (!now.ok || now.size != before.size || now.mtimeNs != before.mtimeNs) {
            r.ok = false;
            r.error = "Something else changed the file while its tags were written; it was left alone";
            return r;
        }
        if (::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(r.path).constData()) != 0) {
            r.ok = false;
            r.error = QString("Can't replace the original: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            return r;
        }
        removeTemporary.dismiss();
        syncPath(QFileInfo(r.path).absolutePath(), true);
        r.fileWritten = true;
        after = statOf(r.path);
        reread = TagReader::read(r.path);
        reread.quickHash = TagReader::quickHash(r.path);
    }

    // Journal and library row in one transaction.
    QSqlDatabase db = m_library.db();
    db.transaction();
    bool ok = true;
    QString error;
    QSqlQuery journal(db);
    journal.prepare("INSERT INTO tag_journal (batch, track_id, field, old_value, new_value, source, changed_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const TagFieldChange &c : std::as_const(r.changes)) {
        journal.addBindValue(batch);
        journal.addBindValue(trackId);
        journal.addBindValue(c.field);
        journal.addBindValue(c.oldValue);
        journal.addBindValue(c.field == "artwork" ? pictureSummary(c.newValue) : c.newValue);
        journal.addBindValue(options.source);
        journal.addBindValue(now);
        if (!journal.exec()) {
            ok = false;
            error = journal.lastError().text();
            break;
        }
    }
    if (ok && r.fileWritten)
        ok = Scanner::updateTrackRow(m_library, trackId, reread, after.size, after.mtimeNs, &error);
    for (const TagFieldChange &c : std::as_const(r.changes)) {
        if (!ok)
            break;
        if (c.field != "rating" && !isDatabaseOnly(c.field))
            continue;
        QSqlQuery u(db);
        const QString column = c.field == "rating" ? "rating" : c.field;
        u.prepare(QString("UPDATE tracks SET %1 = ?, updated = ? WHERE id = ?").arg(column));
        if (c.newValue.isEmpty())
            u.addBindValue(QVariant(c.field == "rating" ? QMetaType(QMetaType::Int) : QMetaType(QMetaType::QString)));
        else
            u.addBindValue(c.field == "rating" ? QVariant(c.newValue.toInt()) : QVariant(c.newValue));
        u.addBindValue(now);
        u.addBindValue(trackId);
        if (!u.exec()) {
            ok = false;
            error = u.lastError().text();
        }
    }
    if (ok && !db.commit()) {
        ok = false;
        error = db.lastError().text();
    }
    if (!ok) {
        db.rollback();
        r.ok = false;
        r.error = r.fileWritten ? QString("The file was written but the library was not updated (%1); the next scan "
                                          "reads it")
                                      .arg(error)
                                : error;
    }
    return r;
}

TagBatchResult TagWriter::undo(const QString &batch) {
    TagBatchResult result;
    const QList<Journal::Entry> entries = Journal::entries(m_library, batch);
    // The oldest entry for each track and field holds the value before the batch.
    QList<qint64> order;
    QHash<qint64, QMap<QString, QString>> old;
    int skipped = 0;
    for (const Journal::Entry &e : entries) {
        if (e.undone)
            continue;
        if (!e.trackId) {
            ++skipped;
            continue;
        }
        if (!old.contains(e.trackId))
            order << e.trackId;
        if (!old[e.trackId].contains(e.field))
            old[e.trackId][e.field] = e.oldValue;
    }
    if (order.isEmpty()) {
        result.ok = false;
        result.error = entries.isEmpty() ? QString("No batch called %1 in the journal").arg(batch)
                                         : QString("Batch %1 has nothing left to undo").arg(batch);
        return result;
    }
    QList<TagEdit> edits;
    for (qint64 id : std::as_const(order)) {
        const QMap<QString, QString> &fields = old[id];
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
            edits << TagEdit{id, it.key(), it.value(), {}, {}};
    }
    TagWriteOptions options;
    options.source = "undo:" + batch;
    result = apply(edits, options);

    QSqlQuery mark(m_library.db());
    mark.prepare("UPDATE tag_journal SET undone = 1 WHERE batch = ? AND track_id = ?");
    for (const TagFileResult &f : std::as_const(result.files)) {
        if (!f.ok)
            continue;
        mark.addBindValue(batch);
        mark.addBindValue(f.trackId);
        mark.exec();
    }
    if (skipped && result.error.isEmpty())
        result.error = QString("%1 changes belong to tracks no longer in the library").arg(skipped);
    return result;
}

namespace Journal {

QJsonObject Entry::toJson() const {
    const bool picture = field == "artwork";
    return {{"id", id},
            {"batch", batch},
            {"track_id", trackId ? QJsonValue(trackId) : QJsonValue()},
            {"field", field},
            {"old", picture && oldValue.startsWith('[') ? pictureSummary(oldValue) : oldValue},
            {"new", newValue},
            {"source", source},
            {"changed_at", changedAt},
            {"undone", undone}};
}

QString Batch::kind() const {
    if (source.startsWith("undo"))
        return "undo";
    if (source == "organise")
        return "organise";
    return "tag";
}

QJsonObject Batch::toJson() const {
    return {{"batch", id},       {"kind", kind()},  {"source", source}, {"changed_at", changedAt},
            {"entries", entries}, {"tracks", tracks}, {"undone", undone()}};
}

QList<Batch> batches(Library &library, int limit) {
    QList<Batch> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT batch, min(source), max(changed_at), count(*), count(DISTINCT track_id), sum(undone), max(id) "
              "FROM tag_journal GROUP BY batch ORDER BY max(id) DESC LIMIT ?");
    q.addBindValue(limit > 0 ? limit : -1);
    if (q.exec()) {
        while (q.next()) {
            Batch b;
            b.id = q.value(0).toString();
            b.source = q.value(1).toString();
            b.changedAt = q.value(2).toLongLong();
            b.entries = q.value(3).toInt();
            b.tracks = q.value(4).toInt();
            b.undoneEntries = q.value(5).toInt();
            out << b;
        }
    }
    return out;
}

QList<Entry> entries(Library &library, const QString &batch) {
    QList<Entry> out;
    QSqlQuery q(library.db());
    q.prepare("SELECT id, batch, track_id, field, old_value, new_value, source, changed_at, undone "
              "FROM tag_journal WHERE batch = ? ORDER BY id");
    q.addBindValue(batch);
    if (q.exec()) {
        while (q.next()) {
            Entry e;
            e.id = q.value(0).toLongLong();
            e.batch = q.value(1).toString();
            e.trackId = q.value(2).toLongLong();
            e.field = q.value(3).toString();
            e.oldValue = q.value(4).toString();
            e.newValue = q.value(5).toString();
            e.source = q.value(6).toString();
            e.changedAt = q.value(7).toLongLong();
            e.undone = q.value(8).toBool();
            out << e;
        }
    }
    return out;
}

QString lastUndoable(Library &library) {
    QSqlQuery q(library.db());
    q.exec("SELECT batch FROM tag_journal GROUP BY batch "
           "HAVING min(source) NOT LIKE 'undo%' AND sum(undone) < count(*) ORDER BY max(id) DESC LIMIT 1");
    return q.next() ? q.value(0).toString() : QString();
}

bool isOrganise(Library &library, const QString &batch) {
    QSqlQuery q(library.db());
    q.prepare("SELECT EXISTS (SELECT 1 FROM tag_journal WHERE batch = ? AND field IN ('path', 'companion', "
              "'folder_removed'))");
    q.addBindValue(batch);
    return q.exec() && q.next() && q.value(0).toBool();
}

qint64 startJobRow(Library &library, const QString &kind, const QStringList &args) {
    QSqlQuery q(library.db());
    q.prepare("INSERT INTO jobs (kind, state, args, created, started) VALUES (?, 'running', ?, ?, ?)");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    q.addBindValue(kind);
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact)));
    q.addBindValue(now);
    q.addBindValue(now);
    return q.exec() ? q.lastInsertId().toLongLong() : 0;
}

void finishJobRow(Library &library, qint64 jobId, bool ok, bool cancelled, const QJsonObject &result,
                  const QString &error) {
    if (!jobId)
        return;
    QJsonObject small = result;
    small.remove("files");
    small.remove("items");
    QSqlQuery q(library.db());
    q.prepare("UPDATE jobs SET state = ?, result = ?, error = ?, finished = ? WHERE id = ?");
    q.addBindValue(cancelled ? "cancelled" : ok ? "done" : "failed");
    q.addBindValue(QString::fromUtf8(QJsonDocument(small).toJson(QJsonDocument::Compact)));
    q.addBindValue(error.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(error));
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(jobId);
    q.exec();
}

bool insideSandbox(const QString &folder, const QString &path) {
    if (folder.isEmpty())
        return true;
    const QString f = QDir::cleanPath(folder);
    const QString p = QDir::cleanPath(path);
    return p == f || p.startsWith(f + '/');
}

}
