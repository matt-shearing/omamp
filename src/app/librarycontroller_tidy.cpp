// LibraryController, Phase 2: jobs that write, undo and redo, the journal,
// removing tracks, the trash, dates, and the Files to Edit counts.

#include "librarycontroller.h"

#include "audit.h"
#include "dates.h"
#include "job.h"
#include "library.h"
#include "organise.h"
#include "ownwrites.h"
#include "tagwriter.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

QString LibraryController::dateText(const QDate &date) { return Dates::text(date); }

QString LibraryController::formatDate(qint64 secondsSinceEpoch) const {
    if (secondsSinceEpoch <= 0)
        return {};
    return dateText(QDateTime::fromSecsSinceEpoch(secondsSinceEpoch).date());
}

QString LibraryController::formatDateTime(qint64 secondsSinceEpoch) const {
    if (secondsSinceEpoch <= 0)
        return {};
    const QDateTime when = QDateTime::fromSecsSinceEpoch(secondsSinceEpoch);
    const QLocale locale;
    return QString("%1, %2").arg(dateText(when.date()), locale.toString(when.time(), QLocale::ShortFormat));
}

QVariantList LibraryController::selectedTrackIds() const {
    QVariantList out;
    for (int row : m_tracks.selectedRows()) {
        const qint64 id = m_tracks.trackId(row);
        if (id > 0)
            out << id;
    }
    return out;
}

QVariantList LibraryController::albumTrackIds(qint64 albumId) const {
    QVariantList out;
    if (!m_library || albumId <= 0)
        return out;
    QSqlQuery q(m_library->db());
    q.setForwardOnly(true);
    q.prepare("SELECT id FROM tracks WHERE album_id = ? ORDER BY disc_no, track_no, rel_path");
    q.addBindValue(albumId);
    if (q.exec())
        while (q.next())
            out << q.value(0).toLongLong();
    return out;
}

QVariantList LibraryController::auditList() const {
    QVariantList out;
    for (const Audits::Audit &a : Audits::audits())
        out << QVariantMap{{"id", a.id}, {"title", a.title}, {"description", a.description}};
    return out;
}

namespace {

struct Chain {
    int depth = 0;
    QString root; // the batch at depth 0
};

Chain chainOf(const QHash<QString, QString> &sourceOf, const QString &batch) {
    Chain c;
    c.root = batch;
    while (c.depth < 1000) {
        const QString source = sourceOf.value(c.root);
        if (!source.startsWith(QLatin1String("undo:")))
            break;
        const QString target = source.mid(5);
        ++c.depth;
        c.root = target;
        // The batch it undid is older than the list: take it as a change.
        if (!sourceOf.contains(target))
            break;
    }
    return c;
}

QString fieldWord(const QString &field) {
    static const QHash<QString, QString> words = {
        {"albumartist", "album artist"}, {"originalyear", "original year"}, {"track", "track number"},
        {"tracktotal", "track total"},   {"disc", "disc number"},           {"disctotal", "disc total"},
        {"bpm", "BPM"},                  {"folder_removed", "folder"}};
    return words.value(field, field);
}

} // namespace

QString LibraryController::describeBatch(Library &library, const QString &batch) {
    const QList<Journal::Entry> entries = Journal::entries(library, batch);
    if (entries.isEmpty())
        return tr("an earlier change");
    const QString source = entries.first().source;
    QSet<qint64> tracks;
    QStringList fields;
    bool organise = false;
    for (const Journal::Entry &e : entries) {
        if (e.field == QLatin1String("path") || e.field == QLatin1String("companion") ||
            e.field == QLatin1String("folder_removed")) {
            organise = true;
            if (e.field == QLatin1String("path"))
                tracks.insert(e.trackId);
            continue;
        }
        if (e.trackId)
            tracks.insert(e.trackId);
        if (!fields.contains(e.field))
            fields << e.field;
    }
    const int n = int(tracks.size());
    if (organise)
        return tr("organise of %n files", "", n);
    if (source == QLatin1String("tag-from-filename"))
        return tr("tags from file names for %n tracks", "", n);
    QStringList words;
    for (const QString &f : std::as_const(fields))
        words << fieldWord(f);
    QString list = words.mid(0, 3).join(", ");
    if (words.size() > 3)
        list += ", …";
    return tr("tag edit of %n tracks", "", n) + (list.isEmpty() ? QString() : QString(" (%1)").arg(list));
}

LibraryController::UndoTargets LibraryController::undoTargets(Library &library) {
    UndoTargets t;
    const QList<Journal::Batch> list = Journal::batches(library, 500); // newest first
    QHash<QString, QString> sourceOf;
    for (const Journal::Batch &b : list)
        sourceOf.insert(b.id, b.source);
    bool newChangeSeen = false;
    for (const Journal::Batch &b : list) {
        const Chain c = chainOf(sourceOf, b.id);
        if (!b.undone()) {
            if (t.undo.isEmpty() && c.depth % 2 == 0) {
                t.undo = b.id;
                t.undoLabel = describeBatch(library, c.root);
            }
            if (t.redo.isEmpty() && c.depth % 2 == 1 && !newChangeSeen) {
                t.redo = b.id;
                t.redoLabel = describeBatch(library, c.root);
            }
        }
        // A new change, even one undone since, ends what can be redone.
        if (c.depth == 0)
            newChangeSeen = true;
        if (!t.undo.isEmpty() && (!t.redo.isEmpty() || newChangeSeen))
            break;
    }
    return t;
}

void LibraryController::refreshUndo() {
    QString undo, redo;
    if (m_library) {
        const UndoTargets t = undoTargets(*m_library);
        undo = t.undoLabel;
        redo = t.redoLabel;
    }
    if (undo != m_undoText || redo != m_redoText) {
        m_undoText = undo;
        m_redoText = redo;
        emit undoChanged();
    }
}

bool LibraryController::undo() {
    if (!activate())
        return false;
    // While a job runs (a save, say), what Ctrl+Z means is decided when its
    // turn comes, so it undoes that save rather than the change before it.
    if (m_scanThread) {
        enqueue("undo", [this] { undo(); });
        return true;
    }
    const UndoTargets t = undoTargets(*m_library);
    if (t.undo.isEmpty()) {
        emit toast(tr("Nothing to undo"));
        return false;
    }
    return runUndo(t.undo, t.undoLabel, false);
}

bool LibraryController::redo() {
    if (!activate())
        return false;
    if (m_scanThread) {
        enqueue("undo", [this] { redo(); });
        return true;
    }
    const UndoTargets t = undoTargets(*m_library);
    if (t.redo.isEmpty()) {
        emit toast(tr("Nothing to redo"));
        return false;
    }
    return runUndo(t.redo, t.redoLabel, true);
}

bool LibraryController::undoBatch(const QString &batch) {
    if (!activate() || batch.isEmpty())
        return false;
    return runUndo(batch, describeBatch(*m_library, batch), false);
}

bool LibraryController::runUndo(const QString &batch, const QString &label, bool redo) {
    const QString sandbox = m_sandbox;
    return runJob(
        "undo", redo ? tr("Redoing %1").arg(label) : tr("Undoing %1").arg(label),
        [batch, sandbox](Library &library, Job *job) {
            QJsonObject o;
            if (Journal::isOrganise(library, batch)) {
                Organiser organiser(library, job);
                organiser.setSandbox(sandbox);
                o = organiser.undo(batch).toJson();
                o["kind"] = "organise";
            } else {
                TagWriter writer(library, job);
                writer.setSandbox(sandbox);
                o = writer.undo(batch).toJson();
                o["kind"] = "tag";
            }
            o["undid"] = batch;
            return o;
        },
        [this, label, redo](const QJsonObject &r) {
            const int failed = r.value("failed").toInt();
            QString text = redo ? tr("Redone: %1").arg(label) : tr("Undone: %1").arg(label);
            if (!r.value("ok").toBool(true) && failed == 0 && r.contains("error"))
                text = (redo ? tr("Couldn't redo: %1") : tr("Couldn't undo: %1")).arg(r.value("error").toString());
            else if (failed > 0)
                text += "  " + tr("(%n files failed)", "", failed);
            setMessage(text);
            emit toast(text);
            refreshAll();
        });
}

QVariantList LibraryController::journalBatches(int limit) const {
    QVariantList out;
    if (!m_library)
        return out;
    const QList<Journal::Batch> list = Journal::batches(*m_library, limit);
    QHash<QString, QString> sourceOf;
    for (const Journal::Batch &b : list)
        sourceOf.insert(b.id, b.source);
    for (const Journal::Batch &b : list) {
        const Chain c = chainOf(sourceOf, b.id);
        const QString what = describeBatch(*m_library, c.root);
        QString label = c.depth == 0 ? what : c.depth % 2 ? tr("Undo of %1").arg(what) : tr("Redo of %1").arg(what);
        if (!label.isEmpty())
            label[0] = label[0].toUpper();
        out << QVariantMap{{"batch", b.id},
                           {"kind", b.kind()},
                           {"label", label},
                           {"when", formatDateTime(b.changedAt)},
                           {"tracks", b.tracks},
                           {"entries", b.entries},
                           {"undone", b.undone()},
                           {"canUndo", !b.undone()}};
    }
    return out;
}

QVariantList LibraryController::journalEntries(const QString &batch) const {
    QVariantList out;
    if (!m_library)
        return out;
    QSqlQuery title(m_library->db());
    title.prepare("SELECT coalesce(nullif(title, ''), file_name) FROM tracks WHERE id = ?");
    for (const Journal::Entry &e : Journal::entries(*m_library, batch)) {
        QString name;
        if (e.trackId) {
            title.addBindValue(e.trackId);
            if (title.exec() && title.next())
                name = title.value(0).toString();
        }
        auto shown = [&](const QString &v) {
            if (e.field == QLatin1String("artwork"))
                return v.isEmpty() ? tr("no picture") : tr("a picture");
            return v;
        };
        out << QVariantMap{{"track", e.trackId},
                           {"title", !name.isEmpty() ? name : e.trackId ? tr("(track %1)").arg(e.trackId) : tr("(other files)")},
                           {"field", fieldWord(e.field)},
                           {"oldValue", shown(e.oldValue)},
                           {"newValue", shown(e.newValue)},
                           {"undone", e.undone}};
    }
    return out;
}

int LibraryController::removeFromLibrary(const QVariantList &ids) {
    if (!m_library || ids.isEmpty())
        return 0;
    QSqlDatabase db = m_library->db();
    db.transaction();
    QSqlQuery q(db);
    q.prepare("DELETE FROM tracks WHERE id = ?");
    int removed = 0;
    for (const QVariant &id : ids) {
        q.addBindValue(id.toLongLong());
        if (q.exec())
            removed += q.numRowsAffected();
    }
    // Albums, artists and genres nothing points at any more go too, as
    // after a scan.
    QSqlQuery tidy(db);
    tidy.exec("DELETE FROM albums WHERE id NOT IN (SELECT album_id FROM tracks WHERE album_id IS NOT NULL)");
    tidy.exec("DELETE FROM artists WHERE id NOT IN (SELECT artist_id FROM track_artists)");
    tidy.exec("DELETE FROM genres WHERE id NOT IN (SELECT genre_id FROM track_genres)");
    if (!db.commit()) {
        db.rollback();
        setMessage(tr("Couldn't remove the tracks: %1").arg(db.lastError().text()));
        return 0;
    }
    m_tracks.clearSelection();
    refreshAll();
    const QString text = tr("%n tracks removed from the library; the files are still on disk", "", removed);
    setMessage(text);
    emit toast(text);
    emit tracksRemoved();
    return removed;
}

bool LibraryController::moveToTrash(const QVariantList &ids) {
    if (!activate() || ids.isEmpty())
        return false;
    QList<qint64> list;
    for (const QVariant &v : ids)
        list << v.toLongLong();
    const QString sandbox = m_sandbox;
    auto trash = m_trash;
    return runJob(
        "trash", tr("Moving files to the trash"),
        [list, sandbox, trash](Library &library, Job *job) {
            QJsonArray trashed, failed;
            int done = 0;
            for (qint64 id : list) {
                if (job->isCancelled())
                    break;
                job->setProgress(QString(), done++, list.size());
                const QString path = library.absolutePath(id);
                QString error;
                if (path.isEmpty())
                    error = tr("the track is offline");
                else if (sandbox.isEmpty() ? false : !Journal::insideSandbox(sandbox, path))
                    error = tr("it is outside the allowed folder");
                else if (!QFileInfo::exists(path))
                    error = tr("the file is already gone");
                if (error.isEmpty()) {
                    // Our own change: the folder watcher leaves it to us.
                    OwnWrites::note(path);
                    if (!trash(path, &error) && error.isEmpty())
                        error = tr("the trash refused it");
                }
                if (error.isEmpty())
                    trashed << QJsonObject{{"id", id}, {"path", path}};
                else
                    failed << QJsonObject{{"id", id}, {"path", path}, {"error", error}};
            }
            return QJsonObject{{"ok", failed.isEmpty()}, {"trashed", trashed}, {"failed", failed}};
        },
        [this](const QJsonObject &r) {
            QVariantList gone;
            for (const QJsonValue &v : r.value("trashed").toArray())
                gone << v.toObject().value("id").toInteger();
            const QJsonArray failed = r.value("failed").toArray();
            // The files are in the trash, so their rows go too.
            if (!gone.isEmpty())
                removeFromLibrary(gone);
            QString text = tr("%n files moved to the trash", "", int(gone.size()));
            if (!failed.isEmpty())
                text += "  " + tr("(%n not moved: %1)", "", int(failed.size()))
                                   .arg(failed.first().toObject().value("error").toString());
            setMessage(text);
            emit toast(text);
        });
}

void LibraryController::countAudits() {
    if (!m_ready)
        return;
    if (m_auditThread) {
        // Counting already; count again when it ends, so the numbers are current.
        m_auditAgain = true;
        return;
    }
    const QString path = m_path;
    m_auditThread = QThread::create([this, path] {
        Library library(path);
        QHash<QString, qint64> counts;
        if (library.open())
            for (const Audits::Count &c : Audits::counts(library))
                counts.insert(c.audit.id, c.count);
        QMetaObject::invokeMethod(
            this,
            [this, counts] {
                if (m_auditThread) {
                    m_auditThread->wait();
                    delete m_auditThread;
                    m_auditThread = nullptr;
                }
                m_tree.setAuditCounts(counts);
                emit auditsCounted();
                if (m_auditAgain) {
                    m_auditAgain = false;
                    countAudits();
                }
            },
            Qt::QueuedConnection);
    });
    m_auditThread->setObjectName("omamp-audits");
    m_auditThread->start(QThread::LowPriority);
}
