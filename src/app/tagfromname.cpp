#include "tagfromname.h"

#include "librarycontroller.h"
#include "library.h"
#include "mask.h"
#include "tagwriter.h"

#include <QJsonArray>
#include <QJsonObject>

TagFromNameModel::TagFromNameModel(LibraryController *controller, QObject *parent)
    : QAbstractListModel(parent), m_controller(controller) {}

QVariantList TagFromNameModel::presets() const {
    return {QString("<Track#> - <Title>"), QString("<Track#> <Title>"), QString("<Artist> - <Title>"),
            QString("<Track#> - <Artist> - <Title>"), QString("<Album>/<Track#> - <Title>"),
            QString("<Album Artist>/<Album> (<Year>)/<Track#> <Title>")};
}

int TagFromNameModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }

QHash<int, QByteArray> TagFromNameModel::roleNames() const {
    return {{PathRole, "path"}, {MatchedRole, "matched"}, {ErrorRole, "error"}, {ValuesRole, "values"},
            {TrackIdRole, "trackId"}};
}

QVariant TagFromNameModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case PathRole: {
        // As much of the path as the pattern reads: the file name and one
        // folder per separator.
        const int folders = m_pattern.count('/') + m_pattern.count('\\');
        return r.path.section('/', -1 - folders);
    }
    case MatchedRole: return r.matched;
    case ErrorRole: return r.error;
    case ValuesRole: return r.values;
    case TrackIdRole: return r.id;
    }
    return {};
}

QStringList TagFromNameModel::fieldLabels() const {
    QHash<QString, QString> display;
    for (const QString &name : Mask::fieldNames()) {
        const QString bare = name.mid(1, name.size() - 2);
        display.insert(Mask::canonicalField(bare), bare);
    }
    QStringList out;
    for (const QString &f : m_fields)
        out << display.value(f, f);
    return out;
}

int TagFromNameModel::matched() const {
    int n = 0;
    for (const Row &r : m_rows)
        n += r.matched;
    return n;
}

void TagFromNameModel::setPattern(const QString &pattern) {
    if (pattern == m_pattern)
        return;
    m_pattern = pattern;
    emit patternChanged();
    preview();
}

bool TagFromNameModel::load(const QVariantList &ids) {
    if (!m_controller || !m_controller->activate() || !m_controller->library())
        return false;
    QList<Row> rows;
    for (const QVariant &v : ids) {
        Row r;
        r.id = v.toLongLong();
        r.path = m_controller->library()->absolutePath(r.id);
        if (r.id > 0)
            rows << r;
    }
    if (rows.isEmpty())
        return false;
    if (const QString last = m_controller->library()->setting("tagfromname.pattern").toString(); !last.isEmpty() &&
        last != m_pattern) {
        m_pattern = last;
        emit patternChanged();
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
    m_resultText.clear();
    emit appliedChanged();
    preview();
    return true;
}

void TagFromNameModel::preview() {
    // The fields, in the pattern's order.
    m_fields.clear();
    m_error.clear();
    m_errorPosition = -1;
    const Mask::Parsed parsed = Mask::parse(m_pattern, true);
    if (!parsed.ok()) {
        m_error = parsed.error;
        m_errorPosition = parsed.position;
    } else {
        for (const Mask::Node &n : parsed.nodes)
            if (n.kind == Mask::Node::Field && n.text != QLatin1String("skip") && !m_fields.contains(n.text))
                m_fields << n.text;
    }
    beginResetModel();
    for (Row &r : m_rows) {
        r.values.clear();
        r.matched = false;
        r.error.clear();
        if (r.path.isEmpty()) {
            r.error = tr("Offline");
            continue;
        }
        if (!m_error.isEmpty())
            continue;
        const Mask::FilenameMatch m = Mask::matchFilename(m_pattern, r.path);
        if (!m.ok) {
            if (m_error.isEmpty() && m.position >= 0) {
                // A problem with the pattern itself, not this file.
                m_error = m.error;
                m_errorPosition = m.position;
            }
            r.error = m.position >= 0 ? QString() : tr("Doesn't match");
            continue;
        }
        r.matched = true;
        for (const QString &f : std::as_const(m_fields))
            r.values << m.values.value(f);
    }
    endResetModel();
    emit previewed();
}

bool TagFromNameModel::apply() {
    if (!m_controller || m_applying || matched() == 0)
        return false;
    QList<TagEdit> edits;
    for (const Row &r : std::as_const(m_rows)) {
        if (!r.matched)
            continue;
        for (int k = 0; k < m_fields.size() && k < r.values.size(); ++k)
            edits << TagEdit{r.id, m_fields[k], r.values[k], {}, {}};
    }
    if (m_controller->library())
        m_controller->library()->setSetting("tagfromname.pattern", m_pattern);
    const QString sandbox = m_controller->writeSandbox();
    const bool started = m_controller->runJob(
        "tag", tr("Tagging %n files from their names", "", matched()),
        [edits, sandbox](Library &library, Job *job) {
            TagWriter writer(library, job);
            writer.setSandbox(sandbox);
            TagWriteOptions options;
            options.source = "tag-from-filename";
            return writer.apply(edits, options).toJson();
        },
        [this](const QJsonObject &r) {
            m_applying = false;
            const int changed = r.value("changed").toInt(), failed = r.value("failed").toInt();
            QString firstError;
            for (const QJsonValue &v : r.value("files").toArray())
                if (!v.toObject().value("ok").toBool(true) && firstError.isEmpty())
                    firstError = v.toObject().value("error").toString();
            m_resultText = tr("%n tracks tagged", "", changed);
            if (failed)
                m_resultText += ", " + tr("%n failed", "", failed) + ": " + firstError;
            emit appliedChanged();
            m_controller->refreshAll();
            emit m_controller->toast(m_resultText + (changed > 0 ? "  ·  " + tr("Ctrl+Z undoes") : QString()));
            emit applied(failed == 0);
        });
    if (started) {
        m_applying = true;
        emit appliedChanged();
    }
    return started;
}

void TagFromNameModel::close() {
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit previewed();
}
