#include "tageditor.h"

#include "librarycontroller.h"
#include "library.h"
#include "portal.h"
#include "tagwriter.h"

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMimeData>
#include <QUrl>

namespace {

QString mimeOf(const QByteArray &bytes) {
    if (bytes.startsWith("\x89PNG"))
        return "image/png";
    if (bytes.startsWith("\xFF\xD8"))
        return "image/jpeg";
    if (bytes.startsWith("GIF8"))
        return "image/gif";
    if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP")
        return "image/webp";
    if (bytes.startsWith("BM"))
        return "image/bmp";
    return {};
}

// "600 × 600, JPEG, 48 KB".
QString describePicture(const QByteArray &bytes, const QString &mime) {
    QImage image;
    image.loadFromData(bytes);
    const QString type = mime.section('/', 1).toUpper();
    const QString size = bytes.size() >= 1048576
                             ? QString("%1 MB").arg(QLocale().toString(bytes.size() / 1048576.0, 'f', 1))
                             : QString("%1 KB").arg(qMax<qsizetype>(1, bytes.size() / 1024));
    if (image.isNull())
        return QString("%1, %2").arg(type, size);
    return QString("%1 × %2, %3, %4").arg(image.width()).arg(image.height()).arg(type, size);
}

// A picture beside the file that players use for the album: cover.jpg and
// the like. Only looked for when the file has none of its own.
QString folderPicture(const QString &trackPath) {
    const QDir dir = QFileInfo(trackPath).absoluteDir();
    const QStringList names = dir.entryList({"*.jpg", "*.jpeg", "*.png", "*.webp"}, QDir::Files, QDir::Name);
    static const QStringList preferred = {"cover", "folder", "front", "album", "albumart"};
    for (const QString &want : preferred)
        for (const QString &name : names)
            if (QFileInfo(name).completeBaseName().compare(want, Qt::CaseInsensitive) == 0)
                return dir.filePath(name);
    for (const QString &name : names)
        if (name.startsWith("albumart", Qt::CaseInsensitive))
            return dir.filePath(name);
    return {};
}

} // namespace

const QStringList &TagEditor::editableFields() {
    static const QStringList list = {"title",   "artist", "albumartist", "album",     "genre",   "composer",
                                     "year",    "track",  "tracktotal",  "disc",      "disctotal", "bpm",
                                     "comment", "rating", "custom1",     "custom2",   "custom3", "custom4",
                                     "custom5", "lyrics"};
    return list;
}

TagEditor::TagEditor(LibraryController *controller, QObject *parent) : QObject(parent), m_controller(controller) {}

TagEditor::~TagEditor() {
    if (m_loadThread) {
        m_loadThread->wait();
        delete m_loadThread;
    }
}

void TagEditor::setPicker(FolderPicker *picker) {
    m_picker = picker;
    picker->setup(tr("Choose a picture for the artwork"), tr("Use picture"), false, false, tr("Pictures"),
                  {"*.jpg", "*.jpeg", "*.png", "*.webp", "*.gif", "*.bmp", "*.JPG", "*.JPEG", "*.PNG"});
    connect(picker, &FolderPicker::picked, this, [this](const QStringList &paths) {
        if (!paths.isEmpty())
            setArtworkFromFile(paths.first());
    });
    connect(picker, &FolderPicker::failed, this, &TagEditor::message);
}

TagEditor::Loaded TagEditor::readTracks(const QString &dbPath, const QList<qint64> &ids) {
    Loaded out;
    Library library(dbPath);
    if (!library.open(&out.firstError)) {
        out.unreadable = ids.size();
        return out;
    }
    TagWriter writer(library);
    QByteArray firstHash;
    bool firstPictureSeen = false;
    for (qint64 id : ids) {
        QString error;
        QMap<QString, QString> v = writer.currentValues(id, &error);
        if (!error.isEmpty()) {
            if (library.absolutePath(id).isEmpty())
                ++out.offline;
            else
                ++out.unreadable;
            if (out.firstError.isEmpty())
                out.firstError = error;
        }
        for (const QString &field : editableFields()) {
            QStringList &seen = out.values[field];
            const QString value = v.value(field);
            if (!seen.contains(value))
                seen << value;
        }
        // Pictures: the first track's front cover is shown; a hash of each
        // track's set tells whether they all carry the same one.
        const QString art = v.value("artwork");
        QByteArray bytes, mime;
        if (!art.isEmpty()) {
            const QJsonArray pictures = QJsonDocument::fromJson(art.toUtf8()).array();
            QJsonObject chosen = pictures.isEmpty() ? QJsonObject() : pictures.first().toObject();
            for (const QJsonValue &p : pictures)
                if (p.toObject().value("type").toString().contains("Front", Qt::CaseInsensitive)) {
                    chosen = p.toObject();
                    break;
                }
            bytes = QByteArray::fromBase64(chosen.value("data").toString().toLatin1());
            mime = chosen.value("mime").toString().toLatin1();
        }
        const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
        if (!firstPictureSeen) {
            firstPictureSeen = true;
            firstHash = hash;
            if (!bytes.isEmpty()) {
                out.picture = bytes;
                out.pictureMime = mime.isEmpty() ? mimeOf(bytes) : QString::fromLatin1(mime);
                out.pictureNote = tr("In the file: %1").arg(describePicture(bytes, out.pictureMime));
            } else {
                const QString path = library.absolutePath(id);
                const QString folder = path.isEmpty() ? QString() : folderPicture(path);
                QFile f(folder);
                if (!folder.isEmpty() && f.open(QIODevice::ReadOnly)) {
                    out.picture = f.readAll();
                    out.pictureMime = mimeOf(out.picture);
                    out.pictureNote = tr("Folder picture %1, not in the file: %2")
                                          .arg(QFileInfo(folder).fileName(), describePicture(out.picture, out.pictureMime));
                }
            }
        } else if (hash != firstHash) {
            out.picturesDiffer = true;
        }
    }
    return out;
}

bool TagEditor::load(const QVariantList &ids) {
    if (!m_controller || !m_controller->activate())
        return false;
    QList<qint64> list;
    for (const QVariant &v : ids)
        if (v.toLongLong() > 0 && !list.contains(v.toLongLong()))
            list << v.toLongLong();
    if (list.isEmpty())
        return false;
    waitForLoad();
    m_ids = list;
    m_original.clear();
    m_edits.clear();
    m_picture.clear();
    m_newPicture.clear();
    m_pictureNote.clear();
    m_picturesDiffer = false;
    m_artworkAction = "keep";
    m_notice.clear();
    m_failures.clear();
    m_resultText.clear();
    if (list.size() == 1) {
        const QVariantMap info = m_controller->trackInfo(list.first());
        m_heading = info.value("title").toString();
    } else {
        m_heading = tr("%n tracks", "", int(list.size()));
    }
    const int generation = ++m_generation;
    const QString path = m_controller->databasePath();
    m_loadThread = QThread::create([this, generation, path, list] {
        const Loaded loaded = readTracks(path, list);
        QMetaObject::invokeMethod(this, [this, generation, loaded] { onLoaded(generation, loaded); },
                                  Qt::QueuedConnection);
    });
    m_loadThread->setObjectName("thrum-tags-read");
    m_loadThread->start();
    emit loadingChanged();
    emit loadedChanged();
    emit appliedChanged();
    bump();
    return true;
}

void TagEditor::waitForLoad() {
    if (!m_loadThread)
        return;
    m_loadThread->wait();
    // Deliver the queued result now.
    QCoreApplication::sendPostedEvents(this);
}

void TagEditor::onLoaded(int generation, const Loaded &loaded) {
    if (m_loadThread) {
        m_loadThread->wait();
        delete m_loadThread;
        m_loadThread = nullptr;
        emit loadingChanged();
    }
    if (generation != m_generation)
        return;
    m_original = loaded.values;
    m_picture = loaded.picture;
    m_pictureMime = loaded.pictureMime;
    m_pictureNote = loaded.pictureNote;
    m_picturesDiffer = loaded.picturesDiffer;
    QStringList notes;
    if (loaded.offline)
        notes << tr("%n tracks are offline and can't be changed", "", loaded.offline);
    if (loaded.unreadable)
        notes << tr("%n files couldn't be read: %1", "", loaded.unreadable).arg(loaded.firstError);
    m_notice = notes.join(". ");
    emit loadedChanged();
    bump();
}

void TagEditor::bump() {
    ++m_revision;
    emit changed();
}

QString TagEditor::value(const QString &field) const {
    if (m_edits.contains(field))
        return m_edits.value(field);
    const QStringList v = m_original.value(field);
    return v.size() == 1 ? v.first() : QString();
}

bool TagEditor::mixed(const QString &field) const {
    return !m_edits.contains(field) && m_original.value(field).size() > 1;
}

bool TagEditor::edited(const QString &field) const { return m_edits.contains(field); }

QStringList TagEditor::originalValues(const QString &field) const {
    QStringList out;
    for (const QString &v : m_original.value(field)) {
        out << (v.isEmpty() ? tr("(empty)") : v.left(80));
        if (out.size() == 5)
            break;
    }
    return out;
}

void TagEditor::setValue(const QString &field, const QString &text) {
    if (!editableFields().contains(field))
        return;
    const QStringList original = m_original.value(field);
    // Typing the value every track already has is no edit.
    if (original.size() == 1 && original.first() == text) {
        if (m_edits.remove(field))
            bump();
        return;
    }
    if (original.size() > 1 && text.isEmpty()) {
        // Emptying a mixed field by typing goes back to keeping each
        // track's value; the field's ✕ (clearField) empties it for real.
        if (m_edits.remove(field))
            bump();
        return;
    }
    if (m_edits.contains(field) && m_edits.value(field) == text)
        return;
    m_edits.insert(field, text);
    bump();
}

void TagEditor::clearField(const QString &field) {
    if (!editableFields().contains(field))
        return;
    const QStringList original = m_original.value(field);
    if (original.size() == 1 && original.first().isEmpty())
        m_edits.remove(field);
    else
        m_edits.insert(field, QString());
    bump();
}

void TagEditor::revert(const QString &field) {
    if (m_edits.remove(field))
        bump();
}

void TagEditor::revertAll() {
    m_edits.clear();
    m_artworkAction = "keep";
    m_newPicture.clear();
    bump();
}

QString TagEditor::artworkSource() const {
    const QByteArray &bytes = m_artworkAction == QLatin1String("new") ? m_newPicture
                              : m_artworkAction == QLatin1String("remove") ? QByteArray()
                                                                           : m_picture;
    if (bytes.isEmpty())
        return {};
    const QString mime = m_artworkAction == QLatin1String("new") ? m_newPictureMime : m_pictureMime;
    if (bytes.size() > 2 * 1048576) {
        // Very large pictures are shown from a smaller copy.
        QImage image;
        image.loadFromData(bytes);
        if (!image.isNull()) {
            QByteArray small;
            QBuffer buffer(&small);
            buffer.open(QIODevice::WriteOnly);
            image.scaled(600, 600, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "JPEG", 85);
            return "data:image/jpeg;base64," + QString::fromLatin1(small.toBase64());
        }
    }
    return QString("data:%1;base64,").arg(mime.isEmpty() ? QStringLiteral("image/jpeg") : mime) +
           QString::fromLatin1(bytes.toBase64());
}

QString TagEditor::artworkNote() const {
    if (m_artworkAction == QLatin1String("new"))
        return m_ids.size() > 1 ? tr("New picture for all %n tracks: %1", "", int(m_ids.size())).arg(m_newPictureNote)
                                : tr("New picture: %1").arg(m_newPictureNote);
    if (m_artworkAction == QLatin1String("remove"))
        return tr("Every picture in the files will be removed when you save");
    if (loading())
        return {};
    QString note = m_pictureNote.isEmpty() ? tr("No picture in the file") : m_pictureNote;
    if (m_picturesDiffer)
        note = tr("The tracks have different pictures; showing the first track's") + "\n" + note;
    return note;
}

bool TagEditor::setArtworkFromFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        emit message(tr("Can't open %1").arg(QFileInfo(path).fileName()));
        return false;
    }
    const QByteArray bytes = f.readAll();
    const QString mime = mimeOf(bytes);
    if (mime.isEmpty() || QImage::fromData(bytes).isNull()) {
        emit message(tr("%1 isn't a picture Thrum can read").arg(QFileInfo(path).fileName()));
        return false;
    }
    m_newPicture = bytes;
    m_newPictureMime = mime;
    m_newPictureNote = QString("%1, %2").arg(QFileInfo(path).fileName(), describePicture(bytes, mime));
    m_artworkAction = "new";
    bump();
    return true;
}

bool TagEditor::pasteArtwork() {
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (data && data->hasUrls()) {
        for (const QUrl &url : data->urls())
            if (url.isLocalFile())
                return setArtworkFromFile(url.toLocalFile());
    }
    if (data && data->hasImage()) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        if (!image.isNull()) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            // A photo pastes as a JPEG; something with transparency as a PNG.
            const bool png = image.hasAlphaChannel();
            image.save(&buffer, png ? "PNG" : "JPEG", 92);
            m_newPicture = bytes;
            m_newPictureMime = png ? "image/png" : "image/jpeg";
            m_newPictureNote = tr("pasted, %1").arg(describePicture(bytes, m_newPictureMime));
            m_artworkAction = "new";
            bump();
            return true;
        }
    }
    emit message(tr("There's no picture on the clipboard"));
    return false;
}

void TagEditor::removeArtwork() {
    m_artworkAction = "remove";
    m_newPicture.clear();
    bump();
}

void TagEditor::keepArtwork() {
    m_artworkAction = "keep";
    m_newPicture.clear();
    bump();
}

void TagEditor::pickArtwork() {
    if (m_picker)
        m_picker->pickFolders();
    else
        emit message(tr("No file picker is available"));
}

static QJsonObject runEdits(Library &library, Job *job, const QList<TagEdit> &edits, const QString &sandbox,
                            const QString &source) {
    TagWriter writer(library, job);
    writer.setSandbox(sandbox);
    TagWriteOptions options;
    options.source = source;
    return writer.apply(edits, options).toJson();
}

bool TagEditor::apply() {
    if (!m_controller || m_applying || loading() || !dirty())
        return false;
    QList<TagEdit> edits;
    for (qint64 id : std::as_const(m_ids)) {
        for (auto it = m_edits.constBegin(); it != m_edits.constEnd(); ++it)
            edits << TagEdit{id, it.key(), it.value(), {}, {}};
        if (m_artworkAction == QLatin1String("new"))
            edits << TagEdit{id, "artwork", {}, m_newPicture, m_newPictureMime};
        else if (m_artworkAction == QLatin1String("remove"))
            edits << TagEdit{id, "artwork", {}, {}, {}};
    }
    const QString sandbox = m_controller->writeSandbox();
    const int n = m_ids.size();
    const bool started = m_controller->runJob(
        "tag", tr("Saving tags of %n tracks", "", n),
        [edits, sandbox](Library &library, Job *job) { return runEdits(library, job, edits, sandbox, "edit"); },
        [this](const QJsonObject &r) {
            m_applying = false;
            emit applyingChanged();
            m_failures.clear();
            for (const QJsonValue &v : r.value("files").toArray()) {
                const QJsonObject f = v.toObject();
                if (!f.value("ok").toBool(true))
                    m_failures << QVariantMap{{"path", f.value("path").toString()},
                                              {"error", f.value("error").toString()}};
            }
            const int changed = r.value("changed").toInt(), failed = r.value("failed").toInt();
            if (r.contains("error") && failed == 0 && changed == 0)
                m_resultText = tr("Nothing was saved: %1").arg(r.value("error").toString());
            else if (failed > 0)
                m_resultText = tr("%n tracks saved", "", changed) + ", " + tr("%n failed", "", failed);
            else if (changed == 0)
                m_resultText = tr("Nothing needed changing");
            else
                m_resultText = tr("%n tracks saved", "", changed);
            emit appliedChanged();
            m_controller->refreshAll();
            emit m_controller->toast(m_resultText + (changed > 0 ? "  ·  " + tr("Ctrl+Z undoes") : QString()));
            const bool ok = m_failures.isEmpty() && !(r.contains("error") && changed == 0);
            if (ok) {
                m_edits.clear();
                m_artworkAction = "keep";
                bump();
            } else {
                // Read the files again, so the form shows what is there now
                // and the edits stay for another try.
                const QHash<QString, QString> keep = m_edits;
                const QString art = m_artworkAction;
                const QByteArray pic = m_newPicture;
                const QVariantList failures = m_failures;
                const QString text = m_resultText;
                QVariantList ids;
                for (qint64 id : std::as_const(m_ids))
                    ids << id;
                load(ids);
                m_edits = keep;
                m_artworkAction = art;
                m_newPicture = pic;
                m_failures = failures;
                m_resultText = text;
                emit appliedChanged();
                bump();
            }
            emit applied(ok);
        });
    if (started) {
        m_applying = true;
        emit applyingChanged();
    }
    return started;
}

bool TagEditor::quickSet(qint64 trackId, const QString &field, const QString &value) {
    if (!m_controller || trackId <= 0 || TagWriter::canonicalField(field).isEmpty())
        return false;
    const QList<TagEdit> edits{TagEdit{trackId, field, value, {}, {}}};
    const QString sandbox = m_controller->writeSandbox();
    return m_controller->runJob(
        "tag", field == "rating" ? tr("Saving the rating") : tr("Saving the title"),
        [edits, sandbox](Library &library, Job *job) { return runEdits(library, job, edits, sandbox, "edit"); },
        [this](const QJsonObject &r) {
            const QJsonArray files = r.value("files").toArray();
            const QJsonObject f = files.isEmpty() ? QJsonObject() : files.first().toObject();
            QString text;
            if (!f.value("ok").toBool(true))
                text = tr("Couldn't save: %1").arg(f.value("error").toString());
            else if (r.value("changed").toInt() > 0)
                text = tr("Saved  ·  Ctrl+Z undoes");
            m_controller->refreshAll();
            if (!text.isEmpty())
                emit m_controller->toast(text);
        });
}

void TagEditor::close() {
    ++m_generation; // a load still running is dropped
    m_ids.clear();
    m_original.clear();
    m_edits.clear();
    m_picture.clear();
    m_newPicture.clear();
    m_artworkAction = "keep";
    m_failures.clear();
    m_resultText.clear();
    emit loadedChanged();
    emit appliedChanged();
    bump();
}
