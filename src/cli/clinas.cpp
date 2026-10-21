// `thrum volume`, `thrum nas` and `thrum remote`: network volumes, mount
// guides and Subsonic sources, from the command line. Kept apart from
// cli.cpp so the NAS work doesn't collide with other commands.
#include "cli.h"

#include "job.h"
#include "library.h"
#include "mounts.h"
#include "remote.h"
#include "subsonic.h"
#include "volumes.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlQuery>

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

namespace Cli {

namespace {

bool g_hasStdin = false;
QString g_stdin;

struct NasOptions {
    bool json = false, force = false, full = false, keepTracks = false, passwordStdin = false, apiKeyStdin = false;
    QString db, user, name, smb, nfs, at, writeTo, format;
    int uid = -1, gid = -1, maxBitrate = -1, idle = 600;
    QStringList rest;
};

QString toJson(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

int fail(const NasOptions &o, QTextStream &out, QTextStream &err, const QString &message) {
    err << "thrum: " << message << Qt::endl;
    if (o.json)
        out << toJson({{"ok", false}, {"error", message}}) << Qt::flush;
    return 1;
}

QString readSecret() {
    if (g_hasStdin)
        return g_stdin.section('\n', 0, 0).trimmed();
    std::string line;
    std::getline(std::cin, line);
    return QString::fromStdString(line).trimmed();
}

QString when(qint64 secs) {
    return secs ? QDateTime::fromSecsSinceEpoch(secs).toString("yyyy-MM-dd HH:mm") : QString("never");
}

// --- thrum nas mount-guide ----------------------------------------------

// systemd-escape --path: the unit name for a mount point.
QString unitName(const QString &path, const QString &suffix) {
    QString p = QDir::cleanPath(path);
    while (p.startsWith('/'))
        p.remove(0, 1);
    while (p.endsWith('/'))
        p.chop(1);
    if (p.isEmpty())
        return "-." + suffix;
    QString out;
    const QByteArray bytes = p.toUtf8();
    for (int i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '/')
            out += '-';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                 c == ':' || (c == '.' && i > 0))
            out += QChar(c);
        else
            out += QString("\\x%1").arg(uchar(c), 2, 16, QChar('0'));
    }
    return out + '.' + suffix;
}

int mountGuide(const NasOptions &o, QTextStream &out, QTextStream &err) {
    if (o.smb.isEmpty() == o.nfs.isEmpty())
        return fail(o, out, err, "mount-guide needs one of --smb //server/share or --nfs server:/export");
    if (o.at.isEmpty() || !o.at.startsWith('/'))
        return fail(o, out, err, "mount-guide needs --at /absolute/mount/point (for example /mnt/music)");
    const bool smb = !o.smb.isEmpty();
    const QString what = smb ? o.smb : o.nfs;
    if (smb && (!what.startsWith("//") || what.mid(2).section('/', 0, 0).isEmpty() || what.mid(2).section('/', 1).isEmpty()))
        return fail(o, out, err, "--smb looks like //server/share");
    if (!smb && (!what.contains(":/") || what.startsWith(':')))
        return fail(o, out, err, "--nfs looks like server:/export/path");
    const QString where = QDir::cleanPath(o.at);
    if (where == "/" || where.startsWith("/mnt/old-root") || where.startsWith("/mnt/old-home"))
        return fail(o, out, err, QString("%1 is not a sensible mount point for a music share").arg(where));
    const int uid = o.uid >= 0 ? o.uid : int(getuid());
    const int gid = o.gid >= 0 ? o.gid : int(getgid());
    const QString server = smb ? what.mid(2).section('/', 0, 0) : what.section(':', 0, 0);
    const QString share = smb ? what.mid(2).section('/', 1).replace('/', '-') : what.section(':', 1).section('/', -1);
    const QString mountUnit = unitName(where, "mount");
    const QString automountUnit = unitName(where, "automount");
    const QString credentials = QString("/etc/samba/credentials/%1-%2").arg(server, share.isEmpty() ? "share" : share);

    QString options;
    if (smb) {
        // vers=3.1.1: SMB 3.1.1 (encryption, pre-auth integrity); every NAS
        // from the last decade speaks it. noserverino: use client-made
        // inode numbers, because some servers (and anything that crosses
        // filesystems inside the share) hand out unstable or colliding
        // ones, which confuses tools that track files by inode. Thrum
        // itself keys tracks by path, so it is a safety setting, not a need.
        // actimeo=30: cache attributes for 30 s, which keeps a rescan's
        // stats cheap; a change made on the NAS shows within 30 s.
        options = QString("_netdev,nofail,credentials=%1,uid=%2,gid=%3,forceuid,forcegid,file_mode=0644,dir_mode=0755,"
                          "vers=3.1.1,noserverino,iocharset=utf8,actimeo=30")
                      .arg(credentials)
                      .arg(uid)
                      .arg(gid);
    } else {
        // NFSv4.1: one TCP port (2049), no rpcbind or lockd, sessions. hard
        // (the default) retries forever rather than failing a write half
        // way, which is what you want for tag writing; Thrum never waits on
        // a hung share itself (it probes with a timeout). noatime saves a
        // write per file read.
        options = "_netdev,nofail,vers=4.1,hard,noatime,timeo=150,retrans=3";
    }

    const QString fsType = smb ? "cifs" : "nfs";
    const QString mount = QString("[Unit]\n"
                                  "Description=Music share %1\n"
                                  "Wants=network-online.target\n"
                                  "After=network-online.target\n"
                                  "\n"
                                  "[Mount]\n"
                                  "What=%1\n"
                                  "Where=%2\n"
                                  "Type=%3\n"
                                  "Options=%4\n"
                                  "TimeoutSec=30\n")
                              .arg(what, where, fsType, options);
    const QString automount = QString("[Unit]\n"
                                      "Description=Mount %1 when it is first used\n"
                                      "\n"
                                      "[Automount]\n"
                                      "Where=%1\n"
                                      "TimeoutIdleSec=%2\n"
                                      "\n"
                                      "[Install]\n"
                                      "WantedBy=remote-fs.target\n")
                                  .arg(where)
                                  .arg(o.idle);
    // The same thing as one /etc/fstab line, for those who prefer it.
    const QString fstab = QString("%1  %2  %3  %4,x-systemd.automount,x-systemd.idle-timeout=%5,"
                                  "x-systemd.mount-timeout=30  0  0")
                              .arg(what, where, fsType, options)
                              .arg(o.idle);

    const QString unitDir = o.writeTo.isEmpty() ? QString("~/thrum-mount-units") : o.writeTo;
    QStringList commands;
    commands << QString("sudo pacman -S --needed %1").arg(smb ? "cifs-utils" : "nfs-utils");
    if (smb) {
        commands << "sudo install -d -m 0700 /etc/samba/credentials"
                 << QString("sudo install -m 0600 /dev/null %1").arg(credentials)
                 << QString("sudoedit %1    # two lines: username=<user on the NAS> and password=<its password>")
                        .arg(credentials);
    }
    commands << QString("sudo install -d -m 0755 %1").arg(where)
             << QString("sudo install -m 0644 %1/%2 %1/%3 /etc/systemd/system/").arg(unitDir, mountUnit, automountUnit)
             << "sudo systemctl daemon-reload"
             << QString("sudo systemctl enable --now %1").arg(automountUnit)
             << QString("ls %1    # the first look mounts it; then: thrum scan %1").arg(where);

    if (!o.writeTo.isEmpty()) {
        // Only into a folder Matt names, never /etc: the sudo lines copy them.
        QDir().mkpath(o.writeTo);
        for (const auto &[name, text] : {std::pair{mountUnit, mount}, std::pair{automountUnit, automount}}) {
            QSaveFile f(o.writeTo + '/' + name);
            if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
                return fail(o, out, err, QString("Can't write %1/%2").arg(o.writeTo, name));
        }
    }

    if (o.json) {
        out << toJson({{"ok", true},
                       {"protocol", smb ? "smb" : "nfs"},
                       {"what", what},
                       {"where", where},
                       {"units", QJsonObject{{mountUnit, mount}, {automountUnit, automount}}},
                       {"fstab", fstab},
                       {"commands", QJsonArray::fromStringList(commands)},
                       {"written_to", o.writeTo}})
            << Qt::flush;
        return 0;
    }

    out << "# Mounting " << what << " at " << where << " with systemd automount\n"
        << "#\n"
        << "# Thrum never runs sudo or mounts anything. "
        << (o.writeTo.isEmpty() ? QString("Save the two units below in %1 (or rerun with\n"
                                          "# --write-to %1 to have them written there), then run the commands at the end.\n")
                                      .arg(unitDir)
                                : QString("The two units below are written to %1;\n"
                                          "# run the commands at the end to install them.\n")
                                      .arg(unitDir))
        << "# The share mounts the first time anything looks at " << where << ", unmounts after\n"
        << "# " << o.idle << " s idle, and a NAS that is off never holds up boot (nofail, automount).\n\n"
        << "# ---- " << mountUnit << " ----\n"
        << mount << "\n"
        << "# ---- " << automountUnit << " ----\n"
        << automount << "\n"
        << "# Or, instead of the two units, this one line in /etc/fstab:\n"
        << "# " << fstab << "\n\n";
    if (smb)
        out << "# Notes: vers=3.1.1 needs SMB 3 on the NAS (on by default for years); use vers=3.0 for an old one.\n"
            << "# noserverino makes the client invent inode numbers, because some servers hand out unstable ones.\n"
            << "# uid=" << uid << ",gid=" << gid << " make the files yours on this machine, whatever the NAS says.\n\n";
    else
        out << "# Notes: vers=4.1 needs NFSv4 on the NAS (Synology, QNAP and TrueNAS offer it; use 4.2 if listed).\n"
            << "# The export must allow this machine; file ownership comes from the NAS (match your uid there,\n"
            << "# " << uid << ", or map it with all_squash/anonuid on the export).\n\n";
    out << "# Commands, in order:\n";
    for (const QString &c : commands)
        out << c << "\n";
    out << Qt::flush;
    return 0;
}

// --- thrum volume ------------------------------------------------------

int volumeCommand(Library &library, const NasOptions &o, const VolumeResolver &resolver, QTextStream &out,
                  QTextStream &err) {
    const QString sub = o.rest.value(0, "list");
    const QList<VolumeCheck> checks = library.refreshVolumes(resolver);
    QHash<qint64, QString> state;
    for (const VolumeCheck &c : checks)
        state.insert(c.id, c.state);

    if (sub == "list") {
        const QList<VolumeRow> rows = Volumes::list(library);
        if (o.json) {
            QJsonArray all;
            for (const VolumeRow &r : rows) {
                QJsonObject j = r.toJson();
                j["state"] = state.value(r.id, r.online ? "online" : "offline");
                all.append(j);
            }
            out << toJson({{"ok", true}, {"volumes", all}}) << Qt::flush;
            return 0;
        }
        if (rows.isEmpty()) {
            out << "No volumes yet. Add a folder with: thrum scan <folder>" << Qt::endl;
            return 0;
        }
        for (const VolumeRow &r : rows) {
            const QString st = state.value(r.id, r.online ? "online" : "offline");
            out << QString("%1  %2  %3  %4 tracks").arg(r.id, 3).arg(r.kind, -8).arg(st, -10).arg(r.tracks);
            if (r.offline)
                out << QString(" (%1 offline)").arg(r.offline);
            if (r.missing)
                out << QString(" (%1 missing)").arg(r.missing);
            out << "\n     " << r.rootPath << "   " << r.key;
            if (!r.fsType.isEmpty() && r.kind != "subsonic")
                out << "   " << r.fsType;
            out << "\n";
        }
        out << Qt::flush;
        return 0;
    }

    if (sub == "show" || sub == "reassign" || sub == "forget" || sub == "workers") {
        if (o.rest.size() < 2)
            return fail(o, out, err, QString("volume %1 needs a volume (an id from: thrum volume list)").arg(sub));
        QString why;
        const VolumeRow row = Volumes::find(library, o.rest[1], &why);
        if (!row.id)
            return fail(o, out, err, why);

        if (sub == "show") {
            QJsonObject j = row.toJson();
            j["state"] = state.value(row.id, row.online ? "online" : "offline");
            if (o.json) {
                out << toJson({{"ok", true}, {"volume", j}}) << Qt::flush;
                return 0;
            }
            out << "Volume " << row.id << " (" << row.kind << ", " << j["state"].toString() << ")\n"
                << "  identity:      " << row.key << "\n"
                << "  root:          " << row.rootPath << "\n"
                << "  mounted from:  " << row.source << (row.fsType.isEmpty() ? QString() : " (" + row.fsType + ")")
                << "\n"
                << "  last seen:     " << when(row.lastSeen) << "\n"
                << "  tracks:        " << row.tracks << " (" << row.offline << " offline, " << row.missing
                << " missing)\n"
                << "  folders:       " << (row.libraryRoots.isEmpty() ? QString("-") : row.libraryRoots.join(", "))
                << "\n"
                << "  scan workers:  " << row.effectiveWorkers() << (row.scanWorkers ? "" : " (default)") << "\n"
                << Qt::flush;
            return 0;
        }

        if (sub == "reassign") {
            if (o.rest.size() < 3)
                return fail(o, out, err, "volume reassign needs the new root: thrum volume reassign <volume> <folder>");
            QJsonObject report;
            if (!Volumes::reassign(library, resolver, row.id, o.rest[2], &why, &report))
                return fail(o, out, err, why);
            if (o.json) {
                report["ok"] = true;
                out << toJson(report) << Qt::flush;
            } else {
                out << "Volume " << row.id << " now lives at " << report["new_root"].toString() << " ("
                    << report["new_key"].toString() << ").";
                if (report["marker_written"].toBool())
                    out << " Wrote " << VolumeResolver::markerName << " there so it is recognised next time.";
                out << Qt::endl;
            }
            return 0;
        }

        if (sub == "workers") {
            bool ok = false;
            const int n = o.rest.value(2).toInt(&ok);
            if (!ok)
                return fail(o, out, err, "volume workers needs a number (0 for the default)");
            if (!Volumes::setScanWorkers(library, row.id, n, &why))
                return fail(o, out, err, why);
            const VolumeRow now = Volumes::find(library, QString::number(row.id), &why);
            if (o.json)
                out << toJson({{"ok", true}, {"id", row.id}, {"scan_workers", now.effectiveWorkers()}}) << Qt::flush;
            else
                out << "Volume " << row.id << " is scanned with " << now.effectiveWorkers() << " workers." << Qt::endl;
            return 0;
        }

        // forget
        qint64 removed = 0;
        if (!Volumes::forget(library, row.id, o.force, &why, &removed))
            return fail(o, out, err, why);
        if (o.json)
            out << toJson({{"ok", true}, {"id", row.id}, {"tracks_removed", removed}}) << Qt::flush;
        else
            out << "Forgot volume " << row.id << " and its " << removed << " tracks." << Qt::endl;
        return 0;
    }
    return fail(o, out, err, "volume takes list, show, reassign, forget or workers");
}

// --- thrum remote ------------------------------------------------------

int remoteCommand(Library &library, const NasOptions &o, QTextStream &out, QTextStream &err) {
    const QString sub = o.rest.value(0, "list");
    QString why;

    if (sub == "add") {
        if (o.rest.value(1) != "subsonic" || o.rest.size() < 3)
            return fail(o, out, err, "Usage: thrum remote add subsonic <url> --user <name> --password-stdin | --api-key-stdin");
        if (o.passwordStdin == o.apiKeyStdin)
            return fail(o, out, err, "Give one of --password-stdin or --api-key-stdin (the secret is read from stdin, "
                                     "never from the command line)");
        const QString secret = readSecret();
        RemoteSource added;
        const qint64 id = RemoteSources::add(library, QUrl(o.rest[2]), o.user, secret,
                                             o.apiKeyStdin ? "apikey" : "token", o.name, &why, &added);
        if (!id)
            return fail(o, out, err, why);
        if (o.json) {
            QJsonObject j = added.toJson();
            j["ok"] = true;
            out << toJson(j) << Qt::flush;
        } else {
            out << "Added " << added.name << " (" << (added.serverType.isEmpty() ? QString("Subsonic") : added.serverType)
                << " " << added.serverVersion << ", API " << added.apiVersion << ") as source " << id << ".\n"
                << "The " << (o.apiKeyStdin ? "API key" : "password") << " is kept in "
                << (added.secretStore == "libsecret" ? QString("the desktop keyring (secret-tool)")
                                                     : Secrets::filePath() + " (mode 0600)")
                << ".\nNow run: thrum remote sync " << id << Qt::endl;
        }
        return 0;
    }

    if (sub == "list") {
        const QList<RemoteSource> sources = RemoteSources::list(library);
        if (o.json) {
            QJsonArray all;
            for (const RemoteSource &s : sources)
                all.append(s.toJson());
            out << toJson({{"ok", true}, {"sources", all}}) << Qt::flush;
            return 0;
        }
        if (sources.isEmpty()) {
            out << "No remote sources. Add one with: thrum remote add subsonic <url> --user <name> --password-stdin"
                << Qt::endl;
            return 0;
        }
        for (const RemoteSource &s : sources) {
            out << QString("%1  %2  %3  %4").arg(s.id, 3).arg(s.name, -20).arg(s.url).arg(s.username) << "\n"
                << "     " << (s.serverType.isEmpty() ? QString("subsonic") : s.serverType) << " " << s.serverVersion
                << ", " << s.auth << " auth, last sync " << when(s.lastSync);
            if (!s.lastError.isEmpty())
                out << ", last error: " << s.lastError;
            out << "\n";
        }
        out << Qt::flush;
        return 0;
    }

    if (sub == "sync" || sub == "remove" || sub == "set") {
        QList<RemoteSource> targets;
        if (sub == "sync" && (o.rest.size() < 2 || o.rest[1] == "all")) {
            targets = RemoteSources::list(library);
            if (targets.isEmpty())
                return fail(o, out, err, "No remote sources to sync");
        } else {
            if (o.rest.size() < 2)
                return fail(o, out, err, QString("remote %1 needs a source (an id or name from: thrum remote list)").arg(sub));
            const RemoteSource s = RemoteSources::find(library, o.rest[1], &why);
            if (!s.id)
                return fail(o, out, err, why);
            targets << s;
        }

        if (sub == "remove") {
            if (!RemoteSources::remove(library, targets[0].id, o.keepTracks, &why))
                return fail(o, out, err, why);
            if (o.json)
                out << toJson({{"ok", true}, {"id", targets[0].id}, {"kept_tracks", o.keepTracks}}) << Qt::flush;
            else
                out << "Removed " << targets[0].name << (o.keepTracks ? " (its tracks stay, offline)." : " and its tracks.")
                    << Qt::endl;
            return 0;
        }

        if (sub == "set") {
            const int bitrate = o.maxBitrate >= 0 ? o.maxBitrate : targets[0].maxBitrate;
            const QString format = o.format.isNull() ? targets[0].streamFormat : o.format;
            if (!RemoteSources::setStreaming(library, targets[0].id, bitrate, format, &why))
                return fail(o, out, err, why);
            if (o.json)
                out << toJson({{"ok", true}, {"id", targets[0].id}, {"max_bitrate", bitrate}, {"stream_format", format}})
                    << Qt::flush;
            else
                out << targets[0].name << " streams at " << (bitrate ? QString("%1 kbit/s").arg(bitrate) : QString("the original bitrate"))
                    << (format.isEmpty() ? QString() : " as " + format) << "." << Qt::endl;
            return 0;
        }

        bool allOk = true;
        QJsonArray results;
        for (const RemoteSource &s : std::as_const(targets)) {
            Job job("sync");
            const RemoteSyncResult r = RemoteSources::sync(library, s.id, o.full, &job);
            allOk = allOk && r.ok;
            QJsonObject j = r.toJson();
            j["id"] = s.id;
            j["name"] = s.name;
            results.append(j);
            if (!o.json) {
                if (!r.ok) {
                    err << "thrum: " << s.name << ": " << r.error << Qt::endl;
                    continue;
                }
                out << s.name << ": " << r.albums << " albums (" << r.albumsFetched << " fetched, " << r.albumsUnchanged
                    << " unchanged, " << r.albumsGone << " gone); tracks " << r.tracksAdded << " added, "
                    << r.tracksUpdated << " updated, " << r.tracksMissing << " missing; " << r.requests
                    << " requests in " << r.elapsedMs << " ms" << Qt::endl;
                if (!r.error.isEmpty())
                    err << "thrum: " << s.name << ": " << r.error << Qt::endl;
            }
        }
        if (o.json)
            out << toJson({{"ok", allOk}, {"results", results}}) << Qt::flush;
        return allOk ? 0 : 1;
    }
    return fail(o, out, err, "remote takes add, list, sync, set or remove");
}

} // namespace

void setStdinForTesting(const QString &text) {
    g_hasStdin = true;
    g_stdin = text;
}

int runNas(const QStringList &arguments, QTextStream &out, QTextStream &err, const VolumeResolver *resolver) {
    NasOptions o;
    const QString command = arguments.value(0);
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        auto value = [&](QString *target) {
            if (i + 1 >= arguments.size())
                return false;
            *target = arguments[++i];
            return true;
        };
        auto number = [&](int *target) {
            QString v;
            bool ok = false;
            if (value(&v))
                *target = v.toInt(&ok);
            return ok && *target >= 0;
        };
        if (a == "--json")
            o.json = true;
        else if (a == "--force")
            o.force = true;
        else if (a == "--full")
            o.full = true;
        else if (a == "--keep-tracks")
            o.keepTracks = true;
        else if (a == "--password-stdin")
            o.passwordStdin = true;
        else if (a == "--api-key-stdin")
            o.apiKeyStdin = true;
        else if (a == "--db" || a == "--user" || a == "--name" || a == "--smb" || a == "--nfs" || a == "--at" ||
                 a == "--write-to" || a == "--format") {
            QString v;
            if (!value(&v))
                return fail(o, out, err, a + " needs a value");
            if (a == "--db")
                o.db = v;
            else if (a == "--user")
                o.user = v;
            else if (a == "--name")
                o.name = v;
            else if (a == "--smb")
                o.smb = v;
            else if (a == "--nfs")
                o.nfs = v;
            else if (a == "--at")
                o.at = v;
            else if (a == "--write-to")
                o.writeTo = v;
            else
                o.format = v;
        } else if (a == "--uid" || a == "--gid" || a == "--max-bitrate" || a == "--idle") {
            int n = 0;
            if (!number(&n))
                return fail(o, out, err, a + " needs a number");
            if (a == "--uid")
                o.uid = n;
            else if (a == "--gid")
                o.gid = n;
            else if (a == "--max-bitrate")
                o.maxBitrate = n;
            else
                o.idle = n;
        } else if (a == "--password" || a == "--api-key") {
            return fail(o, out, err, "Secrets are never taken from the command line (they would land in shell history); "
                                     "use --password-stdin or --api-key-stdin");
        } else if (a == "--") {
            o.rest += arguments.mid(i + 1);
            break;
        } else {
            o.rest << a;
        }
    }

    if (command == "nas") {
        if (o.rest.value(0) != "mount-guide")
            return fail(o, out, err, "Usage: thrum nas mount-guide --smb //nas/music --at /mnt/music "
                                     "(or --nfs nas:/export/music)");
        return mountGuide(o, out, err);
    }

    Library library(o.db);
    QString error;
    if (!library.open(&error))
        return fail(o, out, err, error);
    VolumeResolver system;
    const VolumeResolver &volumes = resolver ? *resolver : system;
    if (command == "volume")
        return volumeCommand(library, o, volumes, out, err);
    if (command == "remote")
        return remoteCommand(library, o, out, err);
    return fail(o, out, err, "Unknown command " + command);
}

}
