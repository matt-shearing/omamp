#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QSet>
#include <QVariantList>

class LibraryController;

// The Duplicates view under Files to Edit: groups of tracks that are the same
// song, found by tags, by content or by sound (Chromaprint fingerprints,
// computed first as a job when needed). Each group lists its suggested
// keeper first, with the reason. A flat list: a heading row per group, then
// its tracks, so one ListView draws it.
//
// Nothing here deletes. The view's actions take tracks out of the library
// or move their files to the desktop's trash, and only the ones ticked.
class DuplicatesModel : public QAbstractListModel {
    Q_OBJECT
    // tags, content or fingerprint.
    Q_PROPERTY(QString by READ by WRITE setBy NOTIFY byChanged)
    Q_PROPERTY(bool searched READ searched NOTIFY found)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int groupCount READ groupCount NOTIFY found)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY found)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY found)
    Q_PROPERTY(bool fingerprintAvailable READ fingerprintAvailable CONSTANT)

public:
    enum Roles {
        KindRole = Qt::UserRole + 1, // group or track
        LabelRole,      // group: "Artist — Title"; track: the title
        DetailRole,     // track: artist and album
        ReasonRole,     // group: why its first track is the keeper
        PathRole,
        FormatRole,     // "FLAC", "MP3 · 320 kbps"
        LengthRole,
        PlaysRole,
        RatingRole,
        KeeperRole,
        SelectedRole,
        AvailableRole,
        TrackIdRole,
        GroupRole,      // the group's number, from 0
    };

    explicit DuplicatesModel(LibraryController *controller, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString by() const { return m_by; }
    void setBy(const QString &by);
    bool searched() const { return m_searched; }
    bool busy() const { return m_busy; }
    int groupCount() const { return m_groups; }
    int trackCount() const;
    int selectedCount() const { return m_selected.size(); }
    QString summary() const { return m_summary; }
    bool fingerprintAvailable() const;

    // Looks for duplicates as a job (fingerprints the library first, by sound).
    Q_INVOKABLE bool find();
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void setSelected(int row, bool on);
    // Ticks every track but each group's keeper.
    Q_INVOKABLE void selectNonKeepers();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QVariantList selectedIds() const;
    // Drops rows for tracks no longer in the library, and groups left with one.
    Q_INVOKABLE void prune(const QVariantList &goneIds);

signals:
    void byChanged();
    void found();
    void busyChanged();
    void selectionChanged();
    void message(const QString &text);

private:
    struct Row {
        bool group = false;
        int groupIndex = 0;
        QString label, detail, reason, path, format;
        qint64 trackId = 0, durationMs = 0;
        int plays = 0, rating = -1;
        bool keeper = false, available = true;
    };
    void selectionRowsChanged();

    QPointer<LibraryController> m_controller;
    QString m_by = "tags";
    QList<Row> m_rows;
    QSet<qint64> m_selected;
    int m_groups = 0;
    bool m_searched = false, m_busy = false;
    QString m_summary;
};
