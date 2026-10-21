#pragma once

#include <QObject>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class LibraryController;

// The Settings page's library side: the library's own settings table
// (folders, watching, ratings in files, scrobbling, web services), read and
// written through Library::setting(). Playback settings belong to the Player
// and its thrum.conf. QML sees it as `library.appSettings`.
class SettingsController : public QObject {
    Q_OBJECT
    // Goes up after every change, so bindings that call value() read again.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool validating READ validating NOTIFY validatingChanged)
    // Sending the scrobble queue now.
    Q_PROPERTY(bool flushing READ flushing NOTIFY flushingChanged)
    // A Last.fm step (asking for a token, or for the session) is running.
    Q_PROPERTY(bool lastFmBusy READ lastFmBusy NOTIFY lastFmChanged)
    // Last.fm's page was opened and is waiting for "Finish logging in".
    Q_PROPERTY(bool lastFmWaiting READ lastFmWaiting NOTIFY changed)

public:
    explicit SettingsController(LibraryController *library);
    ~SettingsController() override;

    int revision() const { return m_revision; }
    bool validating() const { return m_validateThread != nullptr; }
    bool flushing() const { return m_flushThread != nullptr; }
    bool lastFmBusy() const { return m_lastFmThread != nullptr; }
    bool lastFmWaiting() const;
    // How links open (Last.fm's permission page); tests capture them instead.
    void setUrlOpener(std::function<bool(const QUrl &)> opener) { m_openUrl = std::move(opener); }

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
    // Sends what waits in the scrobble queue now, on a worker thread;
    // answers with flushed().
    Q_INVOKABLE void flushScrobbles();
    // Last.fm's desktop log-in: begin asks for a token and opens the page
    // where you allow Thrum; finish, once you have, asks for the session
    // and saves it with the user name. Each answers with lastFmResult().
    Q_INVOKABLE void beginLastFm();
    Q_INVOKABLE void finishLastFm();
    Q_INVOKABLE void forgetLastFm();
    // The page that beginLastFm() opened, to open again or copy.
    Q_INVOKABLE QString lastFmAuthUrl() const { return m_lastFmUrl; }

signals:
    void changed();
    void validatingChanged();
    void validated(bool ok, const QString &user, const QString &error);
    void flushingChanged();
    void flushed(const QVariantMap &result);
    void lastFmChanged();
    // step: "begin" or "finish".
    void lastFmResult(const QString &step, bool ok, const QString &text);

private:
    LibraryController *m_controller;
    int m_revision = 0;
    QThread *m_validateThread = nullptr;
    QThread *m_flushThread = nullptr;
    QThread *m_lastFmThread = nullptr;
    QString m_lastFmUrl;
    std::function<bool(const QUrl &)> m_openUrl;
};
