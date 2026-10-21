#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include "spotify.h"

class QDBusServiceWatcher;

// Follows a Spotify backend through its MPRIS service on the session bus,
// so Thrum knows what plays, whether it is playing and where it is without
// asking Spotify's Web API at all. spotify_player publishes
// org.mpris.MediaPlayer2.spotify_player when enable_media_control is on
// (the default on Linux) and Spotifast publishes
// org.mpris.MediaPlayer2.spotifast.
//
// Only properties are read and only signals are listened to: no method is
// called on the player. spotify_player refreshes its MPRIS state once a
// second from what it already knows, and its metadata carries no Spotify
// id ("mpris:trackid" is "/"), so a track is matched by title and length
// there. Spotifast's metadata has the id in "xesam:url".
namespace Spotify {

class MprisWatch : public QObject {
    Q_OBJECT
public:
    explicit MprisWatch(QObject *parent = nullptr);
    ~MprisWatch() override;

    // The bus name to follow; empty stops following. THRUM_SPOTIFY_MPRIS_NAME
    // replaces it (tests), and THRUM_SPOTIFY_MPRIS_BUS names a private bus
    // to use instead of the session bus (tests).
    void setService(const QString &busName);
    // Sets the player's MPRIS Volume (0..1). A local D-Bus call: it costs
    // Spotify no Web API request.
    void setVolume(qreal volume);
    QString service() const { return m_service; }
    bool available() const { return m_available; }

    // The player's state as a Playback: the track's name, artists, album,
    // length and picture, its id when the player says it, and the position
    // moved on by the time since it was last read.
    Playback state() const;
    QString status() const { return m_status; }
    // Reads the position again (a property read on the bus, not Spotify).
    void refreshPosition();
    // Reads every property again.
    void refreshAll();

    static QDBusConnection connection();

signals:
    void availableChanged();
    // The status, the track or the position changed.
    void changed();

private slots:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);
    void onSeeked(const QDBusMessage &message);

private:
    void watch();
    void unwatch();
    void setAvailable(bool on);
    void apply(const QVariantMap &properties, bool announce = true);
    void setPosition(qint64 us);

    QDBusConnection m_bus;
    QString m_service;
    QPointer<QDBusServiceWatcher> m_watcher;
    bool m_available = false;
    bool m_connected = false;
    QString m_status = "Stopped";
    QVariantMap m_metadata;
    qint64 m_positionUs = 0;
    QElapsedTimer m_positionAt;
    int m_generation = 0;
};

// The track in an MPRIS metadata map (tests use it directly).
Item itemFromMpris(const QVariantMap &metadata);

} // namespace Spotify
