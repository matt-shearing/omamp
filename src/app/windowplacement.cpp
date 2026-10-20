#include "windowplacement.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

WindowPlacement::WindowPlacement(QObject *parent, const QString &hyprctl) : QObject(parent) {
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") ? QStandardPaths::findExecutable("hyprctl")
              : QString();
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &WindowPlacement::ask);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(1000);
    connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_process, &QProcess::errorOccurred, this, [this] { m_timeout.stop(); });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        if (code != 0 || status != QProcess::NormalExit)
            return;
        bool found = false;
        const bool managed = managedIn(m_process.readAllStandardOutput(), QCoreApplication::applicationPid(), &found);
        // Not listed yet (still mapping): keep the last answer.
        if (found && managed != m_managed) {
            m_managed = managed;
            emit managedChanged();
        }
    });
}

WindowPlacement::~WindowPlacement() {
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void WindowPlacement::refresh() {
    if (!m_hyprctl.isEmpty())
        m_debounce.start();
}

void WindowPlacement::ask() {
    if (m_hyprctl.isEmpty() || m_process.state() != QProcess::NotRunning) {
        if (!m_hyprctl.isEmpty())
            m_debounce.start();
        return;
    }
    m_process.start(m_hyprctl, {"-j", "clients"});
    m_timeout.start();
}

bool WindowPlacement::managedIn(const QByteArray &clientsJson, qint64 pid, bool *found) {
    if (found)
        *found = false;
    const QJsonArray clients = QJsonDocument::fromJson(clientsJson).array();
    for (const QJsonValue &v : clients) {
        const QJsonObject c = v.toObject();
        if (c.value("pid").toInteger() != pid)
            continue;
        // Omamp has one toplevel window (its menus and sheets are drawn
        // inside it), so the first client with our pid is the window.
        if (found)
            *found = true;
        const bool floating = c.value("floating").toBool();
        // A number on current Hyprland, a boolean on older ones.
        const QJsonValue fs = c.value("fullscreen");
        const bool fullscreen = fs.isBool() ? fs.toBool() : fs.toInt() > 0;
        return !floating || fullscreen;
    }
    return false;
}
