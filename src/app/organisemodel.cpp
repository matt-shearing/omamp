#include "organisemodel.h"

#include "librarycontroller.h"
#include "library.h"
#include "portal.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

namespace {

const char *const defaultMask = "<Album Artist>/<Album> (<Year>)/<Disc#>-<Track#:2> <Title>";

bool existsOnDisk(const QString &path) {
    return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
}

} // namespace

OrganiseModel::OrganiseModel(LibraryController *controller, QObject *parent)
    : QAbstractListModel(parent), m_controller(controller) {
    m_options.mask = defaultMask;
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &OrganiseModel::replan);
}

void OrganiseModel::setPicker(FolderPicker *picker) {
    m_picker = picker;
    picker->setup(tr("Choose where to organise the files"), tr("Choose"), true, false);
    connect(picker, &FolderPicker::picked, this, [this](const QStringList &paths) {
        if (!paths.isEmpty())
            setDestination(paths.first());
    });
    connect(picker, &FolderPicker::failed, this, &OrganiseModel::message);
}

QVariantList OrganiseModel::presets() const {
    return {
        QVariantMap{{"name", tr("Album artist, album and year, disc and track")}, {"mask", QString(defaultMask)}},
        QVariantMap{{"name", tr("Album artist and album, track")},
                    {"mask", "<Album Artist>/<Album>/<Track#:2> <Title>"}},
        QVariantMap{{"name", tr("Grouped by first letter")},
                    {"mask", "<Album Artist@1>/<Album Artist>/<Album> (<Year>)/<Track#:2> <Title>"}},
        QVariantMap{{"name", tr("Compilations in their own folder")},
                    {"mask", "$If(<Album Artist>=Various Artists,Compilations/<Album> (<Year>)/<Track#:2> <Artist> - "
                             "<Title>,<Album Artist>/<Album> (<Year>)/<Track#:2> <Title>)"}},
        QVariantMap{{"name", tr("Genre, then album artist")},
                    {"mask", "$First(<Genre>)/<Album Artist>/<Album>/<Track#:2> <Title>"}},
    };
}

int OrganiseModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_plan.items.size(); }

QHash<int, QByteArray> OrganiseModel::roleNames() const {
    return {{OldPathRole, "oldPath"}, {NewPathRole, "newPath"}, {StatusRole, "status"},  {StatusTextRole, "statusText"},
            {NoteRole, "note"},       {EditedRole, "edited"},   {TrackIdRole, "trackId"}};
}

QString OrganiseModel::statusText(const OrganiseItem &item) {
    const QString s = item.status;
    if (s == "move")
        return tr("Move");
    if (s == "renamed")
        return tr("Move, renamed");
    if (s == "unchanged")
        return tr("Already there");
    if (s == "conflict")
        return tr("Conflict");
    if (s == "offline")
        return tr("Offline");
    if (s == "missing")
        return tr("Missing");
    if (s == "moved")
        return tr("Moved");
    if (s == "failed")
        return tr("Failed");
    if (s == "skipped")
        return tr("Left alone");
    return tr("Error");
}

QVariant OrganiseModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_plan.items.size())
        return {};
    const OrganiseItem &item = m_plan.items[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case NewPathRole: return item.newPath;
    case OldPathRole: return item.oldPath;
    case StatusRole: return item.status;
    case StatusTextRole: return statusText(item);
    case NoteRole: return item.note;
    case EditedRole: return m_editedRows.contains(index.row());
    case TrackIdRole: return item.trackId;
    }
    return {};
}

int OrganiseModel::moves() const {
    int n = 0;
    for (const OrganiseItem &i : m_plan.items)
        n += i.moves() || i.status == "moved";
    return n;
}

int OrganiseModel::unchanged() const {
    int n = 0;
    for (const OrganiseItem &i : m_plan.items)
        n += i.status == "unchanged";
    return n;
}

int OrganiseModel::conflicts() const {
    int n = 0;
    for (const OrganiseItem &i : m_plan.items)
        n += i.status == "conflict" || i.status == "renamed";
    return n;
}

int OrganiseModel::problems() const {
    int n = 0;
    for (const OrganiseItem &i : m_plan.items)
        n += i.status == "error" || i.status == "offline" || i.status == "missing" || i.status == "failed";
    return n;
}

int OrganiseModel::companions() const {
    int n = 0;
    for (const OrganiseItem &i : m_plan.items)
        if (i.moves())
            for (const CompanionMove &c : i.companions)
                n += !c.skip;
    for (const CompanionMove &c : m_plan.folderCompanions)
        n += !c.skip;
    return n;
}

void OrganiseModel::setMask(const QString &mask) {
    if (mask == m_options.mask)
        return;
    m_options.mask = mask;
    emit optionsChanged();
    m_debounce.start();
}

void OrganiseModel::setDestination(const QString &folder) {
    const QString clean = folder.trimmed().isEmpty() ? QString() : QDir::cleanPath(folder.trimmed());
    if (clean == m_options.destination)
        return;
    m_options.destination = clean;
    emit optionsChanged();
    m_debounce.start();
}

void OrganiseModel::setMoveCompanions(bool on) {
    if (on == m_options.moveCompanions)
        return;
    m_options.moveCompanions = on;
    emit optionsChanged();
    m_debounce.start();
}

void OrganiseModel::setRemoveEmptyFolders(bool on) {
    if (on == m_options.removeEmptyFolders)
        return;
    m_options.removeEmptyFolders = on;
    emit optionsChanged();
}

bool OrganiseModel::load(const QVariantList &ids) {
    if (!m_controller || !m_controller->activate())
        return false;
    m_ids.clear();
    for (const QVariant &v : ids)
        if (v.toLongLong() > 0 && !m_ids.contains(v.toLongLong()))
            m_ids << v.toLongLong();
    if (m_ids.isEmpty())
        return false;
    // The mask used last time, kept in the library.
    if (Library *library = m_controller->library()) {
        const QString last = library->setting("organise.mask").toString();
        if (!last.isEmpty() && last != m_options.mask) {
            m_options.mask = last;
            emit optionsChanged();
        }
    }
    m_applied = false;
    m_resultText.clear();
    m_lastBatch.clear();
    emit appliedChanged();
    replan();
    return true;
}

void OrganiseModel::replan() {
    m_debounce.stop();
    if (!m_controller || !m_controller->library() || m_applying)
        return;
    Organiser organiser(*m_controller->library());
    organiser.setSandbox(m_controller->writeSandbox());
    beginResetModel();
    m_editedRows.clear();
    m_plan = m_ids.isEmpty() ? OrganisePlan() : organiser.plan(m_ids, m_options);
    if (m_applied) {
        m_applied = false;
        m_resultText.clear();
        emit appliedChanged();
    }
    endResetModel();
    emit planned();
}

void OrganiseModel::setNewPath(int row, const QString &path) {
    if (row < 0 || row >= m_plan.items.size() || m_applied)
        return;
    OrganiseItem &item = m_plan.items[row];
    if (!(item.moves() || item.status == "unchanged" || item.status == "conflict"))
        return;
    QString clean = QDir::cleanPath(path.trimmed());
    // A path without a folder stays in the folder planned for it.
    if (!clean.contains('/'))
        clean = QFileInfo(item.newPath).absolutePath() + '/' + clean;
    // The file keeps its type, whatever was typed.
    const QString suffix = QFileInfo(item.oldPath).suffix();
    if (!suffix.isEmpty() && QFileInfo(clean).suffix().compare(suffix, Qt::CaseInsensitive) != 0)
        clean += '.' + suffix;
    item.newPath = clean;
    item.note.clear();
    item.status = clean == item.oldPath ? "unchanged" : "move";
    m_editedRows.insert(row);
    // The .lrc follows the new name.
    for (CompanionMove &c : item.companions) {
        const QFileInfo to(clean);
        c.to = to.absolutePath() + '/' + to.completeBaseName() + ".lrc";
        c.skip = existsOnDisk(c.to);
    }
    markConflicts();
    emit dataChanged(index(0), index(m_plan.items.size() - 1));
    emit planned();
}

// After a hand edit: a new path that is taken on disk, or that two moving
// rows share, is a conflict, and that track stays where it is. A folder's
// pictures go along only while every moving track of the folder still goes
// to one place.
void OrganiseModel::markConflicts() {
    QHash<QString, int> claims;
    for (const OrganiseItem &i : std::as_const(m_plan.items))
        if (i.moves() || i.status == "unchanged" || i.status == "conflict")
            ++claims[i.newPath];
    for (int row = 0; row < m_plan.items.size(); ++row) {
        OrganiseItem &i = m_plan.items[row];
        if (!(i.moves() || i.status == "conflict"))
            continue;
        const bool taken = i.newPath != i.oldPath && existsOnDisk(i.newPath);
        const bool shared = claims.value(i.newPath) > 1;
        if (taken || shared) {
            i.status = "conflict";
            i.note = taken ? tr("%1 is already there").arg(QFileInfo(i.newPath).fileName())
                           : tr("Another track would get the same name");
        } else if (i.status == "conflict") {
            i.status = i.newPath == i.oldPath ? "unchanged" : "move";
            i.note.clear();
        }
    }
    QHash<QString, QSet<QString>> targets;
    for (const OrganiseItem &i : std::as_const(m_plan.items))
        if (i.moves())
            targets[QFileInfo(i.oldPath).absolutePath()].insert(QFileInfo(i.newPath).absolutePath());
    for (CompanionMove &c : m_plan.folderCompanions) {
        const QSet<QString> to = targets.value(QFileInfo(c.from).absolutePath());
        if (to.size() != 1 || *to.constBegin() != QFileInfo(c.to).absolutePath()) {
            c.skip = true;
            c.note = tr("The folder's tracks no longer go to one place");
        }
    }
}

bool OrganiseModel::apply() {
    if (!m_controller || m_applying || m_applied || moves() == 0)
        return false;
    if (Library *library = m_controller->library())
        library->setSetting("organise.mask", m_options.mask);
    const OrganisePlan plan = m_plan;
    const OrganiseOptions options = m_options;
    const QString sandbox = m_controller->writeSandbox();
    const bool started = m_controller->runJob(
        "organise", tr("Organising %n files", "", moves()),
        [plan, options, sandbox](Library &library, Job *job) {
            Organiser organiser(library, job);
            organiser.setSandbox(sandbox);
            return organiser.apply(plan, options).toJson();
        },
        [this](const QJsonObject &r) {
            m_applying = false;
            // The rows show what happened.
            const QJsonArray items = r.value("items").toArray();
            beginResetModel();
            for (int k = 0; k < items.size() && k < m_plan.items.size(); ++k) {
                const QJsonObject o = items[k].toObject();
                m_plan.items[k].status = o.value("status").toString(m_plan.items[k].status);
                m_plan.items[k].note = o.value("note").toString();
                if (o.contains("new_path") && !o.value("new_path").toString().isEmpty())
                    m_plan.items[k].newPath = o.value("new_path").toString();
            }
            endResetModel();
            m_applied = true;
            m_lastBatch = r.value("batch").toString();
            const int moved = r.value("moved").toInt(), failed = r.value("failed").toInt();
            QStringList parts{tr("%n files moved", "", moved)};
            if (r.value("companions_moved").toInt() > 0)
                parts << tr("%n other files went along", "", r.value("companions_moved").toInt());
            if (r.value("folders_removed").toInt() > 0)
                parts << tr("%n empty folders removed", "", r.value("folders_removed").toInt());
            if (failed > 0)
                parts << tr("%n failed", "", failed);
            if (r.value("cancelled").toBool())
                parts << tr("cancelled");
            m_resultText = parts.join(", ");
            if (!r.value("ok").toBool(true) && r.contains("error") && moved == 0)
                m_resultText = tr("Nothing was moved: %1").arg(r.value("error").toString());
            emit appliedChanged();
            emit planned();
            m_controller->refreshAll();
            emit m_controller->toast(m_resultText + (moved > 0 ? "  ·  " + tr("Ctrl+Z undoes") : QString()));
        });
    if (started) {
        m_applying = true;
        emit appliedChanged();
    }
    return started;
}

bool OrganiseModel::undo() {
    if (!m_controller || m_lastBatch.isEmpty())
        return false;
    const bool ok = m_controller->undoBatch(m_lastBatch);
    if (ok) {
        m_lastBatch.clear();
        m_resultText = tr("Putting the files back…");
        emit appliedChanged();
    }
    return ok;
}

void OrganiseModel::pickDestination() {
    if (m_picker)
        m_picker->pickFolders();
    else
        emit message(tr("No folder picker is available"));
}

void OrganiseModel::close() {
    m_debounce.stop();
    beginResetModel();
    m_ids.clear();
    m_plan = OrganisePlan();
    m_editedRows.clear();
    endResetModel();
    m_applied = false;
    m_resultText.clear();
    emit appliedChanged();
    emit planned();
}

QVariantList OrganiseModel::completions(const QString &text, int cursor) const {
    QVariantList out;
    cursor = qBound(0, cursor, int(text.size()));
    const QString before = text.left(cursor);
    const int angle = before.lastIndexOf('<');
    const int closeAngle = before.lastIndexOf('>');
    const int dollar = before.lastIndexOf('$');
    if (angle >= 0 && angle > closeAngle && angle >= dollar) {
        const QString typed = before.mid(angle + 1);
        for (const QString &name : Mask::fieldNames()) {
            const QString bare = name.mid(1, name.size() - 2);
            if (bare.startsWith(typed, Qt::CaseInsensitive))
                out << QVariantMap{{"label", name}, {"insert", name}, {"start", angle}};
        }
    } else if (dollar >= 0 && dollar > closeAngle) {
        const QString typed = before.mid(dollar + 1);
        bool word = true;
        for (const QChar c : typed)
            word = word && c.isLetter();
        if (word)
            for (const QString &name : Mask::functionNames())
                if (name.mid(1).startsWith(typed, Qt::CaseInsensitive))
                    out << QVariantMap{{"label", name + "(…)"}, {"insert", name + "("}, {"start", dollar}};
    }
    return out;
}

QString OrganiseModel::sample(const QString &mask) const {
    if (!m_controller || !m_controller->library() || m_ids.isEmpty())
        return {};
    QString error;
    const Mask::Values values = Mask::fromTrack(*m_controller->library(), m_ids.first());
    return Mask::evaluate(mask, values, Mask::Context(), &error);
}
