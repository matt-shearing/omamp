#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "playlistitemsmodel.h"
#include "portal.h"

class LibraryController;

// Playlists, auto-playlists, playlist folders and collections for the
// window: what the tree's context menu, the palette, the playlist view and
// the auto-playlist and collection editors call. QML sees it as
// `library.playlists`. Every change reloads the tree.
class PlaylistsController : public QObject {
    Q_OBJECT
    Q_PROPERTY(PlaylistItemsModel *items READ items CONSTANT)
    // Goes up after any change to playlists or collections, so lists of
    // them (menus, pickers) read again.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit PlaylistsController(LibraryController *library);

    PlaylistItemsModel *items() { return &m_items; }
    int revision() const { return m_revision; }
    // After the library opens.
    void attach();

    // Each returns the new id, or 0 with a message in a toast.
    Q_INVOKABLE qint64 createPlaylist(const QString &name, qint64 parentId = 0);
    Q_INVOKABLE qint64 createFolder(const QString &name, qint64 parentId = 0);
    Q_INVOKABLE qint64 createAutoPlaylist(const QString &name, const QString &query, qint64 parentId = 0);
    // Changes an auto-playlist's name and query together; false if either fails.
    Q_INVOKABLE bool saveAutoPlaylist(qint64 id, const QString &name, const QString &query);
    Q_INVOKABLE bool rename(qint64 id, const QString &name);
    Q_INVOKABLE bool remove(qint64 id);
    // Moves a playlist or folder into a folder (0: the top level).
    Q_INVOKABLE bool moveTo(qint64 id, qint64 parentId);
    // {id, name, kind, query, parentId, items, path}, or an empty map.
    Q_INVOKABLE QVariantMap get(qint64 id) const;
    // The folders a playlist can move into, as [{id, name}] with the top level first.
    Q_INVOKABLE QVariantList folderChoices(qint64 movingId = 0) const;
    // Static playlists tracks can be added to: [{id, name}] by path.
    Q_INVOKABLE QVariantList staticPlaylists() const;
    // Chooses the playlist's node in the tree (opening its folders).
    Q_INVOKABLE int show(qint64 id);

    // Adds library tracks to a static playlist at a position (-1: the end).
    Q_INVOKABLE int addTracks(qint64 id, const QVariantList &trackIds, int at = -1);
    // Replaces Now Playing with the playlist and plays it, or queues it
    // after the current track, or adds it at the end.
    Q_INVOKABLE int play(qint64 id, int fromRow = 0);
    Q_INVOKABLE int queueNext(qint64 id);
    Q_INVOKABLE int append(qint64 id);
    // The playlist view's selection.
    Q_INVOKABLE int playSelection();
    Q_INVOKABLE int queueSelection();
    // Makes a static playlist of what is in Now Playing, in order.
    Q_INVOKABLE qint64 saveNowPlaying(const QString &name);
    // A random auto-playlist's next shuffle.
    Q_INVOKABLE void reroll(qint64 id);

    // Import and export through the desktop's file picker.
    Q_INVOKABLE void importPlaylists();
    Q_INVOKABLE void exportPlaylist(qint64 id);
    // The same without a picker: {ok, name, entries, matched, unmatched, error}.
    Q_INVOKABLE QVariantMap importFile(const QString &file, qint64 parentId = 0);
    // {ok, file, written, error}. A .pls name writes PLS; anything else M3U8.
    Q_INVOKABLE QVariantMap exportTo(qint64 id, const QString &file, bool relative = false);

    // The auto-playlist editor. checkQuery() parses and counts:
    // {ok, error, position, count, durationMs, preview: [{title, artist, album, duration}]}.
    Q_INVOKABLE QVariantMap checkQuery(const QString &text, int previewRows = 40) const;
    Q_INVOKABLE QVariantMap rulesFromQuery(const QString &text) const;
    Q_INVOKABLE QString queryFromRules(const QVariantMap &rules) const;
    Q_INVOKABLE QVariantList ruleFields() const;
    Q_INVOKABLE QVariantList ruleOperators(const QString &type) const;

    // Collections: [{id, name, query, columns, tree, sort, visible}].
    Q_INVOKABLE QVariantList collections() const;
    Q_INVOKABLE QVariantMap collection(qint64 id) const;
    // Creates (id 0) or updates; returns the id, or 0 with a toast.
    Q_INVOKABLE qint64 saveCollection(const QVariantMap &collection);
    Q_INVOKABLE bool removeCollection(qint64 id);
    Q_INVOKABLE int restoreCollections();
    // Remembers a collection's visible columns (names from columnNames()).
    Q_INVOKABLE void setCollectionColumns(qint64 id, const QStringList &columns);
    // The track list's columns by name, in its order, and the tree nodes a
    // collection can show.
    Q_INVOKABLE QStringList columnNames() const;
    Q_INVOKABLE QVariantList treeNodeChoices() const;

    // Test seams for the pickers.
    FolderPicker *importPicker() { return &m_importPicker; }
    FolderPicker *exportPicker() { return &m_exportPicker; }

signals:
    void changed();

private:
    Library *lib() const;
    void touched();
    bool fail(const QString &error);
    QString pathOf(qint64 id) const;

    LibraryController *m_controller;
    PlaylistItemsModel m_items;
    FolderPicker m_importPicker, m_exportPicker;
    qint64 m_exporting = 0;
    int m_revision = 0;
};
