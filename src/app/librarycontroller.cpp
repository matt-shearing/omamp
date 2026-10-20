#include "librarycontroller.h"

#include "organise.h"
#include "organisemodel.h"
#include "tageditor.h"
#include "tagfromname.h"
#include "tagwriter.h"

#include "artwork.h"
#include "djcontroller.h"
#include "playlistscontroller.h"
#include "playrecorder.h"
#include "settingscontroller.h"
#include "audit.h"
#include "duplicatesmodel.h"
#include "job.h"
#include "library.h"
#include "player.h"
#include "portal.h"
#include "scanner.h"
#include "volumes.h"
#include "watcher.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSet>
#include <QSqlError>
#include <QProcess>
#include <QSqlQuery>

LibraryController::LibraryController(const QString &databasePath, QObject *parent)
    : QObject(parent), m_path(databasePath.isEmpty() ? Library::defaultPath() : databasePath) {
    // The chosen node is a row in the tree, so it moves when rows open or
    // close above it. Closing the node that holds it chooses that node.
    connect(&m_tree, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &, int first, int last) {
        if (first <= m_currentNode) {
            m_currentNode += last - first + 1;
            emit currentNodeChanged();
        }
    });
    connect(&m_tree, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int first, int last) {
        if (m_currentNode > last) {
            m_currentNode -= last - first + 1;
            emit currentNodeChanged();
        } else if (m_currentNode >= first) {
            selectNode(first - 1);
        }
    });
    m_browser.setTracks(&m_tracks);
    m_albumGrid.setTracks(&m_tracks);
    connect(&m_tracks, &TrackListModel::playingPathChanged, this, &LibraryController::updatePlayingTrack);
    // The grid's covers come from the cache; making the rest in the
    // background the first time the grid shows lets them fill in.
    connect(&m_albumGrid, &AlbumGridModel::activeChanged, this, [this] {
        if (m_albumGrid.active())
            prefetchArtwork();
    });
    connect(&m_tree, &LibraryTree::auditCountsWanted, this, &LibraryController::countAudits);
    m_trash = [](const QString &path, QString *error) {
        QFile file(path);
        if (file.moveToTrash())
            return true;
        if (error)
            *error = file.errorString();
        return false;
    };
    m_tagEditor = new TagEditor(this, this);
    m_organiser = new OrganiseModel(this, this);
    m_tagFromName = new TagFromNameModel(this, this);
    m_duplicates = new DuplicatesModel(this, this);
    m_playlists = new PlaylistsController(this);
    m_dj = new DjController(this);
    m_settings = new SettingsController(this);
    m_recorder = new PlayRecorder(m_path, this);
    m_web = new WebController(this);
    m_about = new AboutController(this);
    m_nas = new NasController(this);
    m_devices = new DevicesController(this);
    connect(m_devices, &DevicesController::message, this, &LibraryController::toast);
    m_reportPicker.setup(tr("Save the list in which folder?"), tr("Save here"), true, false);
    connect(&m_reportPicker, &FolderPicker::picked, this, [this](const QStringList &folders) {
        if (folders.isEmpty() || m_reportFormat.isEmpty())
            return;
        QString name = currentLabel().trimmed();
        name.replace('/', '-');
        if (name.isEmpty() || name.startsWith('.'))
            name = tr("Tracks");
        exportListTo(QDir(folders.first()).filePath(name + '.' + m_reportFormat), m_reportFormat);
        m_reportFormat.clear();
    });
    connect(&m_reportPicker, &FolderPicker::failed, this, &LibraryController::toast);
    connect(m_web, &WebController::message, this, &LibraryController::toast);
    connect(m_nas, &NasController::message, this, &LibraryController::toast);
    // A counted play changes Plays and Last played on screen.
    connect(m_recorder, &PlayRecorder::recorded, this, [this](qint64, const QString &outcome) {
        if (!m_ready || outcome == QLatin1String("ignored"))
            return;
        m_tracks.refresh();
        ++m_revision;
        emit revisionChanged();
    });
}

void LibraryController::setPlayer(Player *player) {
    m_player = player;
    m_recorder->attach(player);
}

void LibraryController::reloadTree() {
    if (!m_ready)
        return;
    const QString key = m_tree.key(m_currentNode);
    m_tree.reload();
    const int row = m_tree.rowOfKey(key);
    if (row >= 0) {
        m_currentNode = row;
        emit currentNodeChanged();
    } else {
        selectNode(qMin(m_currentNode, m_tree.count() - 1));
    }
}

void LibraryController::applyWatchSettings() {
    if (!m_ready || !m_watching)
        return;
    const bool on = m_library->setting("watch.enabled", "1").toString() != "0";
    if (!on) {
        m_watcher.reset();
        return;
    }
    if (!m_watcher)
        startWatcher();
    else
        m_watcher->reload();
    const int minutes = m_library->setting("watch.network_minutes", 15).toInt();
    m_watcher->setNetworkInterval(qMax(0, minutes) * 60);
}

LibraryController::~LibraryController() {
    m_queued.clear();
    m_watcher.reset();
    // A check that finishes now would report to a half-destroyed controller.
    if (m_nas)
        m_nas->stopMonitor();
    if (m_prefetch) {
        m_prefetch->cancel();
        delete m_prefetch;
    }
    m_palette.setLibrary(nullptr);
    if (m_auditThread) {
        m_auditThread->wait();
        delete m_auditThread;
    }
    if (m_scanThread) {
        if (m_job)
            m_job->cancel();
        m_scanThread->wait();
        delete m_scanThread;
    }
}

void LibraryController::setPicker(FolderPicker *picker) {
    m_picker = picker;
    connect(picker, &FolderPicker::picked, this, &LibraryController::addFolders);
    connect(picker, &FolderPicker::failed, this, &LibraryController::setMessage);
}

bool LibraryController::hasDatabase() const { return QFileInfo::exists(m_path); }

bool LibraryController::activate() {
    if (m_ready)
        return true;
    auto library = std::make_unique<Library>(m_path);
    QString why;
    if (!library->open(&why)) {
        m_error = tr("Can't open the library: %1").arg(why);
        emit errorChanged();
        return false;
    }
    // Drives and shares that went away since the last run show as offline.
    // With the mount monitor that check runs on its thread (a hung share
    // would otherwise hold the window for a probe's 1.5 s); without it
    // (tests, benchmarks) it runs here, as before.
    if (!mountMonitoring())
        library->refreshVolumes(m_nas->resolver());
    m_library = std::move(library);
    m_tree.setDatabase(m_library->db());
    m_browser.setDatabase(m_library->db());
    m_tracks.setDatabase(m_library->db());
    m_albumGrid.setDatabase(m_library->db());
    m_palette.setLibrary(m_library.get());
    m_ready = true;
    if (!m_error.isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
    m_playlists->attach();
    m_dj->attach();
    m_nas->reload();
    m_devices->reload();
    if (mountMonitoring())
        m_nas->startMonitor();
    selectNode(0);
    updatePlayingTrack();
    if (m_library->setting("watch.enabled", "1").toString() != "0") {
        startWatcher();
        if (m_watcher)
            m_watcher->setNetworkInterval(qMax(0, m_library->setting("watch.network_minutes", 15).toInt()) * 60);
    }
    refreshUndo();
    emit readyChanged();
    return true;
}

void LibraryController::startWatcher() {
    if (!m_watching || m_watcher)
        return;
    m_watcher = std::make_unique<Watcher>(m_path);
    // A rescan by the watcher wrote to the library on its own thread; this
    // arrives on the window's thread, after the batch is committed.
    connect(m_watcher.get(), &Watcher::libraryChanged, this, &LibraryController::refreshAll);
    connect(m_watcher.get(), &Watcher::watchLimitReached, this, [this](int watched, int limit) {
        setMessage(tr("Watching %1 folders, the most allowed (%2); the rest are checked every 15 minutes")
                       .arg(QLocale().toString(watched), QLocale().toString(limit)));
    });
    // A share that comes back gets a quick rescan of just its folders.
    if (m_nas->monitor())
        m_watcher->followMounts(m_nas->monitor());
    connect(m_watcher.get(), &Watcher::rescanFinished, m_nas, [this](const QJsonObject &result) {
        if (result.value("kind").toString() == QLatin1String("remount"))
            m_nas->remountRescanned(result);
    });
    m_watcher->start();
    holdWatcher();
}

QString LibraryController::currentLabel() const {
    if (m_currentNode < 0 || m_currentNode >= m_tree.count())
        return {};
    return m_tree.node(m_currentNode).label;
}

void LibraryController::selectNode(int row) {
    if (!m_ready || row < 0 || row >= m_tree.count())
        return;
    m_currentNode = row;
    const LibraryTree::Node &n = m_tree.node(row);
    if (n.kind == QLatin1String("device")) {
        // A sync profile shows the tracks it syncs.
        QJsonArray ids;
        for (qint64 id : m_devices->trackIds(n.value.toLongLong()))
            ids << id;
        m_tracks.setScope("t.id IN (SELECT value FROM json_each(?))",
                          {QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact))});
    } else {
        m_tracks.setScope(n.where, n.params);
    }
    // Playlists show in their own view, in their own order.
    const bool playlist = n.kind == QLatin1String("playlist") || n.kind == QLatin1String("autoplaylist") ||
                          n.kind == QLatin1String("plfolder");
    const qint64 id = playlist ? n.value.toLongLong() : 0;
    if (m_playlists->items()->playlistId() != id)
        m_playlists->items()->setPlaylist(id);
    m_collection = n.kind == QLatin1String("collection") ? n.value.toLongLong() : 0;
    emit currentNodeChanged();
}

void LibraryController::addFolder() {
    // The picker answers through picked(), connected in setPicker().
    if (m_picker)
        m_picker->pickFolders();
    else
        setMessage(tr("No folder picker is available"));
}

void LibraryController::addFolders(const QStringList &folders) {
    if (folders.isEmpty())
        return;
    startScan(folders);
}

void LibraryController::rescan() { startScan({}); }

void LibraryController::cancelJob() {
    if (m_job)
        m_job->cancel();
}

void LibraryController::startScan(const QStringList &folders) {
    if (!activate())
        return;
    // A rescan of everything already running or waiting covers this one.
    bool scanAhead = m_scanThread && m_jobKind == QLatin1String("scan");
    for (const Queued &q : std::as_const(m_queued))
        scanAhead = scanAhead || q.kind == QLatin1String("scan");
    if (scanAhead && folders.isEmpty()) {
        setMessage(tr("A scan is already running"));
        return;
    }
    const QString text = folders.isEmpty() ? tr("Rescanning the library") : tr("Adding %1").arg(folders.join(", "));
    runJob("scan", text,
           [folders](Library &library, Job *job) {
               Scanner scanner(library, VolumeResolver(), job);
               const ScanResult result = folders.isEmpty() ? scanner.rescanAll() : scanner.scan(folders);
               return result.toJson();
           },
           [this](const QJsonObject &result) { onScanFinished(result); });
}

QString LibraryController::waitingText() const {
    if (m_queued.isEmpty())
        return {};
    QString text = m_jobKind == QLatin1String("scan") ? tr("Waiting for the scan to finish…")
                                                      : tr("Waiting for the running job to finish…");
    if (m_queued.size() > 1)
        text += "  " + tr("(%n jobs waiting)", "", int(m_queued.size()));
    return text;
}

void LibraryController::enqueue(const QString &kind, std::function<void()> start) {
    m_queued.append({kind, std::move(start)});
    emit jobChanged();
    emit toast(waitingText());
}

// Starts what waits, in order, until one of them takes the slot (an undo
// with nothing left to undo, say, takes nothing).
void LibraryController::startQueued() {
    while (!m_scanThread && !m_queued.isEmpty()) {
        const Queued next = m_queued.takeFirst();
        next.start();
    }
    emit jobChanged();
}

// The folder watcher holds its rescans while a job of ours runs, so the
// job never shares the library with a rescan, and starts them again after.
void LibraryController::holdWatcher() {
    if (m_watcher)
        m_watcher->setHeld(m_scanThread != nullptr);
}

bool LibraryController::runJob(const QString &kind, const QString &text, JobWork work, JobDone done) {
    if (!activate())
        return false;
    if (m_scanThread) {
        // Not refused: it waits its turn and starts the moment the slot frees.
        enqueue(kind, [this, kind, text, work = std::move(work), done = std::move(done)] {
            runJob(kind, text, work, done);
        });
        return true;
    }
    m_job = new Job(kind, this);
    m_jobKind = kind;
    m_jobDone = std::move(done);
    connect(m_job, &Job::progressChanged, this, [this, text](const QString &phase, qint64 doneCount, qint64 total) {
        const QLocale locale;
        const QString what = phase.isEmpty() ? text : phase;
        if (total > 0)
            m_jobText = tr("%1: %2 of %3").arg(what, locale.toString(doneCount), locale.toString(total));
        else if (doneCount > 0)
            m_jobText = tr("%1: %2 files").arg(what, locale.toString(doneCount));
        else
            m_jobText = what;
        m_jobProgress = total > 0 ? double(doneCount) / total : -1;
        emit jobChanged();
    });
    m_jobText = text;
    m_jobProgress = -1;

    const QString path = m_path;
    Job *job = m_job;
    m_scanThread = QThread::create([this, path, job, work = std::move(work)] {
        // Its own connection, on this thread, for the whole job.
        Library library(path);
        QString why;
        QJsonObject json;
        if (!library.open(&why))
            json = QJsonObject{{"ok", false}, {"error", why}};
        else
            json = work(library, job);
        QMetaObject::invokeMethod(this, [this, json] { finishJob(json); }, Qt::QueuedConnection);
    });
    m_scanThread->setObjectName("omamp-" + kind);
    m_scanThread->start(QThread::LowPriority);
    holdWatcher();
    emit jobChanged();
    return true;
}

void LibraryController::finishJob(const QJsonObject &result) {
    if (m_scanThread) {
        m_scanThread->wait();
        delete m_scanThread;
        m_scanThread = nullptr;
    }
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    const QString kind = m_jobKind;
    m_jobKind.clear();
    m_jobText.clear();
    m_jobProgress = -1;
    emit jobChanged();
    JobDone done = std::move(m_jobDone);
    m_jobDone = nullptr;
    if (done)
        done(result);
    emit jobFinished(kind, result);
    startQueued();
    holdWatcher();
}

void LibraryController::onScanFinished(const QJsonObject &result) {
    const QLocale locale;
    if (!result.value("ok").toBool(true)) {
        setMessage(tr("Scan failed: %1").arg(result.value("error").toString()));
    } else {
        QStringList parts;
        auto add = [&](const char *key, const QString &text) {
            const qint64 n = result.value(key).toInteger();
            if (n > 0)
                parts << text.arg(locale.toString(n));
        };
        add("added", tr("%1 added"));
        add("updated", tr("%1 changed"));
        add("missing", tr("%1 missing"));
        add("failed", tr("%1 unreadable"));
        const QString what = parts.isEmpty() ? tr("nothing new") : parts.join(", ");
        setMessage(result.value("cancelled").toBool() ? tr("Scan cancelled: %1").arg(what)
                                                      : tr("Scan finished: %1").arg(what));
    }
    if (m_ready) {
        refreshAll();
        // A new library folder needs watching.
        if (m_watcher)
            m_watcher->reload();
    }
    emit scanFinished(result);
}

void LibraryController::setMessage(const QString &text) {
    if (text == m_message)
        return;
    m_message = text;
    emit messageChanged();
}

void LibraryController::dismissMessage() { setMessage({}); }

static QList<Playlist::Track> toPlaylist(const QVector<TrackListModel::Row> &rows, QList<int> *kept = nullptr) {
    QList<Playlist::Track> out;
    for (int i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        // Offline and missing tracks can't play; they stay in the library.
        if (r.path.isEmpty() || r.availability != QLatin1String("online"))
            continue;
        Playlist::Track t;
        t.path = r.path;
        t.title = r.title.isEmpty() ? r.fileName : r.title;
        t.artist = r.artist;
        t.durationMs = r.durationMs;
        out << t;
        if (kept)
            kept->append(i);
    }
    return out;
}

int LibraryController::playRow(int row) {
    const int count = m_tracks.count();
    if (!m_player || row < 0 || row >= count)
        return 0;
    QList<int> rows;
    int first = 0, last = count;
    if (count > maxPlayNow) {
        // The chosen row and what follows it, or the last maxPlayNow rows
        // when it is near the end.
        first = qMin(row, count - maxPlayNow);
        last = first + maxPlayNow;
    }
    for (int i = first; i < last; ++i)
        rows << i;
    QList<int> kept;
    const QList<Playlist::Track> tracks = toPlaylist(m_tracks.rows(rows), &kept);
    if (tracks.isEmpty()) {
        setMessage(tr("That track is offline"));
        return 0;
    }
    // Where the chosen row landed once offline tracks were left out.
    int start = 0;
    while (start < kept.size() && rows[kept[start]] < row)
        ++start;
    return m_player->playTracks(tracks, qMin(start, int(tracks.size()) - 1));
}

int LibraryController::playSelection() {
    const QList<int> rows = m_tracks.selectedRows();
    if (rows.size() == 1)
        return playRow(rows.first());
    if (!m_player || rows.isEmpty())
        return 0;
    return m_player->playTracks(toPlaylist(m_tracks.rows(rows.mid(0, maxPlayNow))), 0);
}

int LibraryController::queueSelection() {
    const QList<int> rows = m_tracks.selectedRows();
    if (!m_player || rows.isEmpty())
        return 0;
    const int n = m_player->queueNext(toPlaylist(m_tracks.rows(rows.mid(0, maxPlayNow))));
    setMessage(tr("%n tracks will play next", "", n));
    return n;
}

int LibraryController::appendSelection() {
    const QList<int> rows = m_tracks.selectedRows();
    if (!m_player || rows.isEmpty())
        return 0;
    const int n = m_player->appendTracks(toPlaylist(m_tracks.rows(rows.mid(0, maxPlayNow))));
    setMessage(tr("%n tracks added to Now Playing", "", n));
    return n;
}

void LibraryController::showInFolder(int row) {
    const QString path = m_tracks.path(row);
    if (path.isEmpty()) {
        setMessage(tr("That track is offline"));
        return;
    }
    if (!QProcess::startDetached("xdg-open", {QFileInfo(path).absolutePath()}))
        setMessage(tr("Can't run xdg-open"));
}

// ---- Phase 1D: refresh, info, going to things --------------------------------

void LibraryController::refreshAll() {
    if (!m_ready)
        return;
    const QString key = m_tree.key(m_currentNode);
    m_tree.reload();
    const int row = m_tree.rowOfKey(key);
    m_currentNode = row >= 0 ? row : 0;
    emit currentNodeChanged();
    const LibraryTree::Node &n = m_tree.node(m_currentNode);
    if (n.where == m_tracks.scopeWhere() && n.params == m_tracks.scopeParams()) {
        // The same list with new data: keep the selection, the cursor and
        // (in the view) the scroll position.
        m_browser.recount();
        m_tracks.refresh();
    } else {
        // The node's scope changed (a new volume, say): a new list.
        m_tracks.setScope(n.where, n.params);
    }
    m_albumGrid.refresh();
    m_palette.refresh();
    m_dj->invalidate();
    if (m_playlists->items()->playlistId())
        m_playlists->items()->reload();
    updatePlayingTrack();
    refreshUndo();
    ++m_revision;
    emit revisionChanged();
}

void LibraryController::updatePlayingTrack() {
    qint64 id = 0;
    const QString path = m_tracks.playingPath();
    if (m_library && !path.isEmpty()) {
        QSqlQuery q(m_library->db());
        q.setForwardOnly(true);
        q.exec("SELECT id, root_path FROM volumes WHERE online");
        QList<QPair<qint64, QString>> volumes;
        while (q.next())
            volumes.append({q.value(0).toLongLong(), q.value(1).toString()});
        for (const auto &v : volumes) {
            const QString root = v.second == "/" ? QString() : v.second;
            if (!path.startsWith(root + '/'))
                continue;
            QSqlQuery t(m_library->db());
            t.prepare("SELECT id FROM tracks WHERE volume_id = ? AND rel_path = ?");
            t.addBindValue(v.first);
            t.addBindValue(path.mid(root.size() + 1));
            if (t.exec() && t.next()) {
                id = t.value(0).toLongLong();
                break;
            }
        }
    }
    if (id != m_playingTrackId) {
        m_playingTrackId = id;
        emit playingTrackIdChanged();
    }
}

QVariantMap LibraryController::trackInfo(qint64 trackId) const {
    QVariantMap m;
    if (!m_library || trackId <= 0)
        return m;
    QSqlQuery q(m_library->db());
    q.setForwardOnly(true);
    q.prepare("SELECT t.id, t.title, t.artist, t.album_artist, t.album, t.album_id, t.year, t.genre, t.format, "
              "t.bitrate, t.sample_rate, t.channels, t.duration_ms, t.size, t.rg_track_gain, t.rg_track_peak, "
              "t.rg_album_gain, t.rg_album_peak, t.rating, t.play_count, t.last_played, t.added, t.availability, "
              "t.has_lyrics, t.has_embedded_art, t.track_no, t.disc_no, t.composer, t.file_name, t.rel_path, "
              "v.root_path, v.online, coalesce(v.label, ''), t.bpm, t.compilation, "
              "CASE WHEN t.artwork_id IS NOT NULL OR al.artwork_id IS NOT NULL THEN 1 "
              "WHEN al.art_checked IS NOT NULL AND NOT t.has_embedded_art THEN 2 ELSE 0 END "
              "FROM tracks t JOIN volumes v ON v.id = t.volume_id LEFT JOIN albums al ON al.id = t.album_id "
              "WHERE t.id = ?");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next())
        return m;
    auto number = [&](int i) { return q.value(i).isNull() ? QVariant() : q.value(i); };
    m["id"] = q.value(0);
    m["title"] = q.value(1).toString().isEmpty() ? q.value(28).toString() : q.value(1).toString();
    m["artist"] = q.value(2).toString();
    m["albumArtist"] = q.value(3).toString();
    m["album"] = q.value(4).toString();
    m["albumId"] = q.value(5).toLongLong();
    m["year"] = q.value(6).toInt();
    m["genre"] = q.value(7).toString();
    m["format"] = q.value(8).toString().toUpper();
    m["bitrate"] = q.value(9).toInt();
    m["sampleRate"] = q.value(10).toInt();
    m["channels"] = q.value(11).toInt();
    m["durationMs"] = q.value(12).toLongLong();
    m["size"] = q.value(13).toLongLong();
    m["trackGain"] = number(14);
    m["trackPeak"] = number(15);
    m["albumGain"] = number(16);
    m["albumPeak"] = number(17);
    m["rating"] = q.value(18).isNull() ? -1 : q.value(18).toInt();
    m["plays"] = q.value(19).toInt();
    m["lastPlayed"] = q.value(20).toLongLong();
    m["added"] = q.value(21).toLongLong();
    m["availability"] = q.value(22).toString();
    m["hasLyrics"] = q.value(23).toBool();
    m["hasEmbeddedArt"] = q.value(24).toBool();
    m["trackNo"] = q.value(25).toInt();
    m["discNo"] = q.value(26).toInt();
    m["composer"] = q.value(27).toString();
    const QString root = q.value(30).toString();
    const bool online = q.value(31).toBool();
    // Where the file is: its full path, or the volume's name and the path on
    // it while the volume is offline.
    m["path"] = online ? (root == "/" ? QString() : root) + '/' + q.value(29).toString()
                       : QString("%1: %2").arg(q.value(32).toString().isEmpty() ? root : q.value(32).toString(),
                                               q.value(29).toString());
    m["online"] = online && q.value(22).toString() == "online";
    m["bpm"] = q.value(33).toInt();
    m["compilation"] = q.value(34).toBool();
    // 2: the album was searched and has no picture, so the panel doesn't ask.
    m["artState"] = q.value(35).toInt();
    return m;
}

qint64 LibraryController::firstTrackOfAlbum(qint64 albumId) const {
    if (!m_library || albumId <= 0)
        return 0;
    QSqlQuery q(m_library->db());
    q.prepare("SELECT id FROM tracks WHERE album_id = ? ORDER BY disc_no, track_no, rel_path LIMIT 1");
    q.addBindValue(albumId);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

int LibraryController::playAlbum(qint64 albumId) {
    if (!m_player || !m_library || albumId <= 0)
        return 0;
    QSqlQuery q(m_library->db());
    q.setForwardOnly(true);
    q.prepare("SELECT id FROM tracks t WHERE t.album_id = ? ORDER BY t.disc_no, t.track_no, t.rel_path");
    q.addBindValue(albumId);
    QVector<qint64> ids;
    if (q.exec())
        while (q.next())
            ids << q.value(0).toLongLong();
    const QList<Playlist::Track> tracks = toPlaylist(m_tracks.rowsForIds(ids.mid(0, maxPlayNow)));
    if (tracks.isEmpty()) {
        setMessage(tr("That album is offline"));
        return 0;
    }
    return m_player->playTracks(tracks, 0);
}

void LibraryController::setSearch(const QString &text) {
    m_tracks.setFilter(text);
    emit searchSet(text);
}

int LibraryController::showArtist(qint64 artistId) {
    if (!activate())
        return -1;
    m_browser.clear();
    if (!m_tracks.filter().isEmpty())
        setSearch({});
    const int row = m_tree.reveal({"artists", QString("artist:%1").arg(artistId)});
    if (row >= 0)
        selectNode(row);
    return row;
}

int LibraryController::showAlbum(qint64 albumId) {
    if (!activate())
        return -1;
    // The Album node files albums by the first letter of their tracks' album tag.
    QSqlQuery q(m_library->db());
    q.prepare("SELECT album FROM tracks WHERE album_id = ? LIMIT 1");
    q.addBindValue(albumId);
    if (!q.exec() || !q.next())
        return -1;
    const QString letter = "letter:" + LibraryTree::letterOf(q.value(0).toString().left(1));
    m_browser.clear();
    if (!m_tracks.filter().isEmpty())
        setSearch({});
    const int row = m_tree.reveal({"albums", letter, QString("%1/album:%2").arg(letter).arg(albumId)});
    if (row >= 0)
        selectNode(row);
    return row;
}

int LibraryController::showTrack(qint64 trackId) {
    if (!activate())
        return -1;
    int row = m_tracks.rowOfTrack(trackId);
    if (row < 0 && m_browser.filtering()) {
        m_browser.clear();
        row = m_tracks.rowOfTrack(trackId);
    }
    if (row < 0 && !m_tracks.filter().isEmpty()) {
        setSearch({});
        row = m_tracks.rowOfTrack(trackId);
    }
    if (row < 0 && m_currentNode != 0) {
        selectNode(0);
        row = m_tracks.rowOfTrack(trackId);
    }
    if (row >= 0) {
        m_tracks.select(row);
        emit revealRow(row);
    }
    return row;
}

bool LibraryController::artworkBusy() const { return m_prefetch && m_prefetch->isRunning(); }

void LibraryController::prefetchArtwork() {
    if (!m_ready || artworkBusy())
        return;
    if (!m_prefetch) {
        m_prefetch = new ArtworkPrefetch(m_path, m_artCacheDir);
        connect(m_prefetch, &ArtworkPrefetch::finished, this, [this](int made) {
            emit artworkBusyChanged();
            m_albumGrid.refresh();
            if (made > 0) {
                // Tiles that found no picture before ask again.
                ++m_revision;
                emit revisionChanged();
            }
        });
    }
    m_prefetch->start(96);
    emit artworkBusyChanged();
}
