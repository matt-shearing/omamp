#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

#include "autodj.h"

class LibraryController;

// Auto-DJ and party mode for the window. QML sees it as `library.dj`.
//
// Auto-DJ keeps a number of tracks queued after the one playing, drawn from
// a source (the library, a collection, a playlist or a query) by
// AutoDj::Dj, and tops the queue up after every track change.
//
// Party mode locks what guests can do (PartyMode::allowed): the window asks
// allowed() before each guarded action and greys out the rest. Leaving it
// takes the PIN when one was set; so does quitting.
class DjController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY settingsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool partyOn READ partyOn NOTIFY partyChanged)
    Q_PROPERTY(QString partyPreset READ partyPreset NOTIFY partyChanged)
    Q_PROPERTY(bool partyHasPin READ partyHasPin NOTIFY partyChanged)
    // The big now-playing display covers the window while party mode is on.
    Q_PROPERTY(bool partyDisplay READ partyDisplay NOTIFY partyChanged)

public:
    explicit DjController(LibraryController *library);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QVariantMap settings() const;
    QString sourceLabel() const;
    QString error() const { return m_error; }
    bool partyOn() const { return m_party.enabled; }
    QString partyPreset() const { return m_party.preset; }
    bool partyHasPin() const { return m_party.hasPin(); }
    bool partyDisplay() const { return m_party.enabled && m_display; }

    // Reads the saved Auto-DJ and party settings, after the library opens
    // (or from a short-lived connection at start, for party mode).
    void attach();
    void loadSaved(const QString &databasePath);
    // The library changed (a scan): the picker reads its source again.
    void invalidate() { m_dj.reset(); }

    Q_INVOKABLE void toggle() { setEnabled(!m_enabled); }
    // Saves {source, upcoming, artistSeparation, skipPlayedWithinDays,
    // weightByRating, neverPlayedBoost}; any key may be left out.
    Q_INVOKABLE void saveSettings(const QVariantMap &settings);
    // What the source picker offers: [{value, label}].
    Q_INVOKABLE QVariantList sourceChoices() const;
    // Tops up Now Playing now; returns how many tracks it added.
    Q_INVOKABLE int refill();

    // Party mode. preset: jukebox, dj or display. Returns false with a
    // toast when the library won't open.
    Q_INVOKABLE bool enableParty(const QString &preset, const QString &pin, bool display);
    // false when the PIN is wrong; party mode stays on.
    Q_INVOKABLE bool disableParty(const QString &pin);
    // True when party mode is off or allows the action, by the names
    // PartyMode::actionName() gives: play, skip, seek, volume, add, remove,
    // reorder, edit-tags, edit-playlists, settings, library, quit.
    Q_INVOKABLE bool allowed(const QString &action) const;
    Q_INVOKABLE void setPartyDisplay(bool on);

signals:
    void enabledChanged();
    void settingsChanged();
    void errorChanged();
    void partyChanged();

private:
    void setError(const QString &error);
    void scheduleRefill();

    LibraryController *m_controller;
    AutoDj::Settings m_settings;
    std::unique_ptr<AutoDj::Dj> m_dj;
    PartyMode::Settings m_party;
    bool m_enabled = false, m_display = false, m_refilling = false;
    QString m_error;
    QTimer m_refillTimer;
};
