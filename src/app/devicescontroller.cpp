#include "devicescontroller.h"

#include "devicesync.h"
#include "job.h"
#include "library.h"
#include "librarycontroller.h"
#include "playlists.h"

#include <QJsonArray>

namespace {

QString sizeText(qint64 bytes) {
    if (bytes < 0)
        return {};
    if (bytes >= 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 1) + " GB";
    if (bytes >= 1024LL * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 0) + " MB";
    return QString::number((bytes + 1023) / 1024) + " KB";
}

QString codecText(const QString &rule) {
    const QString codec = rule.section(':', 0, 0);
    const QString kbps = rule.section(':', 1);
    const QString name = codec == "opus" ? "Opus" : codec == "mp3" ? "MP3" : codec == "aac" ? "AAC"
                         : codec == "vorbis" ? "Ogg Vorbis" : codec == "flac" ? "FLAC" : codec;
    return kbps.isEmpty() ? name : QString("%1 %2 kbit/s").arg(name, kbps);
}

} // namespace

DevicesController::DevicesController(LibraryController *library) : QObject(library), m_controller(library) {
    m_picker.setup(tr("Sync to which folder?"), tr("Sync here"), true, false);
    connect(&m_picker, &FolderPicker::picked, this, [this](const QStringList &paths) {
        if (!paths.isEmpty())
            emit targetChosen(paths.first());
    });
    connect(&m_picker, &FolderPicker::failed, this, &DevicesController::message);
}

QString DevicesController::defaultMask() const { return DeviceSync::defaultMask; }

QVariantList DevicesController::rulePresets() const {
    auto preset = [](const QString &label, const QStringList &rules) {
        return QVariantMap{{"label", label}, {"rules", rules}};
    };
    return {preset(tr("Copy every file as it is"), {}),
            preset(tr("Lossless to Opus 128 kbit/s, the rest as it is"), {"lossless->opus:128"}),
            preset(tr("Lossless to MP3 256 kbit/s, the rest as it is"), {"lossless->mp3:256"}),
            preset(tr("Lossless to AAC 256 kbit/s, the rest as it is"), {"lossless->aac:256"}),
            preset(tr("Everything to Opus 96 kbit/s (smallest)"), {"*->opus:96"}),
            preset(tr("Everything to MP3 192 kbit/s (plays anywhere)"), {"*->mp3:192"})};
}

void DevicesController::reload() {
    Library *lib = m_controller->library();
    m_profiles.clear();
    if (lib) {
        for (const DeviceSync::Profile &p : DeviceSync::list(*lib)) {
            const DeviceSync::TargetState t = DeviceSync::checkTarget(*lib, p);
            QStringList sources, rules;
            for (const DeviceSync::Source &s : p.sources)
                sources << s.toString();
            for (const DeviceSync::FormatRule &r : p.rules)
                rules << r.toString();
            QString last = tr("Never synced");
            if (p.lastSync) {
                const QJsonObject r = p.lastResult;
                last = r.value("ok").toBool()
                           ? tr("Synced %1: %2 copied, %3 converted, %4 deleted")
                                 .arg(m_controller->formatDateTime(p.lastSync))
                                 .arg(r.value("copied").toInt())
                                 .arg(r.value("converted").toInt())
                                 .arg(r.value("deleted").toInt())
                           : tr("Last sync %1 failed: %2")
                                 .arg(m_controller->formatDateTime(p.lastSync), r.value("error").toString());
            }
            QString stateText = t.message;
            if (t.state == "ready")
                stateText = tr("Ready");
            else if (t.state == "missing")
                stateText = tr("The folder will be made");
            m_profiles << QVariantMap{{"id", p.id},
                                      {"name", p.name},
                                      {"kind", p.kind},
                                      {"target", p.target},
                                      {"sources", sources},
                                      {"rules", rules},
                                      {"mask", p.mask},
                                      {"deleteOthers", p.deleteOthers},
                                      {"writePlaylists", p.writePlaylists},
                                      {"ready", t.ready},
                                      {"state", t.state},
                                      {"stateText", stateText},
                                      {"freeText", t.freeBytes >= 0 ? tr("%1 free").arg(sizeText(t.freeBytes)) : QString()},
                                      {"lastSync", p.lastSync},
                                      {"lastText", last}};
        }
    }
    emit profilesChanged();
}

void DevicesController::refreshDevices() {
    m_devices.clear();
    for (const DeviceSync::Device &d : DeviceSync::detectDevices())
        m_devices << QVariantMap{{"kind", d.kind},
                                 {"path", d.path},
                                 {"label", d.label},
                                 {"freeText", d.freeBytes >= 0 ? tr("%1 free").arg(sizeText(d.freeBytes)) : QString()}};
    emit devicesChanged();
}

QVariantList DevicesController::sourceChoices() const {
    QVariantList out;
    Library *lib = m_controller->library();
    if (!lib)
        return out;
    out << QVariantMap{{"source", "library"}, {"label", tr("Entire Library")}, {"group", tr("Library")}};
    for (const Collections::Collection &c : Collections::list(*lib))
        out << QVariantMap{{"source", "collection:" + c.name}, {"label", c.name}, {"group", tr("Collections")}};
    for (const Playlists::Playlist &p : Playlists::list(*lib)) {
        if (p.kind == "folder")
            continue;
        out << QVariantMap{{"source", "playlist:" + p.name},
                           {"label", p.name},
                           {"group", p.kind == "auto" ? tr("Auto-playlists") : tr("Playlists")}};
    }
    return out;
}

qint64 DevicesController::saveProfile(const QVariantMap &m) {
    Library *lib = m_controller->library();
    m_lastError.clear();
    if (!lib)
        return 0;
    DeviceSync::Profile p;
    p.id = m.value("id").toLongLong();
    p.name = m.value("name").toString();
    p.kind = m.value("kind", "folder").toString();
    p.target = m.value("target").toString();
    p.mask = m.value("mask").toString().trimmed();
    if (p.mask == DeviceSync::defaultMask)
        p.mask.clear();
    p.deleteOthers = m.value("deleteOthers").toBool();
    p.writePlaylists = m.value("writePlaylists", true).toBool();
    QString why;
    for (const QString &s : m.value("sources").toStringList()) {
        const DeviceSync::Source src = DeviceSync::Source::fromString(s, &why);
        if (!why.isEmpty())
            break;
        p.sources << src;
    }
    for (const QString &r : m.value("rules").toStringList()) {
        if (!why.isEmpty() || r.trimmed().isEmpty())
            continue;
        const DeviceSync::FormatRule rule = DeviceSync::FormatRule::fromString(r, &why);
        if (why.isEmpty())
            p.rules << rule;
    }
    const qint64 id = why.isEmpty() ? DeviceSync::save(*lib, p, &why) : 0;
    if (!id) {
        m_lastError = why;
        emit message(why);
        return 0;
    }
    reload();
    m_controller->reloadTree();
    return id;
}

bool DevicesController::removeProfile(qint64 id) {
    Library *lib = m_controller->library();
    QString why;
    if (!lib || !DeviceSync::remove(*lib, id, &why)) {
        emit message(why);
        return false;
    }
    if (m_preview.value("profileId").toLongLong() == id)
        clearPreview();
    reload();
    m_controller->reloadTree();
    return true;
}

QString DevicesController::nameOf(qint64 id) const {
    for (const QVariant &v : m_profiles)
        if (v.toMap().value("id").toLongLong() == id)
            return v.toMap().value("name").toString();
    return {};
}

QVariantMap DevicesController::previewFrom(qint64 id, const QJsonObject &plan) const {
    QVariantList rows;
    for (const QJsonValue &v : plan.value("actions").toArray()) {
        const QJsonObject a = v.toObject();
        const QString op = a.value("op").toString();
        QString opText, detail;
        if (op == "copy") {
            opText = tr("Copy");
            detail = a.value("reason").toString() == "changed" ? tr("changed since the last sync") : QString();
        } else if (op == "convert") {
            opText = tr("Convert");
            detail = tr("to %1").arg(codecText(a.value("rule").toString()));
        } else if (op == "delete") {
            opText = tr("Delete");
            detail = a.value("reason").toString() == "no longer in the list" ? tr("no longer in the list")
                                                                              : tr("not put there by this sync");
        } else if (op == "skip") {
            opText = tr("Leave off");
            const QString r = a.value("reason").toString();
            detail = r == "rule" ? tr("a format rule leaves it off")
                     : r == "missing" ? tr("the file is missing")
                     : r == "on a server" ? tr("it is on a server, not a disk")
                                          : tr("its disk isn't connected");
        }
        const QString target = a.value("target").toString();
        rows << QVariantMap{{"op", op}, {"opText", opText}, {"target", target.isEmpty() ? a.value("source").toString() : target},
                            {"detail", detail}};
    }
    for (const QJsonValue &v : plan.value("playlists").toArray())
        rows << QVariantMap{{"op", "playlist"}, {"opText", tr("Write")}, {"target", v.toString()}, {"detail", tr("playlist file")}};
    const int listed = plan.value("copy").toInt() + plan.value("convert").toInt() + plan.value("delete").toInt() +
                       plan.value("skip").toInt();
    QStringList warnings;
    for (const QJsonValue &w : plan.value("warnings").toArray())
        warnings << w.toString();
    const QJsonObject target = plan.value("target_state").toObject();
    return {{"profileId", id},
            {"ok", plan.value("ok").toBool()},
            {"error", plan.value("error").toString()},
            {"copy", plan.value("copy").toInt()},
            {"convert", plan.value("convert").toInt()},
            {"keep", plan.value("keep").toInt()},
            {"delete", plan.value("delete").toInt()},
            {"skip", plan.value("skip").toInt()},
            {"writeText", sizeText(plan.value("bytes_to_write").toInteger())},
            {"freeText", sizeText(target.value("free_bytes").toInteger())},
            {"fits", plan.value("fits").toBool(true)},
            {"warnings", warnings},
            {"rows", rows},
            {"more", std::max(0, listed - int(plan.value("actions").toArray().size()))}};
}

bool DevicesController::previewProfile(qint64 id) {
    const QString name = nameOf(id);
    m_previewing = true;
    emit previewingChanged();
    const bool queued = m_controller->runJob(
        "sync", tr("Checking what a sync of %1 would do").arg(name),
        [id](Library &library, Job *job) {
            QString why;
            const DeviceSync::Profile p = DeviceSync::find(library, QString::number(id), &why);
            if (!p.id)
                return QJsonObject{{"ok", false}, {"error", why}};
            return DeviceSync::plan(library, p, job).toJson(1000);
        },
        [this, id](const QJsonObject &plan) {
            m_previewing = false;
            m_preview = previewFrom(id, plan);
            emit previewingChanged();
            emit previewChanged();
        });
    if (!queued) {
        m_previewing = false;
        emit previewingChanged();
    }
    return queued;
}

bool DevicesController::syncProfile(qint64 id) {
    const QString name = nameOf(id);
    return m_controller->runJob(
        "sync", tr("Syncing %1").arg(name),
        [id](Library &library, Job *job) {
            QString why;
            const DeviceSync::Profile p = DeviceSync::find(library, QString::number(id), &why);
            if (!p.id)
                return QJsonObject{{"ok", false}, {"error", why}};
            const DeviceSync::Plan plan = DeviceSync::plan(library, p, job);
            QJsonObject r;
            if (!plan.ok())
                r = QJsonObject{{"ok", false}, {"error", plan.error}};
            else if (!plan.fits)
                r = QJsonObject{{"ok", false}, {"error", plan.warnings.value(0)}};
            else
                r = DeviceSync::apply(library, plan, job).toJson();
            if (!(job && job->isCancelled() && !plan.ok()))
                DeviceSync::recordRun(library, id, r);
            return r;
        },
        [this, id, name](const QJsonObject &r) {
            reload();
            if (m_preview.value("profileId").toLongLong() == id)
                clearPreview();
            if (r.value("ok").toBool())
                emit message(tr("Synced %1: %2 copied, %3 converted, %4 already there, %5 deleted")
                                 .arg(name)
                                 .arg(r.value("copied").toInt())
                                 .arg(r.value("converted").toInt())
                                 .arg(r.value("kept").toInt())
                                 .arg(r.value("deleted").toInt()));
            else
                emit message(tr("Sync of %1 didn't finish: %2").arg(name, r.value("error").toString()));
            emit synced(id, r);
        });
}

void DevicesController::clearPreview() {
    m_preview.clear();
    emit previewChanged();
}

void DevicesController::chooseTarget() { m_picker.pickFolders(); }

QList<qint64> DevicesController::trackIds(qint64 id) const {
    Library *lib = m_controller->library();
    if (!lib)
        return {};
    const DeviceSync::Profile p = DeviceSync::find(*lib, QString::number(id));
    return p.id ? DeviceSync::trackIds(*lib, p) : QList<qint64>{};
}
