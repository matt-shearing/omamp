#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QSet>
#include <QSqlDatabase>
#include <QVariantList>
#include <QVector>

class TrackListModel;

// One pane of the column browser: "All (n)" first, then each value with the
// number of tracks that have it. The selection is a set of keys (a genre or
// album id, an album artist's name), so it survives a recount. Nothing
// selected means All.
class FacetModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

public:
    enum Roles {
        LabelRole = Qt::UserRole + 1,
        DetailRole,   // the album artist, beside an album
        CountRole,
        SelectedRole, // "valueSelected"
        AllRole,      // the All row
        KeyRole,
    };

    struct Entry {
        QString key;   // "" for All; a number, or a name for album artists
        QString label, detail;
        qint64 count = 0;
    };

    explicit FacetModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_entries.size()); }
    QString title() const { return m_title; }
    int selectedCount() const { return int(m_selected.size()); }
    const QSet<QString> &selected() const { return m_selected; }
    const QVector<Entry> &entries() const { return m_entries; }
    Q_INVOKABLE QString label(int row) const { return row >= 0 && row < m_entries.size() ? m_entries[row].label : QString(); }
    Q_INVOKABLE QString key(int row) const { return row >= 0 && row < m_entries.size() ? m_entries[row].key : QString(); }
    Q_INVOKABLE int rowOfKey(const QString &key) const;
    Q_INVOKABLE bool isSelected(int row) const;
    // The first selected row, or 0 (All).
    Q_INVOKABLE int firstSelectedRow() const;

    void setTitle(const QString &title);
    // New values; keeps the selected keys that are still there. Returns true
    // when the selection lost a key.
    bool setEntries(QVector<Entry> entries);
    // Click, Ctrl+click (toggle), Shift+click (range from the last click).
    // Row 0 (All) clears. Returns true when the selection changed.
    bool select(int row, int modifiers);
    bool setSelectedKeys(const QSet<QString> &keys);

signals:
    void countChanged();
    void titleChanged();
    void selectionChanged();

private:
    QVector<Entry> m_entries;
    QSet<QString> m_selected;
    QString m_title;
    int m_anchor = 0;
};

// The column browser above the track list, as foobar2000's facets and
// MediaMonkey's filters have it: Genre | Album Artist (or Artist) | Album.
// Each pane lists the values within the current tree node and search, and
// within what is chosen in the panes to its left; the track list shows the
// tracks that match every pane.
//
// Counts come from GROUP BY queries over the tracks the node and search
// select, so nothing is loaded per track. While it is off the browser runs
// no queries and doesn't filter the list.
class ColumnBrowser : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString artistMode READ artistMode WRITE setArtistMode NOTIFY artistModeChanged)
    Q_PROPERTY(FacetModel *genres READ genres CONSTANT)
    Q_PROPERTY(FacetModel *artists READ artists CONSTANT)
    Q_PROPERTY(FacetModel *albums READ albums CONSTANT)
    Q_PROPERTY(bool filtering READ filtering NOTIFY filteringChanged)
    Q_PROPERTY(double lastQueryMs READ lastQueryMs NOTIFY counted)

public:
    enum Pane { GenrePane, ArtistPane, AlbumPane, PaneCount };
    Q_ENUM(Pane)

    explicit ColumnBrowser(QObject *parent = nullptr);

    void setDatabase(const QSqlDatabase &db);
    void setTracks(TrackListModel *tracks);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    // "albumartist" (one value per track, the default) or "artist" (every
    // artist of a track, as the tree's Artist node).
    QString artistMode() const { return m_artistMode; }
    void setArtistMode(const QString &mode);
    FacetModel *genres() { return &m_panes[GenrePane]; }
    FacetModel *artists() { return &m_panes[ArtistPane]; }
    FacetModel *albums() { return &m_panes[AlbumPane]; }
    Q_INVOKABLE FacetModel *pane(int pane) { return pane >= 0 && pane < PaneCount ? &m_panes[pane] : nullptr; }
    // Something is chosen in a pane, so the list shows less than the node.
    bool filtering() const;
    double lastQueryMs() const { return m_lastQueryMs; }

    Q_INVOKABLE void select(int pane, int row, int modifiers = 0);
    // Chooses values by key (for the palette and tests).
    Q_INVOKABLE void selectKeys(int pane, const QStringList &keys);
    // Back to All in every pane.
    Q_INVOKABLE void clear();
    // Counts again (after the library changed), keeping the selection. The
    // caller refreshes the track list.
    Q_INVOKABLE void recount();

signals:
    void enabledChanged();
    void artistModeChanged();
    void filteringChanged();
    void counted();

private:
    // Recounts panes from `from` on, then hands the list its condition.
    void update(int from, bool reloadList);
    QVector<FacetModel::Entry> countPane(int pane, const QString &where, const QVariantList &params) const;
    // The condition a pane's selection puts on `tracks t`; empty when All.
    QString condition(int pane, QVariantList *params) const;
    void applyCondition(bool reloadList);

    QSqlDatabase m_db;
    QPointer<TrackListModel> m_tracks;
    FacetModel m_panes[PaneCount];
    bool m_enabled = false;
    bool m_wasFiltering = false;
    bool m_baseReady = false; // the temporary table holds the current base
    QString m_artistMode = "albumartist";
    double m_lastQueryMs = 0;
};
