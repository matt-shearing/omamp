#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QSharedPointer>
#include <QSqlDatabase>
#include <QVariantList>
#include <QVector>

// The library window's track list: every track in the current tree node that
// matches the search, in the chosen order, for libraries of 200,000 tracks
// and more.
//
// A refresh asks SQLite for the matching track ids only, in order (about
// 20 ms for 200,000 rows on the default order's index). Rows are then read a
// page at a time, only when the view asks for them, and a few dozen pages
// are kept. So the view scrolls anywhere at once, and the memory cost is one
// id per track.
//
// Selection is kept as a set of track ids, so it survives a re-sort.
class TrackListModel : public QAbstractTableModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString filterError READ filterError NOTIFY filterErrorChanged)
    Q_PROPERTY(int sortColumn READ sortColumn NOTIFY sortChanged)
    Q_PROPERTY(bool sortDescending READ sortDescending NOTIFY sortChanged)
    Q_PROPERTY(double lastQueryMs READ lastQueryMs NOTIFY countChanged)
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString playingPath READ playingPath WRITE setPlayingPath NOTIFY playingPathChanged)

public:
    enum Column { Number, Title, Artist, Album, Year, Genre, Length, Rating, Plays, Added, Format, ColumnCount };
    Q_ENUM(Column)
    enum Roles {
        RatingRole = Qt::UserRole + 1, // 0..100, or -1 when unrated
        AvailableRole,                 // false when the track's volume is offline or the file is missing
        SelectedRole,                  // "rowSelected": TableView owns "selected" and "current"
        CurrentRole,                   // "rowCurrent"
        PlayingRole,
        TrackIdRole,
        NumericRole,                   // the column holds a number: right-aligned
        RemoteRole,                    // mirrored from a Navidrome / Subsonic server
        OfflineNoteRole,               // why the track can't play ("On nas, which isn't connected"), or ""
    };

    // One track as the list shows it.
    struct Row {
        qint64 id = 0;
        QString path; // absolute; empty when offline
        QString title, artist, album, genre, format, availability, fileName;
        QString volumeKind, volumeName; // local, network or subsonic; the volume's label or root
        int year = 0, trackNo = 0, discNo = 0, bitrate = 0, plays = 0;
        int rating = -1;
        qint64 durationMs = 0, added = 0;
    };

    // db is a connection on the calling thread; the model never opens one.
    explicit TrackListModel(QObject *parent = nullptr);

    void setDatabase(const QSqlDatabase &db);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_ids.size(); }
    QString filter() const { return m_filter; }
    QString filterError() const { return m_filterError; }
    int sortColumn() const { return m_sortColumn; }
    bool sortDescending() const { return m_sortDescending; }
    double lastQueryMs() const { return m_lastQueryMs; }
    int currentRow() const { return m_currentRow; }
    int selectedCount() const { return m_selected.size(); }
    QString playingPath() const { return m_playingPath; }

    // The tree node: a SQL condition over `tracks t` and its parameters.
    void setScope(const QString &where, const QVariantList &params);
    // The column browser's choice, a third condition over `tracks t`.
    // reloadNow false only stores it, for a caller that reloads anyway (the
    // browser answering baseChanged()).
    void setBrowse(const QString &where, const QVariantList &params, bool reloadNow = true);
    // The node and the search together (what the column browser counts
    // within), and everything that decides the list (what the album grid
    // shows).
    QString baseWhere() const { return QString("(%1) AND (%2)").arg(m_scopeWhere, m_filterWhere); }
    QVariantList baseParams() const { return m_scopeParams + m_filterParams; }
    QString fullWhere() const {
        return QString("(%1) AND (%2)").arg(m_baseOverride.isEmpty() ? baseWhere() : m_baseOverride, m_browseWhere);
    }
    QVariantList fullParams() const { return (m_baseOverride.isEmpty() ? baseParams() : QVariantList()) + m_browseParams; }
    // A condition, with no parameters, that selects exactly what baseWhere()
    // does: the column browser's temporary table of the node's and search's
    // tracks, so a slow search runs once rather than once more per query.
    // Empty means use baseWhere(). Takes effect at the next reload.
    void setBaseOverride(const QString &where) { m_baseOverride = where; }
    QString scopeWhere() const { return m_scopeWhere; }
    QVariantList scopeParams() const { return m_scopeParams; }
    void setFilter(const QString &text);
    void setPlayingPath(const QString &path);
    void setCurrentRow(int row);

    // Clicking a header: the first click sorts ascending, the second
    // descending, the third goes back to the library order (-1).
    Q_INVOKABLE void sortBy(int column);
    Q_INVOKABLE void setSort(int column, bool descending);
    // Runs the query again, keeping the order, the selection and the current
    // track. Emits aboutToRefresh() first, so a view can keep its place.
    Q_INVOKABLE void refresh();

    // Selection. modifiers are Qt.ShiftModifier and Qt.ControlModifier, as
    // QML hands them over.
    Q_INVOKABLE void select(int row, int modifiers = 0);
    Q_INVOKABLE void moveCurrent(int delta, int modifiers = 0);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(int row) const;
    // The selected rows in list order; the current row when nothing is selected.
    Q_INVOKABLE QList<int> selectedRows() const;
    Q_INVOKABLE QString columnName(int column) const;
    // The rows on screen. Selection and the playing mark refresh only these:
    // a view asks again for any other row when it scrolls there, and
    // TableView handles a change to every row one cell at a time.
    Q_INVOKABLE void setVisibleRows(int first, int last);
    Q_INVOKABLE qint64 trackId(int row) const;
    Q_INVOKABLE QString path(int row) const;
    Q_INVOKABLE int rowOfTrack(qint64 id) const;

    // The tracks behind these rows, in the same order, read in batches.
    QVector<Row> rows(const QList<int> &rows) const;
    QVector<Row> rowsForIds(const QVector<qint64> &ids) const { return fetch(ids); }
    QVector<qint64> ids() const { return m_ids; }

    static QString clock(qint64 ms);

signals:
    void countChanged();
    void filterChanged();
    void filterErrorChanged();
    void sortChanged();
    void currentRowChanged();
    void selectionChanged();
    void playingPathChanged();
    // The list was reloaded (scope, filter, browser or sort changed, or a refresh).
    void reloaded();
    // The node or the search changed and the list is about to reload: the
    // column browser recounts its panes and sets its condition here.
    void baseChanged();
    // refresh() is about to reload the same list with new data.
    void aboutToRefresh();

private:
    static constexpr int pageSize = 128;
    static constexpr int maxPages = 48;

    struct Page {
        QVector<Row> rows;
        quint64 used = 0;
    };

    QString orderBy() const;
    void reload(bool keepSelection);
    const Row *rowAt(int row) const;
    QVector<Row> fetch(const QVector<qint64> &ids) const;
    void selectionDataChanged();
    void visibleDataChanged(int role);
    QVariant display(const Row &r, int column) const;

    QSqlDatabase m_db;
    QString m_scopeWhere = "1";
    QVariantList m_scopeParams;
    QString m_filter, m_filterError;
    QString m_filterWhere = "1";
    QVariantList m_filterParams;
    QString m_browseWhere = "1";
    QString m_baseOverride;
    QVariantList m_browseParams;
    int m_sortColumn = -1;
    bool m_sortDescending = false;
    double m_lastQueryMs = 0;

    QVector<qint64> m_ids;
    mutable QHash<int, QSharedPointer<Page>> m_pages;
    mutable quint64 m_useCounter = 0;

    QSet<qint64> m_selected;
    int m_currentRow = -1, m_anchorRow = -1;
    QString m_playingPath;
    int m_visibleFirst = 0, m_visibleLast = 100;
};
