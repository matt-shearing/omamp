#include "portal.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#include <QUrl>

namespace {

// The portal's file filters are a(sa(us)): a name and a list of (kind,
// pattern), kind 0 being a glob such as "*.jpg".
struct PortalFilter {
    QString name;
    QList<QPair<uint, QString>> patterns;
};
using PortalFilters = QList<PortalFilter>;

QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilter &filter) {
    arg.beginStructure();
    arg << filter.name;
    arg.beginArray(QMetaType::fromType<QPair<uint, QString>>());
    for (const auto &p : filter.patterns) {
        arg.beginStructure();
        arg << p.first << p.second;
        arg.endStructure();
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilter &filter) {
    arg.beginStructure();
    arg >> filter.name;
    arg.beginArray();
    filter.patterns.clear();
    while (!arg.atEnd()) {
        QPair<uint, QString> p;
        arg.beginStructure();
        arg >> p.first >> p.second;
        arg.endStructure();
        filter.patterns << p;
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

} // namespace

Q_DECLARE_METATYPE(PortalFilter)
Q_DECLARE_METATYPE(PortalFilters)

void FolderPicker::pickFolders() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(m_directory ? tr("Can't reach the desktop to open a folder")
                                : tr("Can't reach the desktop to open a file"));
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
    QVariantMap options{{"handle_token", token}, {"directory", m_directory}, {"multiple", m_multiple},
                        {"modal", true}, {"accept_label", m_acceptLabel.isEmpty() ? tr("Add") : m_acceptLabel}};
    if (!m_patterns.isEmpty()) {
        qDBusRegisterMetaType<QPair<uint, QString>>();
        qDBusRegisterMetaType<PortalFilter>();
        qDBusRegisterMetaType<PortalFilters>();
        PortalFilter filter;
        filter.name = m_filterName;
        for (const QString &p : m_patterns)
            filter.patterns << qMakePair(0u, p);
        options.insert("filters", QVariant::fromValue(PortalFilters{filter}));
    }
    call << QString() << (m_title.isEmpty() ? tr("Add music folders") : m_title) << options;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError())
            emit failed((m_directory ? tr("The folder picker didn't open: %1") : tr("The file picker didn't open: %1"))
                            .arg(reply.error().message()));
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
