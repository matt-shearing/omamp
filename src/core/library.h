#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariant>

class QUrl;
class VolumeResolver;

// One volume as refreshVolumes() found it.
struct VolumeCheck {
    qint64 id = 0;
    QString key, kind, label;
    QString root;              // where it is mounted now (the last place, when offline)
    bool wasOnline = false, online = false;
    QString state;             // ProbeResult::name(): "online", "absent", "timed-out", ...
    QStringList libraryRoots;  // the library folders on it, as absolute paths (when online)
};

// Counts for `omamp stats` and, later, the status line of the library window.
struct LibraryStats {
    qint64 tracks = 0, online = 0, offline = 0, missing = 0;
    qint64 volumes = 0, volumesOnline = 0, folders = 0;
    qint64 artists = 0, albums = 0, genres = 0, playlists = 0;
    qint64 totalDurationMs = 0, totalBytes = 0;
};

// The library database: one SQLite file on a local disk, opened in WAL mode
// through Qt SQL, and migrated forward to the newest schema when it opens.
//
// A Library is one connection and must be used on the thread that opened it.
// Another thread (a scan in the window, for example) opens its own Library on
// the same file; WAL lets readers carry on while one writer commits.
class Library {
public:
    // An empty path means the default, ~/.local/share/omamp/library.db
    // (following XDG_DATA_HOME). Tests always pass a temporary path.
    explicit Library(const QString &path = {});
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    // Opens the file, creating it and its folder if needed, and migrates it.
    // targetVersion stops the migrations early; only tests use it, to build
    // an old database and check that a later open upgrades it.
    bool open(QString *error = nullptr, int targetVersion = -1);
    bool isOpen() const;
    void close();

    QSqlDatabase db() const;
    QString path() const { return m_path; }
    int schemaVersion() const;
    // The backup written before the last migration, or empty if none was needed.
    QString lastBackup() const { return m_lastBackup; }
    QString sqliteVersion() const;

    static int latestVersion();
    static QString defaultPath();

    QVariant setting(const QString &key, const QVariant &fallback = {}) const;
    void setSetting(const QString &key, const QVariant &value);

    // Checks every known volume and marks its tracks offline or back online.
    // Missing tracks stay missing: only a scan can prove a file came back.
    // A network volume is looked at through PathProbe, so a hung share costs
    // at most one probe timeout (1.5 s) the first time and nothing after
    // that while it stays hung; the window should still prefer MountMonitor,
    // which does this on a worker thread. Remote (subsonic) volumes are left
    // alone: their state comes from talking to the server.
    QList<VolumeCheck> refreshVolumes(const VolumeResolver &resolver);

    LibraryStats stats() const;

    // The absolute path of a track on its volume's current mount, or empty
    // when the volume is offline.
    QString absolutePath(qint64 trackId) const;

    // What the player should open for a track: a file:// URL on its volume's
    // current mount, or for a track mirrored from a Subsonic server, the
    // server's stream URL (with this machine's auth parameters and the
    // source's bitrate and format limits). Empty when the track is offline,
    // missing, or its server's credentials can't be read.
    QUrl playableUrl(qint64 trackId, QString *error = nullptr) const;

private:
    bool connect(QString *error);
    bool migrate(int target, QString *error);
    bool backup(int fromVersion, QString *error);

    QString m_path, m_connection, m_lastBackup;
};
