#include "nascontroller.h"

#include "cli.h"
#include "job.h"
#include "library.h"
#include "librarycontroller.h"
#include "mounts.h"
#include "portal.h"
#include "remote.h"
#include "scanner.h"
#include "subsonic.h"
#include "watcher.h"

#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTextStream>
#include <QUrl>

NasController::NasController(LibraryController *library) : QObject(library), m_controller(library) {}

NasController::~NasController() {
    if (m_testThread) {
        m_testThread->wait();
        delete m_testThread;
    }
}

void NasController::setPicker(FolderPicker *picker) {
    m_picker = picker;
    picker->setup(tr("Where is this volume's music now?"), tr("Use this folder"), true, false);
    connect(picker, &FolderPicker::picked, this, [this](const QStringList &paths) {
        const qint64 id = m_reassigning;
        m_reassigning = 0;
        if (id && !paths.isEmpty())
            reassignTo(id, paths.first());
    });
    connect(picker, &FolderPicker::failed, this, [this](const QString &text) {
        m_reassigning = 0;
        emit message(text);
    });
}

bool NasController::checking() const { return m_monitor && m_monitor->isBusy(); }

void NasController::startMonitor() {
    if (m_monitor)
        return;
    m_monitor = std::make_unique<MountMonitor>(m_controller->databasePath(), m_resolver);
    m_monitor->setPollInterval(m_pollMs);
    connect(m_monitor.get(), &MountMonitor::busyChanged, this, &NasController::checkingChanged);
    connect(m_monitor.get(), &MountMonitor::volumesChecked, this, &NasController::onChecked);
    connect(m_monitor.get(), &MountMonitor::volumesWentOffline, this, [this](const QList<qint64> &ids) {
        // The rows already say offline; the lists on screen read them again.
        m_controller->refreshAll();
        QStringList names;
        for (const QVariant &v : std::as_const(m_volumes)) {
            const QVariantMap m = v.toMap();
            if (ids.contains(m.value("id").toLongLong()))
                names << m.value("name").toString();
        }
        if (!names.isEmpty())
            emit message(tr("%1 isn't connected; its tracks show as offline").arg(names.join(", ")));
    });
    connect(m_monitor.get(), &MountMonitor::volumesCameOnline, this, [this](const QList<qint64> &, const QStringList &roots) {
        m_controller->refreshAll();
        if (roots.isEmpty())
            return;
        m_cameBack = roots;
        // The folder watcher follows the monitor and quick-rescans the
        // roots that came back; without it, a quick rescan job does.
        if (Watcher *w = m_controller->watcher(); w && w->isRunning()) {
            setShareNote(tr("%1 is back; looking for changes on it…").arg(roots.join(", ")));
        } else {
            setShareNote(tr("%1 is back; rescanning it…").arg(roots.join(", ")));
            m_controller->runJob(
                "scan", tr("Rescanning %1").arg(roots.join(", ")),
                [roots, resolver = m_resolver](Library &library, Job *job) {
                    Scanner scanner(library, resolver, job);
                    return scanner.scan(roots, Scanner::Mode::ChangedFolders).toJson();
                },
                [this](const QJsonObject &) {
                    setShareNote({});
                    m_controller->refreshAll();
                });
        }
    });
    m_monitor->start();
}

void NasController::stopMonitor() {
    if (m_monitor)
        m_monitor->stop();
}

void NasController::remountRescanned(const QJsonObject &result) {
    if (m_shareNote.isEmpty())
        return;
    setShareNote({});
    reload();
    const int added = result.value("added").toInt(), changed = result.value("updated").toInt();
    emit message(added + changed > 0 ? tr("%1 is back: %2 new, %3 changed").arg(m_cameBack.join(", ")).arg(added).arg(changed)
                                     : tr("%1 is back; nothing changed on it").arg(m_cameBack.join(", ")));
}

void NasController::setShareNote(const QString &text) {
    if (text == m_shareNote)
        return;
    m_shareNote = text;
    emit shareNoteChanged();
}

void NasController::checkNow() {
    if (!m_controller->activate())
        return;
    if (m_monitor) {
        m_monitor->checkNow();
        return;
    }
    // No monitor (tests, or watching off): the same check on a throwaway one.
    startMonitor();
}

void NasController::onChecked(const QJsonArray &volumes) {
    for (const QJsonValue &v : volumes) {
        const QJsonObject o = v.toObject();
        m_states.insert(o.value("id").toInteger(), o.value("state").toString());
    }
    reload();
    emit volumesChecked();
}

QString NasController::stateText(const QString &state) const {
    if (state == "online")
        return tr("Connected");
    if (state == "absent")
        return tr("Not connected");
    if (state == "stale")
        return tr("Stale: the server no longer knows this share; remount it");
    if (state == "io-error")
        return tr("The server stopped answering");
    if (state == "denied")
        return tr("Mounted, but you may not read it");
    if (state == "timed-out")
        return tr("Not answering (gave up after 1.5 s)");
    if (state == "still-hung")
        return tr("Still not answering");
    return {};
}

void NasController::reload() {
    Library *lib = m_controller->library();
    m_volumes.clear();
    m_servers.clear();
    if (!lib) {
        emit volumesChanged();
        emit serversChanged();
        return;
    }
    const QLocale locale;
    QHash<qint64, QString> serverNames;
    for (const RemoteSource &s : RemoteSources::list(*lib)) {
        serverNames.insert(s.volumeId, s.name);
        qint64 tracks = 0;
        QSqlQuery q(lib->db());
        q.prepare("SELECT count(*) FROM tracks WHERE volume_id = ?");
        q.addBindValue(s.volumeId);
        if (q.exec() && q.next())
            tracks = q.value(0).toLongLong();
        QString server = s.serverType.isEmpty() ? QString() : s.serverType.left(1).toUpper() + s.serverType.mid(1);
        if (!s.serverVersion.isEmpty())
            server += (server.isEmpty() ? "" : " ") + s.serverVersion;
        m_servers << QVariantMap{{"id", s.id},
                                 {"name", s.name},
                                 {"url", s.url},
                                 {"user", s.username},
                                 {"auth", s.auth},
                                 {"server", server},
                                 {"lastSync", s.lastSync ? m_controller->formatDateTime(s.lastSync) : QString()},
                                 {"lastError", s.lastError},
                                 {"tracks", tracks},
                                 {"maxBitrate", s.maxBitrate},
                                 {"format", s.streamFormat},
                                 {"volumeId", s.volumeId}};
    }
    for (const VolumeRow &r : Volumes::list(*lib)) {
        QString kindText;
        if (r.kind == "subsonic")
            kindText = tr("Navidrome / Subsonic");
        else if (r.kind == "network")
            kindText = r.fsType.startsWith("nfs") ? tr("NFS share")
                       : r.fsType == "cifs" || r.fsType.startsWith("smb") ? tr("SMB share")
                                                                           : tr("Network share (%1)").arg(r.fsType);
        else
            kindText = tr("Local disk");
        const QString state = m_states.value(r.id, r.online ? "online" : "absent");
        QString name = r.label;
        if (r.kind == "subsonic")
            name = serverNames.value(r.id, r.label);
        if (name.isEmpty())
            name = r.kind == "network" && !r.source.isEmpty() ? r.source : r.rootPath;
        QStringList roots;
        for (const QString &rel : r.libraryRoots)
            roots << (rel.isEmpty() ? r.rootPath : (r.rootPath == "/" ? "/" + rel : r.rootPath + '/' + rel));
        m_volumes << QVariantMap{{"id", r.id},
                                 {"kind", r.kind},
                                 {"kindText", kindText},
                                 {"name", name},
                                 {"root", r.kind == "subsonic" ? QString() : r.rootPath},
                                 {"source", r.source},
                                 {"fsType", r.fsType},
                                 {"online", r.online},
                                 {"state", r.online ? QString("online") : state},
                                 {"stateText", r.kind == "subsonic" ? (r.online ? tr("Connected") : tr("Not answering"))
                                                                    : stateText(r.online ? "online" : state)},
                                 {"tracks", r.tracks},
                                 {"offline", r.offline},
                                 {"missing", r.missing},
                                 {"workers", r.scanWorkers},
                                 {"effectiveWorkers", r.effectiveWorkers()},
                                 {"roots", roots},
                                 {"lastSeen", r.lastSeen ? m_controller->formatDateTime(r.lastSeen) : QString()},
                                 {"remoteSourceId", r.remoteSourceId}};
    }
    emit volumesChanged();
    emit serversChanged();
}

bool NasController::rescanVolume(qint64 id, bool quick) {
    for (const QVariant &v : std::as_const(m_volumes)) {
        const QVariantMap m = v.toMap();
        if (m.value("id").toLongLong() != id)
            continue;
        if (m.value("kind") == "subsonic")
            return syncServer(m.value("remoteSourceId").toLongLong(), !quick);
        if (!m.value("online").toBool()) {
            emit message(tr("%1 isn't connected, so it can't be rescanned").arg(m.value("name").toString()));
            return false;
        }
        const QStringList roots = m.value("roots").toStringList();
        if (roots.isEmpty()) {
            emit message(tr("No library folders on %1").arg(m.value("name").toString()));
            return false;
        }
        const VolumeResolver resolver = m_resolver;
        return m_controller->runJob(
            "scan", quick ? tr("Quick rescan of %1").arg(m.value("name").toString()) : tr("Rescanning %1").arg(m.value("name").toString()),
            [roots, quick, resolver](Library &library, Job *job) {
                Scanner scanner(library, resolver, job);
                return scanner.scan(roots, quick ? Scanner::Mode::ChangedFolders : Scanner::Mode::Full).toJson();
            },
            [this](const QJsonObject &r) {
                m_controller->refreshAll();
                reload();
                if (!r.value("ok").toBool(true))
                    emit message(tr("Rescan failed: %1").arg(r.value("error").toString()));
                else
                    emit message(tr("Rescan finished: %1 added, %2 changed, %3 missing")
                                     .arg(r.value("added").toInt())
                                     .arg(r.value("updated").toInt())
                                     .arg(r.value("missing").toInt()));
            });
    }
    return false;
}

void NasController::reassignVolume(qint64 id) {
    // The portal's folder picker, made the first time it's needed.
    if (!m_picker)
        setPicker(new FolderPicker(this));
    m_reassigning = id;
    m_picker->pickFolders();
}

bool NasController::reassignTo(qint64 id, const QString &folder) {
    const VolumeResolver resolver = m_resolver;
    return m_controller->runJob(
        "volume", tr("Pointing the volume at %1").arg(folder),
        [id, folder, resolver](Library &library, Job *) {
            QString error;
            QJsonObject report;
            const bool ok = Volumes::reassign(library, resolver, id, folder, &error, &report);
            report["ok"] = ok;
            if (!ok)
                report["error"] = error;
            return report;
        },
        [this, folder](const QJsonObject &r) {
            m_controller->refreshAll();
            reload();
            emit message(r.value("ok").toBool() ? tr("The volume now lives at %1").arg(folder)
                                                : tr("Couldn't move the volume: %1").arg(r.value("error").toString()));
        });
}

bool NasController::forgetVolume(qint64 id) {
    return m_controller->runJob(
        "volume", tr("Forgetting a volume"),
        [id](Library &library, Job *) {
            QString error;
            qint64 removed = 0;
            const bool ok = Volumes::forget(library, id, false, &error, &removed);
            return QJsonObject{{"ok", ok}, {"error", error}, {"removed", removed}};
        },
        [this](const QJsonObject &r) {
            m_controller->refreshAll();
            reload();
            emit message(r.value("ok").toBool()
                             ? tr("Forgot the volume and its %n tracks", "", int(r.value("removed").toInteger()))
                             : tr("Couldn't forget the volume: %1").arg(r.value("error").toString()));
        });
}

bool NasController::setWorkers(qint64 id, int workers) {
    Library *lib = m_controller->library();
    QString error;
    if (!lib || !Volumes::setScanWorkers(*lib, id, workers, &error)) {
        emit message(error);
        return false;
    }
    reload();
    return true;
}

QVariantMap NasController::mountGuide(const QString &protocol, const QString &what, const QString &where) const {
    const QStringList args = {"nas", "mount-guide", protocol == "nfs" ? "--nfs" : "--smb", what.trimmed(), "--at",
                              where.trimmed()};
    QString jsonText, plainText, errText;
    {
        QTextStream out(&jsonText), err(&errText);
        Cli::runNas(args + QStringList{"--json"}, out, err);
    }
    QVariantMap m;
    const QJsonObject o = QJsonDocument::fromJson(jsonText.toUtf8()).object();
    if (!o.value("ok").toBool()) {
        QString why = o.value("error").toString();
        if (why.isEmpty())
            why = errText.trimmed().remove(QRegularExpression("^omamp: "));
        // The command's own words name its flags; the window has fields.
        if (why.startsWith("--smb looks like"))
            why = tr("The share looks like //nas/music: two slashes, the NAS's name, then the share's.");
        else if (why.startsWith("--nfs looks like"))
            why = tr("The export looks like nas:/volume1/music: the NAS's name, a colon, then the path.");
        else if (why.startsWith("mount-guide needs --at"))
            why = tr("The folder to mount it on starts with a slash, for example /mnt/music.");
        else if (why.startsWith("mount-guide needs one of"))
            why = tr("Enter the share first.");
        m["ok"] = false;
        m["error"] = why;
        return m;
    }
    {
        QTextStream out(&plainText), err(&errText);
        Cli::runNas(args, out, err);
    }
    QVariantList units;
    const QJsonObject u = o.value("units").toObject();
    for (auto it = u.constBegin(); it != u.constEnd(); ++it)
        units << QVariantMap{{"name", it.key()}, {"text", it.value().toString()}};
    // The .mount before the .automount, as they're installed.
    std::sort(units.begin(), units.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString().endsWith(".mount") > b.toMap().value("name").toString().endsWith(".mount");
    });
    m["ok"] = true;
    m["units"] = units;
    m["fstab"] = o.value("fstab").toString();
    m["commands"] = o.value("commands").toVariant();
    m["text"] = plainText;
    return m;
}

void NasController::copyText(const QString &text) const {
    if (QClipboard *c = QGuiApplication::clipboard())
        c->setText(text);
}

QString NasController::nasGuideText() const {
    QFile f(":/docs/nas.md");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// ---- Navidrome / Subsonic ---------------------------------------------------------

QString NasController::normaliseUrl(const QString &text) {
    QString t = text.trimmed();
    if (t.isEmpty())
        return t;
    if (!t.contains("://"))
        t = "http://" + t;
    while (t.endsWith('/'))
        t.chop(1);
    return t;
}

void NasController::testServer(const QString &url, const QString &user, const QString &secret, bool apiKey) {
    if (m_testThread)
        return;
    const QUrl address(normaliseUrl(url));
    if (!address.isValid() || address.host().isEmpty()) {
        emit tested(false, tr("That doesn't look like a server address (try http://nas:4533)"));
        return;
    }
    m_testThread = QThread::create([this, address, user, secret, apiKey] {
        SubsonicClient client(address, user, secret, apiKey ? SubsonicClient::Auth::ApiKey : SubsonicClient::Auth::Token);
        SubsonicServer info;
        QString error;
        const bool ok = client.ping(&info, &error);
        QString text;
        if (ok) {
            QString type = info.type.isEmpty() ? QString("Subsonic") : info.type.left(1).toUpper() + info.type.mid(1);
            text = tr("Connected to %1 %2 (API %3)").arg(type, info.serverVersion, info.apiVersion).simplified();
        } else {
            text = error;
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, text] {
                m_testThread->wait();
                delete m_testThread;
                m_testThread = nullptr;
                emit testingChanged();
                emit tested(ok, text);
            },
            Qt::QueuedConnection);
    });
    m_testThread->start();
    emit testingChanged();
}

bool NasController::addServer(const QString &url, const QString &user, const QString &secret, bool apiKey,
                              const QString &name, int maxBitrate, const QString &format) {
    const QUrl address(normaliseUrl(url));
    if (!address.isValid() || address.host().isEmpty()) {
        emit serverAdded(false, tr("That doesn't look like a server address (try http://nas:4533)"));
        return false;
    }
    if (secret.isEmpty() || (!apiKey && user.trimmed().isEmpty())) {
        emit serverAdded(false, apiKey ? tr("Enter the API key") : tr("Enter the user name and password"));
        return false;
    }
    const QString auth = apiKey ? "apikey" : "token";
    return m_controller->runJob(
        "remote", tr("Adding %1").arg(address.host()),
        [address, user, secret, auth, name, maxBitrate, format](Library &library, Job *job) {
            QString error;
            RemoteSource added;
            const qint64 id = RemoteSources::add(library, address, user.trimmed(), secret, auth, name.trimmed(), &error, &added);
            if (!id)
                return QJsonObject{{"ok", false}, {"error", error}};
            if (maxBitrate > 0 || !format.isEmpty())
                RemoteSources::setStreaming(library, id, maxBitrate, format, &error);
            const RemoteSyncResult r = RemoteSources::sync(library, id, false, job);
            QJsonObject o = r.toJson();
            o["id"] = id;
            o["name"] = added.name;
            o["added"] = true;
            return o;
        },
        [this](const QJsonObject &r) {
            reload();
            m_controller->refreshAll();
            if (!r.value("added").toBool()) {
                emit serverAdded(false, r.value("error").toString());
                return;
            }
            emit serverAdded(true, {});
            if (!r.value("ok").toBool())
                emit message(tr("Added %1, but the first sync failed: %2").arg(r.value("name").toString(), r.value("error").toString()));
            else
                emit message(tr("Added %1: %n tracks", "", int(r.value("tracks_added").toInteger() + r.value("tracks_updated").toInteger()))
                                 .arg(r.value("name").toString()));
        });
}

bool NasController::syncServer(qint64 id, bool full) {
    QString name;
    for (const QVariant &v : std::as_const(m_servers))
        if (v.toMap().value("id").toLongLong() == id)
            name = v.toMap().value("name").toString();
    return m_controller->runJob(
        "remote", full ? tr("Full sync of %1").arg(name) : tr("Syncing %1").arg(name),
        [id, full](Library &library, Job *job) { return RemoteSources::sync(library, id, full, job).toJson(); },
        [this, name](const QJsonObject &r) {
            reload();
            m_controller->refreshAll();
            if (!r.value("ok").toBool())
                emit message(tr("Sync of %1 failed: %2").arg(name, r.value("error").toString()));
            else
                emit message(tr("Synced %1: %2 new, %3 changed, %4 gone")
                                 .arg(name)
                                 .arg(r.value("tracks_added").toInteger())
                                 .arg(r.value("tracks_updated").toInteger())
                                 .arg(r.value("tracks_missing").toInteger()));
        });
}

bool NasController::removeServer(qint64 id, bool keepTracks) {
    return m_controller->runJob(
        "remote", tr("Removing a server"),
        [id, keepTracks](Library &library, Job *) {
            QString error;
            const bool ok = RemoteSources::remove(library, id, keepTracks, &error);
            return QJsonObject{{"ok", ok}, {"error", error}};
        },
        [this](const QJsonObject &r) {
            RemoteSources::clearSecretCache();
            reload();
            m_controller->refreshAll();
            emit message(r.value("ok").toBool() ? tr("Server removed") : tr("Couldn't remove the server: %1").arg(r.value("error").toString()));
        });
}

bool NasController::setStreaming(qint64 id, int maxBitrate, const QString &format) {
    Library *lib = m_controller->library();
    QString error;
    if (!lib || !RemoteSources::setStreaming(*lib, id, maxBitrate, format, &error)) {
        emit message(error);
        return false;
    }
    reload();
    return true;
}
