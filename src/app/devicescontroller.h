#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "portal.h"

class LibraryController;

// The window's side of device sync (Phase 6): the Devices page with its
// profiles, the preview of what a sync would do (the dry run), and syncing
// as a job in the jobs bar. QML sees it as `library.devices`.
//
// Previews and syncs both run through LibraryController::runJob, so they
// show progress, can be cancelled and wait behind a running scan.
class DevicesController : public QObject {
    Q_OBJECT
    // [{id, name, kind, target, sources: [text], rules: [text], mask, deleteOthers, writePlaylists,
    //   ready, state, stateText, freeText, lastSync, lastText}]
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    // USB drives and Syncthing folders found: [{kind, path, label, freeText}]
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // The last preview: {profileId, ok, error, copy, convert, keep, delete, skip, writeText, freeText,
    // fits, warnings: [text], rows: [{op, opText, target, detail}], more}
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewingChanged)
    Q_PROPERTY(QString defaultMask READ defaultMask CONSTANT)
    // [{label, rules: [text]}]: the format choices the page offers.
    Q_PROPERTY(QVariantList rulePresets READ rulePresets CONSTANT)

public:
    explicit DevicesController(LibraryController *library);

    QVariantList profiles() const { return m_profiles; }
    QVariantList devices() const { return m_devices; }
    QVariantMap preview() const { return m_preview; }
    bool previewing() const { return m_previewing; }
    QString defaultMask() const;
    QVariantList rulePresets() const;

    // Reads the profiles (and checks their targets) again.
    Q_INVOKABLE void reload();
    Q_INVOKABLE void refreshDevices();
    // Playlists and collections a profile can sync: [{source, label, group}].
    Q_INVOKABLE QVariantList sourceChoices() const;
    // Saves a profile (id 0 makes one). Takes the same keys profiles have.
    // Returns its id, or 0 with the reason in a toast and lastError.
    Q_INVOKABLE qint64 saveProfile(const QVariantMap &profile);
    Q_INVOKABLE bool removeProfile(qint64 id);
    Q_INVOKABLE QString lastError() const { return m_lastError; }
    // The dry run, as a job; preview fills when it ends.
    Q_INVOKABLE bool previewProfile(qint64 id);
    // Plans and syncs, as a job.
    Q_INVOKABLE bool syncProfile(qint64 id);
    Q_INVOKABLE void clearPreview();
    // The desktop's folder picker; targetChosen says what was picked.
    Q_INVOKABLE void chooseTarget();

    // The tracks a profile syncs, for its node in the tree.
    QList<qint64> trackIds(qint64 id) const;

signals:
    void profilesChanged();
    void devicesChanged();
    void previewChanged();
    void previewingChanged();
    void targetChosen(const QString &path);
    void synced(qint64 id, const QJsonObject &result);
    void message(const QString &text);

private:
    QVariantMap previewFrom(qint64 id, const QJsonObject &plan) const;
    QString nameOf(qint64 id) const;

    LibraryController *m_controller;
    QVariantList m_profiles, m_devices;
    QVariantMap m_preview;
    bool m_previewing = false;
    QString m_lastError;
    FolderPicker m_picker;
};
