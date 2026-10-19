#include "duplicatesmodel.h"

#include "duplicates.h"
#include "librarycontroller.h"
#include "tracklistmodel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>

DuplicatesModel::DuplicatesModel(LibraryController *controller, QObject *parent)
    : QAbstractListModel(parent), m_controller(controller) {
    // Tracks removed elsewhere drop out of the groups.
    connect(controller, &LibraryController::tracksRemoved, this, [this] {
        if (m_rows.isEmpty() || !m_controller || !m_controller->library())
            return;
        QVariantList gone;
        for (const Row &r : std::as_const(m_rows))
            if (!r.group && m_controller->trackInfo(r.trackId).isEmpty())
                gone << r.trackId;
        if (!gone.isEmpty())
            prune(gone);
    });
}

int DuplicatesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }

QHash<int, QByteArray> DuplicatesModel::roleNames() const {
    return {{KindRole, "kind"},       {LabelRole, "label"},         {DetailRole, "detail"},     {ReasonRole, "reason"},
            {PathRole, "path"},       {FormatRole, "format"},       {LengthRole, "length"},     {PlaysRole, "plays"},
            {RatingRole, "rating"},   {KeeperRole, "keeper"},       {SelectedRole, "rowSelected"},
            {AvailableRole, "available"}, {TrackIdRole, "trackId"}, {GroupRole, "group"}};
}

QVariant DuplicatesModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows[index.row()];
    switch (role) {
    case KindRole: return r.group ? QStringLiteral("group") : QStringLiteral("track");
    case Qt::DisplayRole:
    case LabelRole: return r.label;
    case DetailRole: return r.detail;
    case ReasonRole: return r.reason;
    case PathRole: return r.path;
    case FormatRole: return r.format;
    case LengthRole: return r.durationMs > 0 ? TrackListModel::clock(r.durationMs) : QString();
    case PlaysRole: return r.plays;
    case RatingRole: return r.rating;
    case KeeperRole: return r.keeper;
    case SelectedRole: return !r.group && m_selected.contains(r.trackId);
    case AvailableRole: return r.available;
    case TrackIdRole: return r.trackId;
    case GroupRole: return r.groupIndex;
    }
    return {};
}

void DuplicatesModel::setBy(const QString &by) {
    if (by == m_by || !(by == "tags" || by == "content" || by == "fingerprint"))
        return;
    m_by = by;
    emit byChanged();
}

int DuplicatesModel::trackCount() const {
    int n = 0;
    for (const Row &r : m_rows)
        n += !r.group;
    return n;
}

bool DuplicatesModel::fingerprintAvailable() const { return !Fingerprinter::findProgram().isEmpty(); }

bool DuplicatesModel::find() {
    if (!m_controller || m_busy)
        return false;
    Duplicates::Options options;
    Duplicates::parse(m_by, &options.by);
    const bool bySound = options.by == Duplicates::By::Fingerprint;
    if (bySound && !fingerprintAvailable()) {
        emit message(tr("Finding duplicates by sound needs fpcalc, from the chromaprint package"));
        return false;
    }
    const QString sandbox = m_controller->writeSandbox();
    const bool started = m_controller->runJob(
        "duplicates", bySound ? tr("Listening for duplicates") : tr("Looking for duplicates"),
        [options, bySound, sandbox](Library &library, Job *job) {
            QJsonObject fingerprints;
            if (bySound) {
                Fingerprinter fp(library, job);
                fp.setSandbox(sandbox);
                fingerprints = fp.run().toJson();
            }
            QJsonObject o = Duplicates::find(library, options).toJson();
            if (!fingerprints.isEmpty())
                o["fingerprints"] = fingerprints;
            return o;
        },
        [this](const QJsonObject &r) {
            m_busy = false;
            emit busyChanged();
            beginResetModel();
            m_rows.clear();
            m_selected.clear();
            const QJsonArray groups = r.value("groups").toArray();
            const QLocale locale;
            for (int g = 0; g < groups.size(); ++g) {
                const QJsonObject group = groups[g].toObject();
                const QJsonArray tracks = group.value("tracks").toArray();
                const QJsonObject first = tracks.isEmpty() ? QJsonObject() : tracks.first().toObject();
                Row head;
                head.group = true;
                head.groupIndex = g;
                const QString artist = first.value("artist").toString(), title = first.value("title").toString();
                head.label = artist.isEmpty() ? title : QString("%1 — %2").arg(artist, title);
                head.detail = tr("%n copies", "", int(tracks.size()));
                head.reason = group.value("reason").toString();
                m_rows << head;
                for (const QJsonValue &v : tracks) {
                    const QJsonObject t = v.toObject();
                    Row row;
                    row.groupIndex = g;
                    row.trackId = t.value("id").toInteger();
                    row.label = t.value("title").toString();
                    row.detail = QStringList{t.value("artist").toString(), t.value("album").toString()}.join("  ·  ");
                    row.path = t.value("path").toString();
                    const QString format = t.value("format").toString().toUpper();
                    const int bitrate = t.value("bitrate").toInt();
                    row.format = bitrate > 0 ? QString("%1  ·  %2 kbps").arg(format).arg(bitrate) : format;
                    row.durationMs = t.value("duration_ms").toInteger();
                    row.plays = t.value("plays").toInt();
                    row.rating = t.value("rating").isNull() ? -1 : t.value("rating").toInt();
                    row.keeper = t.value("keeper").toBool();
                    row.available = t.value("availability").toString() == QLatin1String("online");
                    m_rows << row;
                }
            }
            m_groups = groups.size();
            m_searched = true;
            endResetModel();
            if (!r.value("ok").toBool(true))
                m_summary = tr("Couldn't look for duplicates: %1").arg(r.value("error").toString());
            else if (m_groups == 0)
                m_summary = tr("No duplicates found");
            else
                m_summary = tr("%n groups", "", m_groups) + ", " + tr("%n tracks", "", trackCount());
            const int unprinted = r.value("without_fingerprint").toInt();
            if (unprinted > 0)
                m_summary += "  ·  " + tr("%n tracks couldn't be fingerprinted (too short or unreadable)", "", unprinted);
            emit found();
            emit selectionChanged();
        });
    if (started) {
        m_busy = true;
        emit busyChanged();
    }
    return started;
}

void DuplicatesModel::selectionRowsChanged() {
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {SelectedRole});
    emit selectionChanged();
}

void DuplicatesModel::toggle(int row) {
    if (row < 0 || row >= m_rows.size() || m_rows[row].group)
        return;
    setSelected(row, !m_selected.contains(m_rows[row].trackId));
}

void DuplicatesModel::setSelected(int row, bool on) {
    if (row < 0 || row >= m_rows.size() || m_rows[row].group)
        return;
    const qint64 id = m_rows[row].trackId;
    if (on == m_selected.contains(id))
        return;
    if (on)
        m_selected.insert(id);
    else
        m_selected.remove(id);
    emit dataChanged(index(row), index(row), {SelectedRole});
    emit selectionChanged();
}

void DuplicatesModel::selectNonKeepers() {
    m_selected.clear();
    for (const Row &r : std::as_const(m_rows))
        if (!r.group && !r.keeper)
            m_selected.insert(r.trackId);
    selectionRowsChanged();
}

void DuplicatesModel::clearSelection() {
    m_selected.clear();
    selectionRowsChanged();
}

QVariantList DuplicatesModel::selectedIds() const {
    QVariantList out;
    for (const Row &r : m_rows)
        if (!r.group && m_selected.contains(r.trackId))
            out << r.trackId;
    return out;
}

void DuplicatesModel::prune(const QVariantList &goneIds) {
    QSet<qint64> gone;
    for (const QVariant &v : goneIds)
        gone.insert(v.toLongLong());
    QList<Row> kept;
    for (const Row &r : std::as_const(m_rows))
        if (r.group || !gone.contains(r.trackId))
            kept << r;
    // A group with one track left is no longer a duplicate.
    QList<Row> out;
    int groups = 0;
    for (int i = 0; i < kept.size(); ++i) {
        if (!kept[i].group)
            continue;
        int end = i + 1;
        while (end < kept.size() && !kept[end].group)
            ++end;
        if (end - i - 1 >= 2) {
            Row head = kept[i];
            head.detail = tr("%n copies", "", end - i - 1);
            out << head;
            for (int k = i + 1; k < end; ++k)
                out << kept[k];
            ++groups;
        }
        i = end - 1;
    }
    beginResetModel();
    m_rows = out;
    m_groups = groups;
    for (qint64 id : gone)
        m_selected.remove(id);
    endResetModel();
    m_summary = groups == 0 ? tr("No duplicates left") : tr("%n groups", "", groups) + ", " + tr("%n tracks", "", trackCount());
    emit found();
    emit selectionChanged();
}
