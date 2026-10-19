#pragma once

#include <QObject>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QVariantList>

class LibraryController;

// The Settings page's library side: the library's own settings table
// (folders, watching, ratings in files, scrobbling, web services), read and
// written through Library::setting(). Playback settings belong to the Player
// and its omamp.conf. QML sees it as `library.appSettings`.
class SettingsController : public QObject {
    Q_OBJECT
    // Goes up after every change, so bindings that call value() read again.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool validating READ validating NOTIFY validatingChanged)

public:
    explicit SettingsController(LibraryController *library);
    ~SettingsController() override;

    int revision() const { return m_revision; }
    bool validating() const { return m_validateThread != nullptr; }

    // The keys the page may change. Anything else is refused, so the page
    // can't write a setting the core doesn't know.
    static bool editable(const QString &key);

    Q_INVOKABLE QVariant value(const QString &key, const QVariant &fallback = {}) const;
    Q_INVOKABLE bool setValue(const QString &key, const QVariant &value);
    Q_INVOKABLE bool flag(const QString &key, bool fallback) const;
    Q_INVOKABLE void setFlag(const QString &key, bool on) { setValue(key, on ? "1" : "0"); }

    // The library folders, as scans see them.
    Q_INVOKABLE QStringList folders() const;
    // Takes a folder out of the library: its tracks leave the library (the
    // files stay) and it is no longer scanned or watched.
    Q_INVOKABLE int removeFolder(const QString &folder);

    // Scrobbling services: [{service, configured, enabled, user, pending, sent, failed, lastError}].
    Q_INVOKABLE QVariantList scrobbleStatus() const;
    // Asks ListenBrainz whether the token is good, on a worker thread;
    // saves it with the user name when it is. Answers with validated().
    Q_INVOKABLE void validateListenBrainz(const QString &token);
    Q_INVOKABLE void forgetListenBrainz();

signals:
    void changed();
    void validatingChanged();
    void validated(bool ok, const QString &user, const QString &error);

private:
    LibraryController *m_controller;
    int m_revision = 0;
    QThread *m_validateThread = nullptr;
};
