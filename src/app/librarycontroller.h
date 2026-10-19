#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <memory>

#include "albumgridmodel.h"
#include "columnbrowser.h"
#include "librarytree.h"
#include "palette.h"
#include "tracklistmodel.h"

#include "portal.h"
class ArtworkPrefetch;
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

public:
    // An empty path means the default library (~/.local/share/omamp/library.db).
    explicit LibraryController(const QString &databasePath = {}, QObject *parent = nullptr);
    ~LibraryController() override;

    void setPlayer(Player *player) { m_player = player; }
    // Where album pictures are cached; empty means ~/.cache/omamp/art.
    void setArtCacheDir(const QString &dir) { m_artCacheDir = dir; }
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

    // Opens the library (once). Returns ready().
    Q_INVOKABLE bool activate();
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

private:
    void startScan(const QStringList &folders);
    void onScanFinished(const QJsonObject &result);
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
    QThread *m_scanThread = nullptr;
    Job *m_job = nullptr;
};
