#include "portal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#include <QUrl>

void FolderPicker::pickFolders() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(tr("Can't reach the desktop to open a folder"));
        return;
    }

    // The portal answers on a request object whose path we can predict from
    // our bus name and a token. Subscribe before calling so a fast answer is
    // never missed.
    const QString token = QString("omamp%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    if (!m_requestPath.isEmpty())
        bus.disconnect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request",
                       "Response", this, SLOT(onResponse(uint, QVariantMap)));
    m_requestPath = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    bus.connect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request", "Response",
                this, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
                                                       "/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.portal.FileChooser", "OpenFile");
    const QVariantMap options{{"handle_token", token}, {"directory", true}, {"multiple", true},
                              {"modal", true}, {"accept_label", tr("Add")}};
    call << QString() << tr("Add music folders") << options;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError())
            emit failed(tr("The folder picker didn't open: %1").arg(reply.error().message()));
        w->deleteLater();
    });
}

void FolderPicker::onResponse(uint response, const QVariantMap &results) {
    // 1 is cancelled, which is not an error.
    if (response != 0)
        return;
    QStringList paths;
    for (const QString &uri : results.value("uris").toStringList()) {
        const QUrl url(uri);
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    if (!paths.isEmpty())
        emit picked(paths);
}
