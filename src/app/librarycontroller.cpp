#include "librarycontroller.h"

#include "artwork.h"
#include "job.h"
#include "library.h"
#include "player.h"
#include "portal.h"
#include "scanner.h"
#include "volumes.h"
#include "watcher.h"

#include <QFileInfo>
#include <QLocale>
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
}

LibraryController::~LibraryController() {
    m_watcher.reset();
    if (m_prefetch) {
        m_prefetch->cancel();
        delete m_prefetch;
    }
    m_palette.setLibrary(nullptr);
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
    library->refreshVolumes(VolumeResolver());
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
    selectNode(0);
    updatePlayingTrack();
    startWatcher();
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
    m_watcher->start();
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
    m_tracks.setScope(n.where, n.params);
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
    if (m_scanThread) {
        setMessage(tr("A scan is already running"));
        return;
    }
    m_job = new Job("scan", this);
    connect(m_job, &Job::progressChanged, this, [this](const QString &phase, qint64 done, qint64 total) {
        const QLocale locale;
        if (total > 0)
            m_jobText = tr("%1: %2 of %3").arg(phase, locale.toString(done), locale.toString(total));
        else
            m_jobText = tr("%1: %2 files").arg(phase, locale.toString(done));
        m_jobProgress = total > 0 ? double(done) / total : -1;
        emit jobChanged();
    });
    m_jobText = folders.isEmpty() ? tr("Rescanning the library") : tr("Adding %1").arg(folders.join(", "));
    m_jobProgress = -1;

    const QString path = m_path;
    Job *job = m_job;
    m_scanThread = QThread::create([this, path, folders, job] {
        // Its own connection, on this thread, for the whole scan.
        Library library(path);
        QString why;
        QJsonObject json;
        if (!library.open(&why)) {
            json = QJsonObject{{"ok", false}, {"error", why}};
        } else {
            Scanner scanner(library, VolumeResolver(), job);
            const ScanResult result = folders.isEmpty() ? scanner.rescanAll() : scanner.scan(folders);
            json = result.toJson();
        }
        QMetaObject::invokeMethod(this, [this, json] { onScanFinished(json); }, Qt::QueuedConnection);
    });
    m_scanThread->setObjectName("omamp-scan");
    m_scanThread->start(QThread::LowPriority);
    emit jobChanged();
}

void LibraryController::onScanFinished(const QJsonObject &result) {
    if (m_scanThread) {
        m_scanThread->wait();
        delete m_scanThread;
        m_scanThread = nullptr;
    }
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_jobText.clear();
    m_jobProgress = -1;
    emit jobChanged();

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
    updatePlayingTrack();
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
              "v.root_path, v.online, coalesce(v.label, ''), t.bpm, t.compilation "
              "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ?");
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
