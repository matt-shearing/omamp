#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <functional>
#include <memory>

#include "albumgridmodel.h"
#include "columnbrowser.h"
#include "librarytree.h"
#include "palette.h"
#include "tracklistmodel.h"

#include "portal.h"
#include "duplicatesmodel.h"
#include "organisemodel.h"
#include "tageditor.h"
#include "tagfromname.h"
class ArtworkPrefetch;
#include "djcontroller.h"
#include "playlistscontroller.h"
class PlayRecorder;
#include "settingscontroller.h"
#include "nascontroller.h"
#include "devicescontroller.h"
#include "webcontroller.h"
#include "aboutcontroller.h"
class Job;
class Library;
#include "watcher.h"
#include "player.h"

// The library window's side of the library: it opens the database, owns the
// tree and the track list, runs scans on a worker thread, and hands tracks
// to the player. QML sees it as `library`.
//
// The database opens the first time advanced mode shows (activate()), so the
// deck alone never touches it. A scan opens a second connection on its own
// thread; WAL lets the window keep reading while the scan writes.
class LibraryController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(LibraryTree *tree READ tree CONSTANT)
    Q_PROPERTY(TrackListModel *tracks READ tracks CONSTANT)
    Q_PROPERTY(int currentNode READ currentNode NOTIFY currentNodeChanged)
    Q_PROPERTY(QString currentKind READ currentKind NOTIFY currentNodeChanged)
    Q_PROPERTY(QString currentLabel READ currentLabel NOTIFY currentNodeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(QString jobText READ jobText NOTIFY jobChanged)
    Q_PROPERTY(double jobProgress READ jobProgress NOTIFY jobChanged) // 0..1, or -1 while unknown
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(ColumnBrowser *browser READ browser CONSTANT)
    Q_PROPERTY(AlbumGridModel *albumGrid READ albumGrid CONSTANT)
    Q_PROPERTY(Palette *palette READ palette CONSTANT)
    // The folder watcher, once the library is open (null before).
    Q_PROPERTY(Watcher *watcher READ watcher NOTIFY readyChanged)
    // The track that is playing, if it is in the library; 0 otherwise.
    Q_PROPERTY(qint64 playingTrackId READ playingTrackId NOTIFY playingTrackIdChanged)
    // Goes up whenever the library's contents change, so bindings that read
    // trackInfo() and covers ("?v=") read again.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool artworkBusy READ artworkBusy NOTIFY artworkBusyChanged)
    // Phase 2: the tag editor, organise preview, tag from file names and
    // the duplicates view, each a model QML binds to.
    Q_PROPERTY(TagEditor *tagEditor READ tagEditor CONSTANT)
    Q_PROPERTY(OrganiseModel *organiser READ organiser CONSTANT)
    Q_PROPERTY(TagFromNameModel *tagFromName READ tagFromName CONSTANT)
    Q_PROPERTY(DuplicatesModel *duplicates READ duplicates CONSTANT)
    // What Ctrl+Z and Ctrl+Shift+Z would do, in words ("tag edit of 2
    // tracks"), or empty when there is nothing; refreshed by refreshUndo().
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoChanged)
    // The kind of the running job: scan, tag, organise, undo, duplicates, trash.
    Q_PROPERTY(QString jobKind READ jobKind NOTIFY jobChanged)
    // Jobs asked for while another ran, waiting their turn, and what the
    // jobs bar says about them ("Waiting for the scan to finish…"), or "".
    Q_PROPERTY(int queuedJobs READ queuedJobs NOTIFY jobChanged)
    Q_PROPERTY(QString waitingText READ waitingText NOTIFY jobChanged)
    // Phase 3: playlists and collections, Auto-DJ and party mode, and the
    // Settings page's library settings.
    Q_PROPERTY(PlaylistsController *playlists READ playlists CONSTANT)
    Q_PROPERTY(DjController *dj READ dj CONSTANT)
    Q_PROPERTY(SettingsController *appSettings READ appSettings CONSTANT)
    // Phases 4 and 5: identify, covers and lyrics; volumes, shares and servers.
    Q_PROPERTY(WebController *web READ web CONSTANT)
    // The sidebar's About the artist and album (aboutcontroller.h).
    Q_PROPERTY(AboutController *about READ about CONSTANT)
    Q_PROPERTY(NasController *nas READ nas CONSTANT)
    // Phase 6: device sync profiles.
    Q_PROPERTY(DevicesController *devices READ devices CONSTANT)

public:
    // An empty path means the default library (~/.local/share/thrum/library.db).
    explicit LibraryController(const QString &databasePath = {}, QObject *parent = nullptr);
    ~LibraryController() override;

    // Also starts counting plays (play counts, skips, scrobbles).
    void setPlayer(Player *player);
    Player *player() const { return m_player; }
    PlaylistsController *playlists() const { return m_playlists; }
    DjController *dj() const { return m_dj; }
    SettingsController *appSettings() const { return m_settings; }
    PlayRecorder *recorder() const { return m_recorder; }
    WebController *web() const { return m_web; }
    AboutController *about() const { return m_about; }
    NasController *nas() const { return m_nas; }
    DevicesController *devices() const { return m_devices; }
    // Watching mounts (MountMonitor) checks volumes on a worker thread
    // instead of on the window's at start, and follows shares as they come
    // and go. The default follows setWatching(); tests turn it on alone.
    void setMountMonitoring(bool on) { m_monitoring = on ? 1 : 0; }
    bool mountMonitoring() const { return m_monitoring < 0 ? m_watching : m_monitoring == 1; }
    // Reads the tree again (after a playlist change), keeping the chosen node.
    Q_INVOKABLE void reloadTree();
    // Starts, stops or re-times the folder watcher from the settings.
    void applyWatchSettings();
    // Where album pictures are cached; empty means ~/.cache/thrum/art.
    void setArtCacheDir(const QString &dir) { m_artCacheDir = dir; }
    QString artCacheDir() const { return m_artCacheDir; }
    // false: don't start the folder watcher (benchmarks).
    void setWatching(bool on) { m_watching = on; }
    // The picker for "Add folder to library"; the deck has its own.
    void setPicker(FolderPicker *picker);

    bool ready() const { return m_ready; }
    QString error() const { return m_error; }
    LibraryTree *tree() { return &m_tree; }
    TrackListModel *tracks() { return &m_tracks; }
    int currentNode() const { return m_currentNode; }
    QString currentKind() const { return m_tree.kind(m_currentNode); }
    QString currentLabel() const;
    bool busy() const { return m_scanThread != nullptr; }
    QString jobText() const { return m_jobText; }
    double jobProgress() const { return m_jobProgress; }
    QString message() const { return m_message; }
    QString databasePath() const { return m_path; }
    ColumnBrowser *browser() { return &m_browser; }
    AlbumGridModel *albumGrid() { return &m_albumGrid; }
    Palette *palette() { return &m_palette; }
    Watcher *watcher() const { return m_watcher.get(); }
    qint64 playingTrackId() const { return m_playingTrackId; }
    int revision() const { return m_revision; }
    bool artworkBusy() const;
    // The window's connection (main thread only), once ready.
    Library *library() const { return m_library.get(); }
    TagEditor *tagEditor() const { return m_tagEditor; }
    OrganiseModel *organiser() const { return m_organiser; }
    TagFromNameModel *tagFromName() const { return m_tagFromName; }
    DuplicatesModel *duplicates() const { return m_duplicates; }
    QString undoText() const { return m_undoText; }
    QString redoText() const { return m_redoText; }
    QString jobKind() const { return m_jobKind; }
    int queuedJobs() const { return int(m_queued.size()); }
    QString waitingText() const;

    // Work that writes to the library or the files runs here, one job at a
    // time, on a worker thread with its own Library on the same file. work
    // gets that Library and the Job (for progress and cancel); done runs on
    // the window's thread with work's result. A job asked for while another
    // runs waits its turn (the jobs bar says so) and starts as soon as the
    // slot is free, so nothing the user asks for is dropped. The folder
    // watcher holds its rescans while any job runs. Returns false only when
    // the library won't open.
    using JobWork = std::function<QJsonObject(Library &library, Job *job)>;
    using JobDone = std::function<void(const QJsonObject &result)>;
    bool runJob(const QString &kind, const QString &text, JobWork work, JobDone done);

    // Tests fence every file the window writes, moves or trashes into this
    // folder; the tag writer and organiser refuse anything outside it.
    void setWriteSandbox(const QString &folder) { m_sandbox = folder; }
    QString writeSandbox() const { return m_sandbox; }
    // Test seam for "Move to trash": the default is QFile::moveToTrash, which
    // uses the desktop's trash (~/.local/share/Trash or the disk's .Trash-uid).
    void setTrashFunction(std::function<bool(const QString &path, QString *error)> trash) {
        m_trash = std::move(trash);
    }

    // Dates as the window shows them. English gets "12 Jul 2026", which
    // reads the same in Sydney and in Boston; other languages get their own
    // short date with a four-digit year.
    static QString dateText(const QDate &date);
    Q_INVOKABLE QString formatDate(qint64 secondsSinceEpoch) const;
    Q_INVOKABLE QString formatDateTime(qint64 secondsSinceEpoch) const;

    // Opens the library (once). Returns ready().
    Q_INVOKABLE bool activate();
    // Whether the library's database file exists, so the window can open
    // it ahead of the first Tab without creating one for a deck-only user.
    Q_INVOKABLE bool hasDatabase() const;
    Q_INVOKABLE void selectNode(int row);

    Q_INVOKABLE void addFolder();
    Q_INVOKABLE void addFolders(const QStringList &folders);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void cancelJob();
    Q_INVOKABLE void dismissMessage();

    // "Play now": replaces Now Playing with the list and plays from row.
    Q_INVOKABLE int playRow(int row);
    Q_INVOKABLE int playSelection();
    Q_INVOKABLE int queueSelection();
    Q_INVOKABLE int appendSelection();
    Q_INVOKABLE void showInFolder(int row);

    // Everything the info panel shows about a track, or an empty map.
    Q_INVOKABLE QVariantMap trackInfo(qint64 trackId) const;
    // The first track of an album in disc and track order, or 0.
    Q_INVOKABLE qint64 firstTrackOfAlbum(qint64 albumId) const;
    // Plays an album from its first track (the grid's Enter).
    Q_INVOKABLE int playAlbum(qint64 albumId);

    // Going to things, for the palette and the grid. Each chooses the tree
    // node (opening its parents), clears the search and the column browser,
    // and returns the node's row, or -1.
    Q_INVOKABLE int showArtist(qint64 artistId);
    Q_INVOKABLE int showAlbum(qint64 albumId);
    // Selects the track in the list, widening the list only as far as needed
    // (browser, then search, then Entire Library). Returns its row, or -1.
    Q_INVOKABLE int showTrack(qint64 trackId);
    // Puts text in the search box (the window follows searchSet()).
    Q_INVOKABLE void setSearch(const QString &text);

    // Reads everything on screen again after the library changed: the tree
    // (keeping what is open and chosen), the browser, the list (keeping the
    // selection), the grid and the palette.
    Q_INVOKABLE void refreshAll();
    // Makes album pictures in the background, so the grid fills in.
    Q_INVOKABLE void prefetchArtwork();

    // The selected tracks' ids in list order (the cursor's when none are
    // selected), for the tag editor, organise and tag from file names.
    Q_INVOKABLE QVariantList selectedTrackIds() const;

    // Reports (Phase 6): the list on screen, in its order, as CSV or an
    // HTML page. exportList asks for a folder and writes "<list name>.csv"
    // (or .html) there; exportListTo writes a file. Both return the number
    // of tracks written (or -1, with a toast saying why).
    Q_INVOKABLE void exportList(const QString &format);
    Q_INVOKABLE int exportListTo(const QString &file, const QString &format);
    // The tracks the list on screen holds, in order (a playlist's entries
    // when a playlist is chosen).
    QList<qint64> listTrackIds() const;
    // An album's tracks in disc and track order (the grid's chosen album).
    Q_INVOKABLE QVariantList albumTrackIds(qint64 albumId) const;
    // The Files to Edit checks, for the palette: [{id, title, description}].
    Q_INVOKABLE QVariantList auditList() const;

    // Undo (Ctrl+Z) the newest change still in effect: a tag edit, tags
    // from file names, an organise, or a redo. Redo (Ctrl+Shift+Z) undoes
    // the newest undo, until a new change is made. Both run as a job and
    // say what they did in a toast. false when there is nothing to do.
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    // Undoes one batch from the history, whatever its age.
    Q_INVOKABLE bool undoBatch(const QString &batch);
    Q_INVOKABLE void refreshUndo();
    // The journal for the Undo history view: batches newest first
    // ({batch, kind, label, when, tracks, entries, undone, canUndo}), and one
    // batch's changes ({track, title, field, oldValue, newValue, undone}).
    Q_INVOKABLE QVariantList journalBatches(int limit = 200) const;
    Q_INVOKABLE QVariantList journalEntries(const QString &batch) const;

    // Takes tracks out of the library only; the files stay where they are
    // (a rescan of their folder adds them back). Returns how many went.
    Q_INVOKABLE int removeFromLibrary(const QVariantList &ids);
    // Moves the tracks' files to the desktop's trash, as a job, and takes
    // them out of the library. Never deletes anything outright.
    Q_INVOKABLE bool moveToTrash(const QVariantList &ids);

    // Counts the Files to Edit checks on a worker thread; the tree shows them.
    Q_INVOKABLE void countAudits();

    // Which batches Ctrl+Z and Ctrl+Shift+Z act on, worked out from the
    // journal. A batch's depth is 0 for a change, 1 for its undo, 2 for the
    // undo of that undo (a redo), and so on: Ctrl+Z takes the newest batch
    // still in effect with an even depth; Ctrl+Shift+Z the newest with an
    // odd depth, unless a new change (depth 0) came after it.
    struct UndoTargets {
        QString undo, redo;
        QString undoLabel, redoLabel;
    };
    static UndoTargets undoTargets(Library &library);
    // "tag edit of 2 tracks (genre)", "organise of 12 files".
    static QString describeBatch(Library &library, const QString &batch);

    // Most tracks "play now" hands the player. Beyond this, the list from
    // the chosen row onward is taken, so Now Playing stays quick to save.
    static constexpr int maxPlayNow = 5000;

signals:
    void readyChanged();
    void errorChanged();
    void currentNodeChanged();
    void jobChanged();
    void messageChanged();
    void scanFinished(const QJsonObject &result);
    void playingTrackIdChanged();
    void revisionChanged();
    void artworkBusyChanged();
    // The search box should show this text (the filter is already set).
    void searchSet(const QString &text);
    // The list should scroll to this row and take the focus.
    void revealRow(int row);
    void undoChanged();
    // A short note for the toast over the window ("Undone: tag edit of 2 tracks").
    void toast(const QString &text);
    // A job started by runJob() finished (after its done callback ran).
    void jobFinished(const QString &kind, const QJsonObject &result);
    // The audit counts arrived (tests wait on this).
    void auditsCounted();
    // Tracks left the library (removed, or their files trashed).
    void tracksRemoved();

private:
    void startScan(const QStringList &folders);
    void onScanFinished(const QJsonObject &result);
    void finishJob(const QJsonObject &result);
    bool runUndo(const QString &batch, const QString &label, bool redo);
    // Puts work in line behind the running job; start runs when it is its turn.
    void enqueue(const QString &kind, std::function<void()> start);
    void startQueued();
    void holdWatcher();
    void setMessage(const QString &text);
    void updatePlayingTrack();
    void startWatcher();

    QString m_path;
    std::unique_ptr<Library> m_library;
    LibraryTree m_tree;
    TrackListModel m_tracks;
    ColumnBrowser m_browser;
    AlbumGridModel m_albumGrid;
    Palette m_palette;
    std::unique_ptr<Watcher> m_watcher;
    ArtworkPrefetch *m_prefetch = nullptr;
    QString m_artCacheDir;
    bool m_watching = true;
    qint64 m_playingTrackId = 0;
    int m_revision = 0;
    QPointer<Player> m_player;
    QPointer<FolderPicker> m_picker;
    bool m_ready = false;
    QString m_error, m_message, m_jobText;
    double m_jobProgress = -1;
    int m_currentNode = 0;
    QThread *m_scanThread = nullptr; // the running job's thread (any kind, not just scans)
    Job *m_job = nullptr;
    QString m_jobKind;
    JobDone m_jobDone;
    struct Queued {
        QString kind;
        std::function<void()> start;
    };
    QList<Queued> m_queued;
    QString m_sandbox;
    std::function<bool(const QString &, QString *)> m_trash;
    QString m_undoText, m_redoText;
    TagEditor *m_tagEditor = nullptr;
    OrganiseModel *m_organiser = nullptr;
    TagFromNameModel *m_tagFromName = nullptr;
    DuplicatesModel *m_duplicates = nullptr;
    QThread *m_auditThread = nullptr;
    bool m_auditAgain = false;
    PlaylistsController *m_playlists = nullptr;
    DjController *m_dj = nullptr;
    SettingsController *m_settings = nullptr;
    PlayRecorder *m_recorder = nullptr;
    WebController *m_web = nullptr;
    AboutController *m_about = nullptr;
    NasController *m_nas = nullptr;
    DevicesController *m_devices = nullptr;
    FolderPicker m_reportPicker;
    QString m_reportFormat;
    int m_monitoring = -1; // -1: follow m_watching
    // The tree node a collection's columns belong to, while one is chosen.
    qint64 m_collection = 0;
};
