#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QVariantList>

#include "playlists.h"

class Library;

// The entries of one playlist, in its order, for the playlist view: a static
// playlist's entries (duplicates and entries that match no track included)
// or an auto-playlist's tracks as its query gives them now.
//
// Selection is kept by row. Rows of a static playlist can be moved
// (Alt+Up/Down or a drag) and removed (Delete); an auto-playlist is
// read-only here, since its query decides what it holds.
class PlaylistItemsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(qint64 playlistId READ playlistId NOTIFY playlistChanged)
    Q_PROPERTY(QString name READ name NOTIFY playlistChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY playlistChanged)   // static, auto, folder
    Q_PROPERTY(QString query READ query NOTIFY playlistChanged) // auto only
    Q_PROPERTY(bool editable READ editable NOTIFY playlistChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(qint64 totalMs READ totalMs NOTIFY countChanged)
    Q_PROPERTY(int unmatched READ unmatched NOTIFY countChanged)
    // Spotify tracks in the playlist (a mixed playlist).
    Q_PROPERTY(int spotifyCount READ spotifyCount NOTIFY countChanged)
    Q_PROPERTY(int duplicates READ duplicates NOTIFY countChanged)
    Q_PROPERTY(QString error READ error NOTIFY countChanged)
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

public:
    enum Roles {
        ItemIdRole = Qt::UserRole + 1,
        TrackIdRole,
        NumberRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        DurationRole,
        AvailabilityRole, // online, offline, missing, unmatched, spotify
        DuplicateRole,    // the track is in the list more than once
        SelectedRole,
        CurrentRole,
        PathRole,
        SpotifyRole, // a Spotify track: plays on Spotify
    };

    explicit PlaylistItemsModel(QObject *parent = nullptr);

    void setLibrary(Library *library) { m_library = library; }
    // Shows a playlist (0 shows nothing).
    void setPlaylist(qint64 id);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    qint64 playlistId() const { return m_id; }
    QString name() const { return m_playlist.name; }
    QString kind() const { return m_playlist.kind; }
    QString query() const { return m_playlist.query; }
    bool editable() const { return m_playlist.kind == QLatin1String("static") && !m_locked; }
    int count() const { return m_items.size(); }
    qint64 totalMs() const { return m_totalMs; }
    int unmatched() const { return m_unmatched; }
    int spotifyCount() const { return m_spotify; }
    int duplicates() const { return m_duplicates; }
    QString error() const { return m_error; }
    int currentRow() const { return m_current; }
    void setCurrentRow(int row);
    int selectedCount() const { return m_selected.size(); }
    const QList<Playlists::Item> &items() const { return m_items; }
    // Party mode locks editing.
    void setLocked(bool locked);

    // Reads the playlist again, keeping the cursor where it can.
    Q_INVOKABLE void reload();
    Q_INVOKABLE void select(int row, int modifiers = 0);
    Q_INVOKABLE void moveCurrent(int delta, int modifiers = 0);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    // Selected rows in order; the current row when nothing is selected.
    Q_INVOKABLE QList<int> selectedRows() const;
    // The selected entries' library tracks, in order, left out when unmatched.
    Q_INVOKABLE QVariantList selectedTrackIds() const;
    Q_INVOKABLE qint64 trackId(int row) const;

    // Static playlists: moves the selected entries up or down by one place
    // (Alt+Up/Down), or to sit together from a row (a drag). Removes them.
    Q_INVOKABLE bool moveSelection(int delta);
    Q_INVOKABLE bool moveSelectionTo(int row);
    Q_INVOKABLE int removeSelection();

signals:
    void playlistChanged();
    void countChanged();
    void currentRowChanged();
    void selectionChanged();
    // The playlist's entries changed here (the tree's count is stale).
    void edited();
    void failed(const QString &message);

private:
    void load();
    void selectRows(const QList<int> &rows, int current);

    Library *m_library = nullptr;
    qint64 m_id = 0;
    Playlists::Playlist m_playlist;
    QList<Playlists::Item> m_items;
    QHash<qint64, int> m_trackUses;
    QHash<QString, int> m_spotifyUses;
    QSet<int> m_selected;
    int m_current = -1, m_anchor = -1;
    qint64 m_totalMs = 0;
    int m_unmatched = 0, m_duplicates = 0, m_spotify = 0;
    bool m_locked = false;
    QString m_error;
};
