#include "librarycontroller.h"

#include "job.h"
#include "library.h"
#include "player.h"
#include "portal.h"
#include "scanner.h"
#include "volumes.h"

#include <QFileInfo>
#include <QLocale>
#include <QProcess>

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
}

LibraryController::~LibraryController() {
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
    m_tracks.setDatabase(m_library->db());
    m_ready = true;
    if (!m_error.isEmpty()) {
        m_error.clear();
        emit errorChanged();
    }
    emit readyChanged();
    selectNode(0);
    return true;
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
        const QString key = m_tree.key(m_currentNode);
        m_tree.reload();
        const int row = m_tree.rowOfKey(key);
        m_currentNode = row >= 0 ? row : 0;
        emit currentNodeChanged();
        // The node's scope may have new values (a new volume); reselecting
        // keeps the list, the sort and the filter.
        const LibraryTree::Node &n = m_tree.node(m_currentNode);
        m_tracks.setScope(n.where, n.params);
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
