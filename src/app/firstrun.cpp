#include "firstrun.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

FirstRun::FirstRun(const QString &windowConf, QObject *parent) : QObject(parent), m_path(windowConf) {
    if (m_path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_path).path());
    QSettings s(m_path, QSettings::IniFormat);
    // The first start of a version that knows about the setup decides
    // whether this is a new install, and that answer is kept, so quitting
    // half-way through the setup brings it back next time.
    QString origin = s.value("setup/origin").toString();
    if (origin != "new" && origin != "existing") {
        origin = s.allKeys().isEmpty() ? "new" : "existing";
        s.setValue("setup/origin", origin);
    }
    m_origin = origin;
    m_completed = s.value("setup/completed", false).toBool();
    m_bannerDismissed = s.value("setup/bannerDismissed", false).toBool();

    // Once per install: the sidebar is now on by default. Someone who had it
    // off had it off because that was the default, so it comes on once; from
    // then on window/infoVisible is theirs again.
    if (s.value("setup/migration", 0).toInt() < 1) {
        s.setValue("window/infoVisible", true);
        s.setValue("setup/migration", migrationVersion);
    }
    s.sync();
}

void FirstRun::save(const QString &key, const QVariant &value) {
    if (m_path.isEmpty())
        return;
    QSettings s(m_path, QSettings::IniFormat);
    s.setValue("setup/" + key, value);
}

void FirstRun::markCompleted() {
    const bool was = m_completed && m_bannerDismissed;
    m_completed = true;
    m_bannerDismissed = true;
    save("completed", true);
    save("bannerDismissed", true);
    if (!was)
        emit changed();
}

void FirstRun::dismissBanner() {
    if (m_bannerDismissed)
        return;
    m_bannerDismissed = true;
    save("bannerDismissed", true);
    emit changed();
}
