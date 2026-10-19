#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <memory>

#include "librarytree.h"
#include "tracklistmodel.h"

#include "portal.h"
class Job;
class Library;
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

public:
    // An empty path means the default library (~/.local/share/omamp/library.db).
    explicit LibraryController(const QString &databasePath = {}, QObject *parent = nullptr);
    ~LibraryController() override;

    void setPlayer(Player *player) { m_player = player; }
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

private:
    void startScan(const QStringList &folders);
    void onScanFinished(const QJsonObject &result);
    void setMessage(const QString &text);

    QString m_path;
    std::unique_ptr<Library> m_library;
    LibraryTree m_tree;
    TrackListModel m_tracks;
    QPointer<Player> m_player;
    QPointer<FolderPicker> m_picker;
    bool m_ready = false;
    QString m_error, m_message, m_jobText;
    double m_jobProgress = -1;
    int m_currentNode = 0;
    QThread *m_scanThread = nullptr;
    Job *m_job = nullptr;
};
