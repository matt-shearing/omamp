#pragma once

#include <QObject>
#include <QString>

// The first-run setup (SetupWizard.qml) and the one-time changes Thrum makes
// to an older window.conf. QML sees it as `firstRun`.
//
// It looks at window.conf once, before the window loads it:
//
// - No window.conf (or an empty one) means a new install. The setup opens on
//   start until it is finished or skipped.
// - A window.conf from an older Thrum means someone already uses it. The
//   setup is never forced on them; a small banner offers it once instead,
//   until they run it or close the banner.
//
// Either way the sidebar is switched on once (window/infoVisible), since it
// is now on by default; after that it stays as the person leaves it.
// Everything is kept under [setup] in window.conf.
class FirstRun : public QObject {
    Q_OBJECT
    // Open the setup now: a new install that hasn't finished it.
    Q_PROPERTY(bool wizardPending READ wizardPending NOTIFY changed)
    // Show the "New: choose your layout" banner: an older install that
    // hasn't run the setup or closed the banner.
    Q_PROPERTY(bool bannerPending READ bannerPending NOTIFY changed)
    // Finished or skipped at least once.
    Q_PROPERTY(bool completed READ completed NOTIFY changed)
    // "new" or "existing", from the first start of this version.
    Q_PROPERTY(QString origin READ origin CONSTANT)

public:
    // The [setup] changes this version makes once; raised when another is added.
    static constexpr int migrationVersion = 1;

    // windowConf: the window.conf file. Empty keeps everything in memory and
    // counts as already set up (tests, and windows loaded without a file).
    explicit FirstRun(const QString &windowConf = {}, QObject *parent = nullptr);

    bool wizardPending() const { return m_origin == "new" && !m_completed; }
    bool bannerPending() const { return m_origin == "existing" && !m_completed && !m_bannerDismissed; }
    bool completed() const { return m_completed; }
    QString origin() const { return m_origin; }

    // Finished or skipped: never opens by itself again, and the banner goes.
    Q_INVOKABLE void markCompleted();
    // The banner's ✕.
    Q_INVOKABLE void dismissBanner();

signals:
    void changed();

private:
    void save(const QString &key, const QVariant &value);

    QString m_path;
    QString m_origin = "existing";
    bool m_completed = true;
    bool m_bannerDismissed = true;
};
