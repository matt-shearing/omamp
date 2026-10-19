#pragma once

#include "organise.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

class FolderPicker;
class LibraryController;

// Organise files (Ctrl+Shift+O): MediaMonkey's Auto-Organize with a preview.
//
// The model is the preview: one row per chosen track, old path and new path
// and what will happen. It plans again shortly after the mask, destination
// or options change (Organiser::plan only reads). Any new path can be edited
// by hand; a path that is taken, or that two rows share, becomes a conflict
// and that track is left where it is. apply() moves the files as one job;
// the rows then show what happened, and undo() puts everything back.
class OrganiseModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString mask READ mask WRITE setMask NOTIFY optionsChanged)
    // An absolute folder, or empty for each track's own library folder.
    Q_PROPERTY(QString destination READ destination WRITE setDestination NOTIFY optionsChanged)
    Q_PROPERTY(bool moveCompanions READ moveCompanions WRITE setMoveCompanions NOTIFY optionsChanged)
    Q_PROPERTY(bool removeEmptyFolders READ removeEmptyFolders WRITE setRemoveEmptyFolders NOTIFY optionsChanged)
    Q_PROPERTY(QString maskError READ maskError NOTIFY planned)
    Q_PROPERTY(int maskErrorPosition READ maskErrorPosition NOTIFY planned)
    Q_PROPERTY(int count READ count NOTIFY planned)
    Q_PROPERTY(int moves READ moves NOTIFY planned)
    Q_PROPERTY(int unchanged READ unchanged NOTIFY planned)
    Q_PROPERTY(int conflicts READ conflicts NOTIFY planned)
    Q_PROPERTY(int problems READ problems NOTIFY planned)
    Q_PROPERTY(int companions READ companions NOTIFY planned)
    // After apply: the result in words, and the batch Undo puts back.
    Q_PROPERTY(bool applied READ applied NOTIFY appliedChanged)
    Q_PROPERTY(bool applying READ applying NOTIFY appliedChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY appliedChanged)
    Q_PROPERTY(QString lastBatch READ lastBatch NOTIFY appliedChanged)
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)

public:
    enum Roles {
        OldPathRole = Qt::UserRole + 1,
        NewPathRole,
        StatusRole,      // move, renamed, unchanged, conflict, offline, missing, error; moved, failed, skipped
        StatusTextRole,  // the status in words
        NoteRole,
        EditedRole,      // the new path was typed by hand
        TrackIdRole,
    };

    explicit OrganiseModel(LibraryController *controller, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString mask() const { return m_options.mask; }
    QString destination() const { return m_options.destination; }
    bool moveCompanions() const { return m_options.moveCompanions; }
    bool removeEmptyFolders() const { return m_options.removeEmptyFolders; }
    void setMask(const QString &mask);
    void setDestination(const QString &folder);
    void setMoveCompanions(bool on);
    void setRemoveEmptyFolders(bool on);
    QString maskError() const { return m_plan.error; }
    int maskErrorPosition() const { return m_plan.errorPosition; }
    int count() const { return m_plan.items.size(); }
    int moves() const;
    int unchanged() const;
    int conflicts() const;
    int problems() const;
    int companions() const;
    bool applied() const { return m_applied; }
    bool applying() const { return m_applying; }
    QString resultText() const { return m_resultText; }
    QString lastBatch() const { return m_lastBatch; }
    QVariantList presets() const;
    const OrganisePlan &plan() const { return m_plan; }

    void setPicker(FolderPicker *picker);

    // Starts a preview for these tracks with the last mask used.
    Q_INVOKABLE bool load(const QVariantList &ids);
    // Plans now (the model otherwise waits for typing to pause).
    Q_INVOKABLE void replan();
    // A new path typed by hand for one row.
    Q_INVOKABLE void setNewPath(int row, const QString &path);
    Q_INVOKABLE bool apply();
    Q_INVOKABLE bool undo();
    Q_INVOKABLE void pickDestination();
    Q_INVOKABLE void close();
    // Field and function names that complete what is typed before the
    // cursor: [{label, insert, start}] where insert replaces text from start.
    Q_INVOKABLE QVariantList completions(const QString &text, int cursor) const;
    // One track's new path for the mask, for a quick look; empty on error.
    Q_INVOKABLE QString sample(const QString &mask) const;

signals:
    void optionsChanged();
    void planned();
    void appliedChanged();
    void message(const QString &text);

private:
    void markConflicts();
    static QString statusText(const OrganiseItem &item);

    QPointer<LibraryController> m_controller;
    QPointer<FolderPicker> m_picker;
    QList<qint64> m_ids;
    OrganiseOptions m_options;
    OrganisePlan m_plan;
    QSet<int> m_editedRows;
    QTimer m_debounce;
    bool m_applied = false, m_applying = false;
    QString m_resultText, m_lastBatch;
};
