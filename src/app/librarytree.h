#pragma once

#include <QAbstractListModel>
#include <QSqlDatabase>
#include <QVariantList>
#include <QVector>

// The library tree on the left of the window, as MediaMonkey has it: Entire
// Library, Collections, Artist, Album Artist, Album, Genre, Year, Rating,
// Composer, Location and Playlists.
//
// It is a flat list of the rows that are showing, each with its depth, so a
// plain ListView draws it and the keyboard works the same everywhere.
// Children are read from the database when a node is first expanded, and
// dropped again when it collapses, so a library with 20,000 artists costs
// nothing until someone opens Artist.
//
// Each node gives the track list a scope: a SQL condition over `tracks t`.
class LibraryTree : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        LabelRole = Qt::UserRole + 1,
        DepthRole,
        ExpandableRole,
        ExpandedRole,
        TrackCountRole, // -1 when not counted
        DimRole,        // offline volume and everything under it
        KindRole,
        KeyRole,
        DescriptionRole, // a sentence for a tooltip (Files to Edit), or empty
    };

    struct Node {
        QString key;   // unique and stable across reloads: "artist:12", "folder:3:Rock/Album"
        QString kind;  // entire, group, collection, artist, albumartist, album, letter, genre, decade,
                       // year, rating, composer, volume, folder, nowplaying, playlist (static),
                       // autoplaylist, plfolder, cgroup and cvalue (a collection's own nodes),
                       // audit, duplicates
        QString label;
        int depth = 0;
        bool expandable = false, expanded = false, dim = false;
        qint64 count = -1;
        QString where = "1"; // the scope
        QVariantList params;
        QVariant value;      // what the children loader needs (an id, a name, a path)
        QString description;
    };

    explicit LibraryTree(QObject *parent = nullptr);

    void setDatabase(const QSqlDatabase &db);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_nodes.size(); }
    const Node &node(int row) const { return m_nodes.at(row); }

    Q_INVOKABLE void expand(int row);
    Q_INVOKABLE void collapse(int row);
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE int parentRow(int row) const;
    Q_INVOKABLE int rowOfKey(const QString &key) const;
    Q_INVOKABLE QString kind(int row) const;
    Q_INVOKABLE QString key(int row) const;
    // What the node stands for: a playlist's or collection's id, an
    // artist's id, and so on.
    Q_INVOKABLE QVariant value(int row) const;
    Q_INVOKABLE bool isExpanded(int row) const;
    Q_INVOKABLE bool isExpandable(int row) const;
    // Opens each key in turn (each must show once the one before is open)
    // and returns the row of the last, or -1.
    Q_INVOKABLE int reveal(const QStringList &keys);

    // The letter an album files under in the Album node: its first letter,
    // upper case, or # for anything that isn't a letter.
    static QString letterOf(const QString &title);

    // Reads the tree again after a scan, keeping what was open.
    Q_INVOKABLE void reload();

    // Files to Edit: how many tracks each check finds, by audit id. The
    // counts are made on a worker thread (they scan the library), so the
    // nodes show without a number until they arrive.
    void setAuditCounts(const QHash<QString, qint64> &counts);
    bool hasAuditCounts() const { return !m_auditCounts.isEmpty(); }
    bool filesToEditOpen() const;

signals:
    void countChanged();
    // Files to Edit opened (or reloaded while open): its counts are wanted.
    void auditCountsWanted();

private:
    QVector<Node> topLevel() const;
    QVector<Node> children(const Node &parent) const;
    QVector<Node> albumsWhere(const Node &parent, const QString &where, const QVariantList &params) const;
    QVector<Node> peopleWithRole(const Node &parent, const QString &role, const QString &kind) const;
    QVector<Node> folderChildren(const Node &parent) const;

    QSqlDatabase m_db;
    QVector<Node> m_nodes;
    QHash<QString, qint64> m_auditCounts;
};
