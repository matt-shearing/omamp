#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

#include "volumes.h"

class FolderPicker;
class LibraryController;
class MountMonitor;

// The window's side of the NAS (Phase 5): the Volumes page, the mount
// monitor that notices shares coming and going, the "Connect a NAS" mount
// guide, and Navidrome / Subsonic servers. QML sees it as `library.nas`.
//
// Nothing here waits on a share in the window's thread: volumes are looked
// at by the MountMonitor on its own thread (each probe gives up after 1.5 s),
// and scans, syncs, reassigning and forgetting run as jobs.
class NasController : public QObject {
    Q_OBJECT
    // [{id, kind, kindText, name, root, online, state, stateText, tracks, offline,
    //   missing, workers, defaultWorkers, roots, remoteSourceId, source, fsType}]
    Q_PROPERTY(QVariantList volumes READ volumes NOTIFY volumesChanged)
    // A check of every volume is running.
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    // [{id, name, url, user, auth, server, lastSync, lastError, tracks, maxBitrate, format, volumeId}]
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    Q_PROPERTY(bool testing READ testing NOTIFY testingChanged)
    // What the jobs bar says about a share that came back ("music on nas is back; rescanning it").
    Q_PROPERTY(QString shareNote READ shareNote NOTIFY shareNoteChanged)

public:
    explicit NasController(LibraryController *library);
    ~NasController() override;

    // The resolver every volume look goes through; tests pass a fake mount table.
    void setResolver(const VolumeResolver &resolver) { m_resolver = resolver; }
    const VolumeResolver &resolver() const { return m_resolver; }
    // How often shares are probed for a hang (ms); the default is 30 s.
    void setPollInterval(int ms) { m_pollMs = ms; }
    // Starts watching the mount table and probing shares, and checks every
    // volume now, on the monitor's thread. Called when the library opens.
    void startMonitor();
    void stopMonitor();
    MountMonitor *monitor() const { return m_monitor.get(); }
    // The watcher's quick rescan of a share that came back finished.
    void remountRescanned(const QJsonObject &result);
    // Opens links (the Last.fm page, the NAS guide) and copies; tests replace them.
    void setPicker(FolderPicker *picker);

    QVariantList volumes() const { return m_volumes; }
    bool checking() const;
    QVariantList servers() const { return m_servers; }
    bool testing() const { return m_testThread != nullptr; }
    QString shareNote() const { return m_shareNote; }

    // Reads the volumes and servers from the library again (no probing).
    Q_INVOKABLE void reload();
    // Looks at every volume again now, on the monitor's thread.
    Q_INVOKABLE void checkNow();

    // Rescans a volume's library folders (a server is synced instead).
    Q_INVOKABLE bool rescanVolume(qint64 id, bool quick);
    // Asks for the folder where the volume's music is now (the portal's
    // picker), then reassigns it there.
    Q_INVOKABLE void reassignVolume(qint64 id);
    Q_INVOKABLE bool reassignTo(qint64 id, const QString &folder);
    // Takes an offline volume and its tracks out of the library.
    Q_INVOKABLE bool forgetVolume(qint64 id);
    // Folder walkers for a volume's scans; 0 is the default for its kind.
    Q_INVOKABLE bool setWorkers(qint64 id, int workers);

    // The mount guide: protocol "smb" or "nfs", what (//nas/music or
    // nas:/export/music) and where (/mnt/music). Returns {ok, error, units:
    // [{name, text}], fstab, commands: [..], text}. Nothing is run or written.
    Q_INVOKABLE QVariantMap mountGuide(const QString &protocol, const QString &what, const QString &where) const;
    Q_INVOKABLE void copyText(const QString &text) const;
    // docs/nas.md, compiled in.
    Q_INVOKABLE QString nasGuideText() const;

    // ---- Navidrome / Subsonic -----------------------------------------------
    // Signs in once (ping) on a worker thread and answers with tested().
    Q_INVOKABLE void testServer(const QString &url, const QString &user, const QString &secret, bool apiKey);
    // Adds the server (its secret goes to the keyring, or a 0600 file), then
    // syncs it, as one job. maxBitrate 0 streams the original.
    Q_INVOKABLE bool addServer(const QString &url, const QString &user, const QString &secret, bool apiKey,
                               const QString &name, int maxBitrate, const QString &format);
    Q_INVOKABLE bool syncServer(qint64 id, bool full);
    Q_INVOKABLE bool removeServer(qint64 id, bool keepTracks);
    Q_INVOKABLE bool setStreaming(qint64 id, int maxBitrate, const QString &format);
    // "nas:4533" and "http://nas:4533/" both become "http://nas:4533".
    static QString normaliseUrl(const QString &text);

signals:
    void volumesChanged();
    void checkingChanged();
    void serversChanged();
    void testingChanged();
    void shareNoteChanged();
    // The test's answer: the server's name and version, or why it failed.
    void tested(bool ok, const QString &text);
    // addServer() finished: ok, or the error to show in the sheet.
    void serverAdded(bool ok, const QString &error);
    void message(const QString &text);
    // A check of every volume finished (tests wait on this).
    void volumesChecked();

private:
    void onChecked(const QJsonArray &volumes);
    QString stateText(const QString &state) const;
    void setShareNote(const QString &text);

    LibraryController *m_controller;
    VolumeResolver m_resolver;
    std::unique_ptr<MountMonitor> m_monitor;
    int m_pollMs = 30000;
    QHash<qint64, QString> m_states; // the last check's state name per volume
    QVariantList m_volumes, m_servers;
    QThread *m_testThread = nullptr;
    QPointer<FolderPicker> m_picker;
    qint64 m_reassigning = 0;
    QString m_shareNote;
    QStringList m_cameBack; // roots rescanned after a share came back
};
