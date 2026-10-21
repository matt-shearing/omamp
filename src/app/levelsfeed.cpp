#include "levelsfeed.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

LevelsFeed::LevelsFeed(const QString &settingsPath, const QString &socketPath, QObject *parent)
    : QObject(parent), m_settingsPath(settingsPath),
      m_socketPath(socketPath.isEmpty() ? defaultSocketPath() : socketPath) {
    if (!m_settingsPath.isEmpty()) {
        QSettings s(m_settingsPath, QSettings::IniFormat);
        m_enabled = s.value("appearance/shareWithBar", m_enabled).toBool();
    }
    m_timer.setInterval(intervalMs);
    connect(&m_timer, &QTimer::timeout, this, &LevelsFeed::tick);
}

LevelsFeed::~LevelsFeed() { close(); }

QString LevelsFeed::defaultSocketPath() {
    const QString override = qEnvironmentVariable("OMAMP_LEVELS_SOCKET");
    if (!override.isEmpty())
        return override;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return dir + "/omamp-levels.sock";
}

QList<int> LevelsFeed::shrink(const QList<qreal> &levels, int count) {
    QList<int> out;
    if (levels.isEmpty() || count <= 0)
        return out;
    out.reserve(count);
    const int n = levels.size();
    for (int i = 0; i < count; ++i) {
        // Each output band takes the loudest of the input bands it covers,
        // so a narrow peak is not averaged away.
        const int from = i * n / count;
        const int to = std::max(from + 1, (i + 1) * n / count);
        qreal v = 0;
        for (int j = from; j < to && j < n; ++j)
            v = std::max(v, levels[j]);
        out << int(std::lround(std::clamp(v, 0.0, 1.0) * 100));
    }
    return out;
}

QByteArray LevelsFeed::frame(const QString &style, bool reduceMotion, bool active, const QList<int> &levels) {
    QJsonArray list;
    for (int v : levels)
        list.append(v);
    QJsonObject o{{"style", style}, {"reduceMotion", reduceMotion}, {"active", active}, {"levels", list}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

void LevelsFeed::setEnabled(bool on) {
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (!m_settingsPath.isEmpty()) {
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.setValue("appearance/shareWithBar", on);
    }
    if (m_started) {
        if (on)
            listen();
        else
            close();
    }
    emit enabledChanged();
}

bool LevelsFeed::listening() const { return m_server && m_server->isListening(); }

void LevelsFeed::start() {
    m_started = true;
    if (m_enabled)
        listen();
}

void LevelsFeed::listen() {
    if (listening())
        return;
    QDir().mkpath(QFileInfo(m_socketPath).absolutePath());
    if (!m_server) {
        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(m_server, &QLocalServer::newConnection, this, &LevelsFeed::onNewConnection);
    }
    // A socket file is there already. If another Omamp answers on it, that
    // copy feeds the bar and this one stays quiet; a stale file left by a
    // crash is removed. (Ask first: listen() with access options renames
    // its socket over the path, which would take the bar from the other.)
    // A refused connection on a local socket fails at once.
    if (QFileInfo::exists(m_socketPath)) {
        QLocalSocket probe;
        probe.connectToServer(m_socketPath);
        if (probe.waitForConnected(50)) {
            probe.abort();
            return;
        }
        QLocalServer::removeServer(m_socketPath);
    }
    if (!m_server->listen(m_socketPath))
        qWarning("omamp: cannot share the visualiser on %s: %s", qPrintable(m_socketPath),
                 qPrintable(m_server->errorString()));
}

void LevelsFeed::close() {
    m_timer.stop();
    const bool had = !m_clients.isEmpty();
    for (QLocalSocket *c : std::as_const(m_clients)) {
        c->disconnect(this);
        c->abort();
        c->deleteLater();
    }
    m_clients.clear();
    if (m_server && m_server->isListening())
        m_server->close(); // removes the socket file
    m_last.clear();
    if (had)
        emit clientsChanged();
}

void LevelsFeed::onNewConnection() {
    while (QLocalSocket *c = m_server->nextPendingConnection()) {
        m_clients << c;
        connect(c, &QLocalSocket::disconnected, this, [this, c] {
            if (m_clients.removeOne(c)) {
                c->deleteLater();
                updateClock();
                emit clientsChanged();
            }
        });
        // The plugin sends nothing; anything it does send is thrown away.
        connect(c, &QLocalSocket::readyRead, c, [c] { c->readAll(); });
        c->write(currentFrame());
        emit clientsChanged();
    }
    updateClock();
}

QByteArray LevelsFeed::currentFrame() {
    QList<int> levels;
    if (m_active && m_source)
        levels = shrink(m_source());
    return frame(m_style, m_reduceMotion, m_active, levels);
}

void LevelsFeed::broadcast(const QByteArray &line) {
    if (m_clients.isEmpty() || line == m_last)
        return;
    m_last = line;
    ++m_framesSent;
    const auto clients = m_clients;
    for (QLocalSocket *c : clients) {
        // A reader that has fallen this far behind is not reading.
        if (c->bytesToWrite() > maxBacklog) {
            c->abort();
            continue;
        }
        c->write(line);
    }
}

void LevelsFeed::tick() { broadcast(currentFrame()); }

void LevelsFeed::sendState() {
    updateClock();
    if (m_clients.isEmpty())
        return;
    broadcast(currentFrame());
}

void LevelsFeed::updateClock() {
    m_timer.setInterval(m_reduceMotion ? calmIntervalMs : intervalMs);
    const bool run = m_active && !m_clients.isEmpty() && listening();
    if (run && !m_timer.isActive())
        m_timer.start();
    else if (!run)
        m_timer.stop();
}

void LevelsFeed::setActive(bool on) {
    if (on == m_active)
        return;
    m_active = on;
    sendState();
}

void LevelsFeed::setStyle(const QString &style) {
    if (style == m_style || style.isEmpty())
        return;
    m_style = style;
    sendState();
}

void LevelsFeed::setReduceMotion(bool on) {
    if (on == m_reduceMotion)
        return;
    m_reduceMotion = on;
    sendState();
}
