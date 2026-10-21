// `thrum sync` and `thrum report`: device sync profiles and track-list
// reports from the command line. Kept apart from cli.cpp, as the NAS
// commands are.
#include "cli.h"

#include "devicesync.h"
#include "job.h"
#include "library.h"
#include "playlists.h"
#include "query.h"
#include "report.h"
#include "volumes.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <unistd.h>

namespace Cli {

namespace {

struct SyncOptions {
    bool json = false, dryRun = false, deleteOthers = false, keepOthers = false, noPlaylists = false, playlists = false,
         all = false;
    QString db, to, kind, mask, name, format, out, columns, title, playlist, collection;
    QStringList sources, rules;
    bool clearRules = false;
    int workers = 0, limit = 0;
    QStringList rest;
};

QString toJson(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

int fail(const SyncOptions &o, QTextStream &out, QTextStream &err, const QString &message) {
    err << "thrum: " << message << Qt::endl;
    if (o.json)
        out << toJson({{"ok", false}, {"error", message}}) << Qt::flush;
    return 1;
}

QString mb(qint64 bytes) {
    if (bytes >= 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
    return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
}

void printProfile(QTextStream &out, const DeviceSync::Profile &p) {
    out << QString("%1  %2").arg(p.id, 3).arg(p.name) << "\n"
        << "     to " << p.target << " (" << p.kind << ")\n";
    for (const DeviceSync::Source &s : p.sources)
        out << "     syncs " << s.toString() << "\n";
    for (const DeviceSync::FormatRule &r : p.rules)
        out << "     rule  " << r.toString() << "\n";
    out << "     names " << (p.mask.isEmpty() ? QString(DeviceSync::defaultMask) : p.mask) << "\n";
    if (p.deleteOthers)
        out << "     deletes other files on the device\n";
    out << "     last sync "
        << (p.lastSync ? QDateTime::fromSecsSinceEpoch(p.lastSync).toString("yyyy-MM-dd HH:mm") : QString("never"));
    if (!p.lastResult.isEmpty() && !p.lastResult.value("error").toString().isEmpty())
        out << " (" << p.lastResult.value("error").toString() << ")";
    out << "\n";
}

// Applies the options to a profile; returns an error or nothing.
QString applyOptions(const SyncOptions &o, DeviceSync::Profile *p) {
    if (!o.to.isEmpty())
        p->target = o.to;
    if (!o.kind.isEmpty())
        p->kind = o.kind;
    if (!o.mask.isNull())
        p->mask = o.mask;
    if (!o.name.isEmpty())
        p->name = o.name;
    if (!o.sources.isEmpty()) {
        p->sources.clear();
        for (const QString &s : o.sources) {
            QString why;
            const DeviceSync::Source src = DeviceSync::Source::fromString(s, &why);
            if (!why.isEmpty())
                return why;
            p->sources << src;
        }
    }
    if (o.clearRules)
        p->rules.clear();
    for (const QString &r : o.rules) {
        QString why;
        const DeviceSync::FormatRule rule = DeviceSync::FormatRule::fromString(r, &why);
        if (!why.isEmpty())
            return why;
        p->rules << rule;
    }
    if (o.deleteOthers)
        p->deleteOthers = true;
    if (o.keepOthers)
        p->deleteOthers = false;
    if (o.noPlaylists)
        p->writePlaylists = false;
    if (o.playlists)
        p->writePlaylists = true;
    return {};
}

void printPlan(QTextStream &out, const DeviceSync::Plan &plan, int limit) {
    int shown = 0;
    for (const DeviceSync::Action &a : plan.actions) {
        if (a.op == "keep")
            continue;
        if (limit > 0 && shown++ >= limit) {
            out << "  …\n";
            break;
        }
        const QString what = a.op == "convert" ? "convert" : a.op;
        out << QString("  %1 %2").arg(what, -8).arg(a.target.isEmpty() ? a.source : a.target);
        if (a.op == "convert")
            out << "  (" << a.rule << ")";
        if (a.op == "delete" || a.op == "skip" || a.reason == "changed")
            out << "  (" << a.reason << ")";
        out << "\n";
    }
    for (const QString &p : plan.playlists)
        out << QString("  %1 %2").arg("playlist", -8).arg(p) << "\n";
    out << QString("%1 to copy, %2 to convert, %3 already there, %4 to delete, %5 left off; "
                   "%6 to write, %7 freed, %8 free on the device")
               .arg(plan.copies)
               .arg(plan.conversions)
               .arg(plan.kept)
               .arg(plan.deletions)
               .arg(plan.skipped)
               .arg(mb(plan.bytesToWrite), mb(plan.bytesToFree),
                    plan.target.freeBytes >= 0 ? mb(plan.target.freeBytes) : QString("unknown"))
        << "\n";
    for (const QString &w : plan.warnings)
        out << "Note: " << w << "\n";
    out << Qt::flush;
}

int syncCommand(Library &library, const SyncOptions &o, const VolumeResolver &resolver, QTextStream &out,
                QTextStream &err) {
    const QString sub = o.rest.value(0, "list");
    QString why;

    if (sub == "list") {
        const QList<DeviceSync::Profile> all = DeviceSync::list(library);
        if (o.json) {
            QJsonArray a;
            for (const DeviceSync::Profile &p : all) {
                QJsonObject j = p.toJson();
                j["target_state"] = DeviceSync::checkTarget(library, p).toJson();
                a << j;
            }
            out << toJson({{"ok", true}, {"profiles", a}}) << Qt::flush;
            return 0;
        }
        if (all.isEmpty()) {
            out << "No sync profiles. Make one with:\n"
                   "  thrum sync create Phone --to /run/media/$USER/PHONE/Music --kind usb \\\n"
                   "      --source 'playlist:Road trip' --rule 'lossless->opus:128'"
                << Qt::endl;
            return 0;
        }
        for (const DeviceSync::Profile &p : all)
            printProfile(out, p);
        out << Qt::flush;
        return 0;
    }

    if (sub == "devices") {
        const QList<DeviceSync::Device> devices = DeviceSync::detectDevices();
        if (o.json) {
            QJsonArray a;
            for (const DeviceSync::Device &d : devices)
                a << d.toJson();
            out << toJson({{"ok", true}, {"devices", a}}) << Qt::flush;
            return 0;
        }
        if (devices.isEmpty())
            out << "No USB drives plugged in and no Syncthing folders found." << Qt::endl;
        for (const DeviceSync::Device &d : devices)
            out << QString("%1  %2  %3").arg(d.kind, -10).arg(d.label, -20).arg(d.path)
                << (d.freeBytes >= 0 ? "  (" + mb(d.freeBytes) + " free)" : QString()) << "\n";
        out << Qt::flush;
        return 0;
    }

    if (sub == "create") {
        DeviceSync::Profile p;
        p.name = o.rest.value(1);
        if (const QString e = applyOptions(o, &p); !e.isEmpty())
            return fail(o, out, err, e);
        if (!o.name.isEmpty())
            p.name = o.name;
        const qint64 id = DeviceSync::save(library, p, &why);
        if (!id)
            return fail(o, out, err, why);
        p = DeviceSync::find(library, QString::number(id));
        if (o.json) {
            QJsonObject j = p.toJson();
            j["ok"] = true;
            out << toJson(j) << Qt::flush;
        } else {
            out << "Made sync profile " << id << ".\n";
            printProfile(out, p);
            out << "See what it would do with: thrum sync run " << id << " --dry-run" << Qt::endl;
        }
        return 0;
    }

    if (o.rest.size() < 2)
        return fail(o, out, err, QString("sync %1 needs a profile (an id or name from: thrum sync list)").arg(sub));
    DeviceSync::Profile p = DeviceSync::find(library, o.rest[1], &why);
    if (!p.id)
        return fail(o, out, err, why);

    if (sub == "show") {
        if (o.json) {
            QJsonObject j = p.toJson();
            j["ok"] = true;
            j["target_state"] = DeviceSync::checkTarget(library, p).toJson();
            out << toJson(j) << Qt::flush;
        } else {
            printProfile(out, p);
            out << "     " << DeviceSync::checkTarget(library, p).message << Qt::endl;
        }
        return 0;
    }

    if (sub == "set") {
        if (const QString e = applyOptions(o, &p); !e.isEmpty())
            return fail(o, out, err, e);
        if (!DeviceSync::save(library, p, &why))
            return fail(o, out, err, why);
        if (o.json) {
            QJsonObject j = DeviceSync::find(library, QString::number(p.id)).toJson();
            j["ok"] = true;
            out << toJson(j) << Qt::flush;
        } else {
            printProfile(out, DeviceSync::find(library, QString::number(p.id)));
        }
        return 0;
    }

    if (sub == "remove" || sub == "delete") {
        if (!DeviceSync::remove(library, p.id, &why))
            return fail(o, out, err, why);
        if (o.json)
            out << toJson({{"ok", true}, {"id", p.id}}) << Qt::flush;
        else
            out << "Removed the sync profile " << p.name << ". Nothing on the device was touched." << Qt::endl;
        return 0;
    }

    if (sub == "run") {
        library.refreshVolumes(resolver);
        Job job("sync");
        const bool progress = !o.json && isatty(STDERR_FILENO) && err.string() == nullptr;
        if (progress)
            QObject::connect(&job, &Job::progressChanged, [&err](const QString &phase, qint64 done, qint64 total) {
                err << "\r" << phase << (total > 0 ? QString(": %1 of %2").arg(done).arg(total) : QString()) << "      "
                    << Qt::flush;
            });
        const DeviceSync::Plan plan = DeviceSync::plan(library, p, &job);
        if (progress)
            err << "\r" << QString(60, ' ') << "\r" << Qt::flush;
        if (!plan.ok())
            return fail(o, out, err, plan.error);
        if (o.dryRun) {
            if (o.json) {
                QJsonObject j = plan.toJson(o.limit, o.all);
                j["dry_run"] = true;
                out << toJson(j) << Qt::flush;
            } else {
                out << "Dry run of " << p.name << " to " << p.target << " (nothing is written):\n";
                printPlan(out, plan, o.limit);
            }
            return 0;
        }
        if (!plan.fits)
            return fail(o, out, err, plan.warnings.value(0));
        const DeviceSync::Result r = DeviceSync::apply(library, plan, &job, o.workers);
        if (progress)
            err << "\r" << QString(60, ' ') << "\r" << Qt::flush;
        DeviceSync::recordRun(library, p.id, r.toJson());
        if (o.json) {
            QJsonObject j = r.toJson();
            j["profile"] = p.name;
            j["target"] = p.target;
            out << toJson(j) << Qt::flush;
        } else {
            out << QString("%1: %2 copied, %3 converted, %4 already there, %5 deleted, %6 left off; %7 written in %8 s")
                       .arg(p.name)
                       .arg(r.copied)
                       .arg(r.converted)
                       .arg(r.kept)
                       .arg(r.deleted)
                       .arg(r.skipped)
                       .arg(mb(r.bytesWritten))
                       .arg(r.elapsedMs / 1000.0, 0, 'f', 1)
                << Qt::endl;
            for (const QString &f : r.failures)
                err << "thrum: " << f << Qt::endl;
        }
        return r.ok ? 0 : 1;
    }
    return fail(o, out, err, "sync takes list, devices, create, show, set, run or remove");
}

int reportCommand(Library &library, const SyncOptions &o, QTextStream &out, QTextStream &err) {
    QString why;
    QList<qint64> ids;
    QString title = o.title;
    if (!o.playlist.isEmpty()) {
        const qint64 id = Playlists::resolve(library, o.playlist, &why);
        if (!id)
            return fail(o, out, err, why);
        Playlists::Playlist pl;
        Playlists::get(library, id, &pl);
        ids = Playlists::trackIds(library, id, &why);
        if (title.isEmpty())
            title = pl.name;
    } else if (!o.collection.isEmpty()) {
        const qint64 id = Collections::resolve(library, o.collection, &why);
        if (!id)
            return fail(o, out, err, why);
        Collections::Collection c;
        Collections::get(library, id, &c);
        ids = Collections::trackIds(library, id, {}, &why);
        if (title.isEmpty())
            title = c.name;
    } else {
        const QString text = o.rest.join(' ');
        ids = Query::ids(library, text, {}, &why);
        if (title.isEmpty())
            title = text.isEmpty() ? QString("Every track") : text;
    }
    if (!why.isEmpty())
        return fail(o, out, err, why);

    Report::Options options;
    options.format = o.format.isEmpty() ? (o.out.endsWith(".html", Qt::CaseInsensitive) || o.out.endsWith(".htm", Qt::CaseInsensitive)
                                               ? QString("html")
                                               : QString("csv"))
                                        : o.format;
    options.title = title;
    if (!o.columns.isEmpty())
        options.columns = o.columns.split(',', Qt::SkipEmptyParts);

    if (o.out.isEmpty()) {
        if (o.json)
            return fail(o, out, err, "--json needs --out <file> (the report itself would be stdout)");
        const Report::Output r = Report::render(library, ids, options);
        if (!r.ok())
            return fail(o, out, err, r.error);
        out << QString::fromUtf8(r.data) << Qt::flush;
        return 0;
    }
    const Report::Output r = Report::write(library, ids, options, o.out);
    if (!r.ok())
        return fail(o, out, err, r.error);
    if (o.json)
        out << toJson({{"ok", true},
                       {"file", QFileInfo(o.out).absoluteFilePath()},
                       {"format", options.format.toLower()},
                       {"tracks", r.tracks},
                       {"duration_ms", r.durationMs},
                       {"bytes", r.bytes}})
            << Qt::flush;
    else
        out << "Wrote " << r.tracks << " tracks to " << QFileInfo(o.out).absoluteFilePath() << Qt::endl;
    return 0;
}

} // namespace

int runSync(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver) {
    SyncOptions o;
    const QString command = arguments.value(0);
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        auto value = [&](QString *target) {
            if (i + 1 >= arguments.size())
                return false;
            *target = arguments[++i];
            return true;
        };
        if (a == "--json")
            o.json = true;
        else if (a == "--dry-run")
            o.dryRun = true;
        else if (a == "--delete-others")
            o.deleteOthers = true;
        else if (a == "--keep-others")
            o.keepOthers = true;
        else if (a == "--no-playlists")
            o.noPlaylists = true;
        else if (a == "--playlists")
            o.playlists = true;
        else if (a == "--clear-rules")
            o.clearRules = true;
        else if (a == "--all")
            o.all = true;
        else if (a == "--workers" || a == "--limit") {
            QString v;
            bool ok = false;
            const int n = value(&v) ? v.toInt(&ok) : 0;
            if (!ok || n < 0)
                return fail(o, out, err, a + " needs a number");
            (a == "--workers" ? o.workers : o.limit) = n;
        } else if (a == "--db" || a == "--to" || a == "--kind" || a == "--mask" || a == "--name" || a == "--format" ||
                   a == "--out" || a == "--columns" || a == "--title" || a == "--playlist" || a == "--collection" ||
                   a == "--source" || a == "--rule") {
            QString v;
            if (!value(&v))
                return fail(o, out, err, a + " needs a value");
            if (a == "--db")
                o.db = v;
            else if (a == "--to")
                o.to = v;
            else if (a == "--kind")
                o.kind = v.toLower();
            else if (a == "--mask")
                o.mask = v.isNull() ? QString("") : v;
            else if (a == "--name")
                o.name = v;
            else if (a == "--format")
                o.format = v;
            else if (a == "--out")
                o.out = v;
            else if (a == "--columns")
                o.columns = v;
            else if (a == "--title")
                o.title = v;
            else if (a == "--playlist")
                o.playlist = v;
            else if (a == "--collection")
                o.collection = v;
            else if (a == "--source")
                o.sources << v;
            else
                o.rules << v;
        } else if (a == "--") {
            o.rest += arguments.mid(i + 1);
            break;
        } else {
            o.rest << a;
        }
    }

    Library library(o.db);
    QString error;
    if (!library.open(&error))
        return fail(o, out, err, error);
    VolumeResolver system;
    const VolumeResolver &volumes = resolver ? *resolver : system;
    if (command == "sync")
        return syncCommand(library, o, volumes, out, err);
    if (command == "report") {
        library.refreshVolumes(volumes);
        return reportCommand(library, o, out, err);
    }
    return fail(o, out, err, "Unknown command " + command);
}

}
