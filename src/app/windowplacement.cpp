#include "windowplacement.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <cmath>

namespace {

// A Lua string literal.
QString luaString(const QString &s) {
    QString out = s;
    out.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + out + '"';
}

bool fullscreenOf(const QJsonObject &c) {
    // A number on current Hyprland, a boolean on older ones.
    const QJsonValue fs = c.value("fullscreen");
    return fs.isBool() ? fs.toBool() : fs.toInt() > 0;
}

int pairAt(const QJsonObject &c, const char *key, int i) {
    return c.value(key).toArray().at(i).toInt();
}

} // namespace

WindowPlacement::WindowPlacement(QObject *parent, const QString &hyprctl) : QObject(parent) {
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") ? QStandardPaths::findExecutable("hyprctl")
              : QString();
    m_available = !m_hyprctl.isEmpty() && QFileInfo(m_hyprctl).isExecutable();
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
        if (found)
            setManaged(managed);
    });
}

WindowPlacement::~WindowPlacement() {
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    for (QProcess *p : findChildren<QProcess *>()) {
        if (p != &m_process && p->state() != QProcess::NotRunning) {
            p->disconnect();
            p->kill();
            p->waitForFinished(1000);
        }
    }
}

void WindowPlacement::setManaged(bool managed) {
    if (managed == m_managed)
        return;
    m_managed = managed;
    emit managedChanged();
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
    const QJsonObject c = clientIn(clientsJson, pid);
    if (found)
        *found = !c.isEmpty();
    if (c.isEmpty())
        return false;
    return !c.value("floating").toBool() || fullscreenOf(c);
}

QJsonObject WindowPlacement::clientIn(const QByteArray &clientsJson, qint64 pid, const QString &title) {
    const QJsonArray clients = QJsonDocument::fromJson(clientsJson).array();
    QJsonObject first;
    for (const QJsonValue &v : clients) {
        const QJsonObject c = v.toObject();
        if (c.value("pid").toInteger() != pid)
            continue;
        // Omamp has one toplevel window (its menus and sheets are drawn
        // inside it), so the first client with our pid is the window; one
        // with the title asked for wins.
        if (!title.isEmpty() && c.value("title").toString() == title)
            return c;
        if (first.isEmpty())
            first = c;
    }
    return first;
}

QStringList WindowPlacement::enterCommands(const QJsonObject &client, const QJsonArray &monitors, int width, int height) {
    const QString address = client.value("address").toString();
    if (address.isEmpty())
        return {};
    QJsonObject monitor;
    for (const QJsonValue &v : monitors)
        if (v.toObject().value("id").toInt(-1) == client.value("monitor").toInt(-2))
            monitor = v.toObject();
    if (monitor.isEmpty())
        return {};
    // The monitor's size in layout pixels, as the rule's monitor_w/_h.
    const double scale = monitor.value("scale").toDouble(1) > 0 ? monitor.value("scale").toDouble(1) : 1;
    int mw = int(std::round(monitor.value("width").toInt() / scale));
    int mh = int(std::round(monitor.value("height").toInt() / scale));
    if (monitor.value("transform").toInt() % 2 == 1)
        std::swap(mw, mh);
    const int x = monitor.value("x").toInt() + mw - width - 24;
    const int y = monitor.value("y").toInt() + int(mh * 0.05);
    const QString w = luaString("address:" + address);
    return {
        QString("hl.dsp.window.float({ window = %1, action = \"on\" })").arg(w),
        QString("hl.dsp.window.resize({ window = %1, x = %2, y = %3, exact = true })").arg(w).arg(width).arg(height),
        QString("hl.dsp.window.move({ window = %1, x = %2, y = %3, exact = true })").arg(w).arg(x).arg(y),
        QString("hl.dsp.window.pin({ window = %1, action = \"on\" })").arg(w),
    };
}

QStringList WindowPlacement::leaveCommands(const QString &address, const Saved &saved) {
    if (address.isEmpty())
        return {};
    const QString w = luaString("address:" + address);
    QStringList out;
    // A pinned window can't tile: unpin first.
    if (!saved.pinned)
        out << QString("hl.dsp.window.pin({ window = %1, action = \"off\" })").arg(w);
    if (saved.floating && saved.width > 0 && saved.height > 0) {
        out << QString("hl.dsp.window.resize({ window = %1, x = %2, y = %3, exact = true })")
                   .arg(w).arg(saved.width).arg(saved.height)
            << QString("hl.dsp.window.move({ window = %1, x = %2, y = %3, exact = true })")
                   .arg(w).arg(saved.x).arg(saved.y);
    } else if (!saved.floating) {
        out << QString("hl.dsp.window.float({ window = %1, action = \"off\" })").arg(w);
    }
    return out;
}

QString WindowPlacement::batch(const QStringList &luas) {
    QStringList parts;
    for (const QString &lua : luas)
        parts << "dispatch " + lua;
    return parts.join("; ");
}

bool WindowPlacement::batchOk(const QByteArray &reply) {
    bool any = false;
    for (const QByteArray &line : reply.split('\n')) {
        const QByteArray t = line.trimmed();
        if (t.isEmpty())
            continue;
        if (t != "ok")
            return false;
        any = true;
    }
    return any;
}

void WindowPlacement::run(const QStringList &args, std::function<void(bool, const QByteArray &)> done) {
    auto *p = new QProcess(this);
    auto *timer = new QTimer(p);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, p, &QProcess::kill);
    auto finished = std::make_shared<bool>(false);
    auto end = [p, done, finished](bool ok, const QByteArray &out) {
        if (*finished)
            return;
        *finished = true;
        p->deleteLater();
        done(ok, out);
    };
    connect(p, &QProcess::errorOccurred, this, [end](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart || e == QProcess::Crashed || e == QProcess::Timedout)
            end(false, {});
    });
    connect(p, &QProcess::finished, this, [p, end](int code, QProcess::ExitStatus status) {
        end(code == 0 && status == QProcess::NormalExit, p->readAllStandardOutput());
    });
    timer->start(2000);
    p->start(m_hyprctl, args);
}

void WindowPlacement::enterCompact(const QString &title, int width, int height, bool fresh) {
    submit({true, title, width, height, fresh});
}

void WindowPlacement::leaveCompact() {
    submit({false, {}, 0, 0, false});
}

void WindowPlacement::submit(const Request &r) {
    if (!m_available) {
        // Asynchronous all the same, so the window always waits for a signal.
        QTimer::singleShot(0, this, [this, enter = r.enter] {
            if (enter)
                emit compactPlaced(false);
            else
                emit compactLeft(false);
        });
        return;
    }
    m_pending = r;
    m_hasPending = true;
    if (!m_busy)
        startNext();
}

void WindowPlacement::startNext() {
    if (!m_hasPending)
        return;
    m_hasPending = false;
    m_busy = true;
    const Request r = m_pending;
    if (r.enter)
        enterFind(r, 0);
    else
        doLeave();
}

void WindowPlacement::finish(bool enter, bool ok) {
    m_busy = false;
    if (enter)
        emit compactPlaced(ok);
    else
        emit compactLeft(ok);
    startNext();
}

void WindowPlacement::enterFind(const Request &r, int tries) {
    run({"-j", "clients"}, [this, r, tries](bool ok, const QByteArray &out) {
        const QJsonObject client = ok ? clientIn(out, QCoreApplication::applicationPid(), r.title) : QJsonObject();
        if (client.isEmpty()) {
            // At start-up the window may not be mapped yet.
            if (ok && tries < 20) {
                QTimer::singleShot(100, this, [this, r, tries] { enterFind(r, tries + 1); });
                return;
            }
            finish(true, false);
            return;
        }
        enterPlace(r, client);
    });
}

void WindowPlacement::enterPlace(const Request &r, const QJsonObject &client) {
    run({"-j", "monitors"}, [this, r, client](bool ok, const QByteArray &out) {
        const QStringList luas = ok ? enterCommands(client, QJsonDocument::fromJson(out).array(), r.width, r.height)
                                    : QStringList();
        if (luas.isEmpty()) {
            finish(true, false);
            return;
        }
        const QString address = client.value("address").toString();
        // The state from before the first compact player; mini to micro keeps it.
        if (!m_saved.valid || address != m_address) {
            m_saved = {};
            m_saved.valid = true;
            if (!r.fresh) {
                m_saved.floating = client.value("floating").toBool() && !fullscreenOf(client);
                m_saved.pinned = client.value("pinned").toBool();
                m_saved.x = pairAt(client, "at", 0);
                m_saved.y = pairAt(client, "at", 1);
                m_saved.width = pairAt(client, "size", 0);
                m_saved.height = pairAt(client, "size", 1);
            }
            m_address = address;
        }
        run({"--batch", batch(luas)}, [this, r, address](bool ok, const QByteArray &reply) {
            if (!ok || !batchOk(reply)) {
                finish(true, false);
                return;
            }
            enterCheck(r, address, 0);
        });
    });
}

// Hyprland says "ok" even for a window it didn't find: look again.
void WindowPlacement::enterCheck(const Request &r, const QString &address, int tries) {
    run({"-j", "clients"}, [this, r, address, tries](bool ok, const QByteArray &out) {
        QJsonObject client;
        if (ok)
            for (const QJsonValue &v : QJsonDocument::fromJson(out).array())
                if (v.toObject().value("address").toString() == address)
                    client = v.toObject();
        const bool floating = client.value("floating").toBool();
        const bool sized = pairAt(client, "size", 0) <= r.width + 40 && pairAt(client, "size", 1) <= r.height + 40;
        if (floating && sized) {
            setManaged(false);
            finish(true, true);
        } else if (ok && !client.isEmpty() && tries < 5) {
            QTimer::singleShot(50, this, [this, r, address, tries] { enterCheck(r, address, tries + 1); });
        } else {
            finish(true, false);
        }
    });
}

void WindowPlacement::doLeave() {
    const QStringList luas = m_saved.valid ? leaveCommands(m_address, m_saved) : QStringList();
    if (luas.isEmpty()) {
        m_saved = {};
        finish(false, false);
        return;
    }
    const bool tiles = !m_saved.floating;
    run({"--batch", batch(luas)}, [this, tiles](bool ok, const QByteArray &reply) {
        const bool done = ok && batchOk(reply);
        if (done) {
            m_saved = {};
            setManaged(tiles);
        }
        finish(false, done);
        refresh();
    });
}
