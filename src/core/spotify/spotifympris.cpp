#include "spotifympris.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

namespace Spotify {

namespace {

const char *kPath = "/org/mpris/MediaPlayer2";
const char *kPlayer = "org.mpris.MediaPlayer2.Player";
const char *kProperties = "org.freedesktop.DBus.Properties";

// A nested a{sv} arrives as a QDBusArgument.
QVariantMap toMap(const QVariant &v) {
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<QVariantMap>(v.value<QDBusArgument>());
    return v.toMap();
}

QStringList toStrings(const QVariant &v) {
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<QStringList>(v.value<QDBusArgument>());
    if (v.typeId() == QMetaType::QStringList)
        return v.toStringList();
    return v.toString().isEmpty() ? QStringList() : QStringList{v.toString()};
}

QVariant unwrap(const QVariant &v) {
    if (v.canConvert<QDBusVariant>())
        return v.value<QDBusVariant>().variant();
    return v;
}

} // namespace

Item itemFromMpris(const QVariantMap &m) {
    Item t;
    t.kind = "track";
    t.name = m.value("xesam:title").toString();
    // spotify_player joins the artists into one string; others list them.
    for (const QString &a : toStrings(m.value("xesam:artist")))
        for (const QString &part : a.split(", ", Qt::SkipEmptyParts))
            t.artists << part;
    t.album = m.value("xesam:album").toString();
    t.durationMs = m.value("mpris:length").toLongLong() / 1000;
    t.imageUrl = m.value("mpris:artUrl").toString();
    QString kind, id;
    if (parseReference(m.value("xesam:url").toString(), &kind, &id) && kind == "track")
        t.id = id;
    return t;
}

MprisWatch::MprisWatch(QObject *parent) : QObject(parent), m_bus(connection()) {}

MprisWatch::~MprisWatch() { unwatch(); }

QDBusConnection MprisWatch::connection() {
    const QString address = qEnvironmentVariable("THRUM_SPOTIFY_MPRIS_BUS");
    if (address.isEmpty())
        return QDBusConnection::sessionBus();
    const QString name = "thrum-spotify-mpris-" + QString::number(qHash(address), 16);
    QDBusConnection c(name);
    if (c.isConnected())
        return c;
    return QDBusConnection::connectToBus(address, name);
}

void MprisWatch::setService(const QString &busName) {
    const QString env = qEnvironmentVariable("THRUM_SPOTIFY_MPRIS_NAME");
    const QString name = busName.isEmpty() ? QString() : (env.isEmpty() ? busName : env);
    // The environment can name a different bus too.
    const QDBusConnection bus = connection();
    if (name == m_service && bus.name() == m_bus.name())
        return;
    unwatch();
    m_bus = bus;
    m_service = name;
    if (!m_service.isEmpty())
        watch();
}

void MprisWatch::watch() {
    if (!m_bus.isConnected())
        return;
    m_watcher = new QDBusServiceWatcher(m_service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &owner) {
                if (owner.isEmpty()) {
                    setAvailable(false);
                } else {
                    refreshAll();
                }
            });
    m_connected = m_bus.connect(m_service, kPath, kProperties, "PropertiesChanged", this,
                                SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    m_bus.connect(m_service, kPath, kPlayer, "Seeked", this, SLOT(onSeeked(QDBusMessage)));
    if (m_bus.interface() && m_bus.interface()->isServiceRegistered(m_service))
        refreshAll();
}

void MprisWatch::unwatch() {
    ++m_generation;
    if (!m_service.isEmpty() && m_bus.isConnected()) {
        m_bus.disconnect(m_service, kPath, kProperties, "PropertiesChanged", this,
                         SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
        m_bus.disconnect(m_service, kPath, kPlayer, "Seeked", this, SLOT(onSeeked(QDBusMessage)));
    }
    delete m_watcher;
    m_connected = false;
    setAvailable(false);
}

void MprisWatch::setAvailable(bool on) {
    if (on == m_available)
        return;
    m_available = on;
    if (!on) {
        m_status = "Stopped";
        m_metadata.clear();
        m_positionUs = 0;
        m_positionAt.invalidate();
    }
    emit availableChanged();
    emit changed();
}

void MprisWatch::refreshAll() {
    if (m_service.isEmpty() || !m_bus.isConnected())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, kPath, kProperties, "GetAll");
    call << QString(kPlayer);
    auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(call, 5000), this);
    const int generation = m_generation;
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, generation] {
        w->deleteLater();
        if (generation != m_generation)
            return;
        const QDBusMessage reply = w->reply();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            setAvailable(false);
            return;
        }
        const QVariantMap all = toMap(reply.arguments().at(0));
        const bool was = m_available;
        m_available = true;
        apply(all);
        if (!was)
            emit availableChanged();
    });
}

void MprisWatch::refreshPosition() {
    if (!m_available)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, kPath, kProperties, "Get");
    call << QString(kPlayer) << QString("Position");
    auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(call, 3000), this);
    const int generation = m_generation;
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, generation] {
        w->deleteLater();
        if (generation != m_generation)
            return;
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            setPosition(unwrap(reply.arguments().at(0)).toLongLong());
        emit changed();
    });
}

void MprisWatch::setPosition(qint64 us) {
    m_positionUs = qMax<qint64>(0, us);
    m_positionAt.restart();
}

void MprisWatch::apply(const QVariantMap &p, bool announce) {
    if (p.contains("PlaybackStatus")) {
        // Keep the moved-on position when the status changes.
        const qint64 now = state().progressMs * 1000;
        m_status = unwrap(p.value("PlaybackStatus")).toString();
        setPosition(now);
    }
    if (p.contains("Metadata")) {
        const QVariantMap m = toMap(unwrap(p.value("Metadata")));
        if (m.value("xesam:title") != m_metadata.value("xesam:title") ||
            m.value("mpris:length") != m_metadata.value("mpris:length"))
            setPosition(0);
        m_metadata = m;
    }
    if (p.contains("Position"))
        setPosition(unwrap(p.value("Position")).toLongLong());
    if (announce)
        emit changed();
}

void MprisWatch::onPropertiesChanged(const QString &interface, const QVariantMap &changedProperties,
                                     const QStringList &invalidated) {
    if (interface != kPlayer)
        return;
    if (!m_available) {
        refreshAll();
        return;
    }
    // Position is rarely announced with the rest, so it is read before the
    // change is passed on: a pause is then seen with the position it
    // stopped at.
    const bool withPosition = changedProperties.contains("Position") && !invalidated.contains("Position");
    apply(changedProperties, withPosition);
    if (!withPosition)
        refreshPosition();
}

void MprisWatch::onSeeked(const QDBusMessage &message) {
    // The MPRIS signal carries the position in microseconds; spotify_player
    // declares it with another type, so read it back instead of trusting it.
    if (!message.arguments().isEmpty() && message.signature() == "x") {
        setPosition(message.arguments().at(0).toLongLong());
        emit changed();
    }
    refreshPosition();
}

Playback MprisWatch::state() const {
    Playback p;
    if (!m_available || m_metadata.isEmpty())
        return p;
    p.track = itemFromMpris(m_metadata);
    if (p.track.name.isEmpty())
        return p;
    p.valid = m_status != "Stopped";
    p.playing = m_status == "Playing";
    qint64 ms = m_positionUs / 1000;
    if (p.playing && m_positionAt.isValid())
        ms += m_positionAt.elapsed();
    if (p.track.durationMs > 0)
        ms = qMin(ms, p.track.durationMs);
    p.progressMs = ms;
    return p;
}

} // namespace Spotify

bool Spotify::MprisWatch::pauseNow(int timeoutMs) {
    if (m_service.isEmpty() || !m_bus.isConnected())
        return false;
    const QDBusMessage pause =
        QDBusMessage::createMethodCall(m_service, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Pause");
    const QDBusMessage reply = m_bus.call(pause, QDBus::Block, timeoutMs);
    return reply.type() == QDBusMessage::ReplyMessage;
}

void Spotify::MprisWatch::setVolume(qreal volume) {
    if (m_service.isEmpty())
        return;
    QDBusMessage set = QDBusMessage::createMethodCall(m_service, "/org/mpris/MediaPlayer2",
                                                      "org.freedesktop.DBus.Properties", "Set");
    set << QString("org.mpris.MediaPlayer2.Player") << QString("Volume")
        << QVariant::fromValue(QDBusVariant(qBound(0.0, volume, 1.0)));
    m_bus.asyncCall(set);
}
