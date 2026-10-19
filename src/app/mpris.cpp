#include "mpris.h"

#include "nowplaying.h"
#include "player.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QUrl>

MprisRoot::MprisRoot(QObject *parent) : QDBusAbstractAdaptor(parent) {}

QStringList MprisRoot::mimeTypes() const {
    return {"audio/mpeg", "audio/flac", "audio/ogg", "audio/opus", "audio/mp4", "audio/aac", "audio/x-wav"};
}

void MprisRoot::Raise() { emit raiseRequested(); }
void MprisRoot::Quit() { QGuiApplication::quit(); }

MprisPlayer::MprisPlayer(QObject *parent, Player *player, NowPlaying *art)
    : QDBusAbstractAdaptor(parent), m_player(player), m_art(art) {
    connect(player, &Player::stateChanged, this, [this] { announce({"PlaybackStatus"}); });
    connect(player, &Player::trackChanged, this, [this] { announce({"Metadata", "CanPlay", "CanGoNext"}); });
    connect(player, &Player::volumeChanged, this, [this] { announce({"Volume"}); });
    connect(player, &Player::modesChanged, this, [this] { announce({"Shuffle", "LoopStatus"}); });
    connect(player, &Player::seeked, this, [this](qint64 ms) { emit Seeked(ms * 1000); });
    if (m_art) {
        connect(m_art, &NowPlaying::artChanged, this, [this] { announce({"Metadata"}); });
        connect(m_art, &NowPlaying::loadingChanged, this, [this] { announce({"Metadata"}); });
    }
}

// Clients only learn about changes from PropertiesChanged, which Qt's
// adaptors don't send on their own.
void MprisPlayer::announce(const QStringList &names) {
    if (!m_announcing)
        return;
    QVariantMap changed;
    for (const auto &name : names)
        changed.insert(name, property(name.toUtf8().constData()));
    QDBusMessage signal = QDBusMessage::createSignal("/org/mpris/MediaPlayer2",
                                                     "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal << QString("org.mpris.MediaPlayer2.Player") << changed << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

QString MprisPlayer::playbackStatus() const {
    const QString s = m_player->state();
    return s == "playing" ? "Playing" : s == "paused" ? "Paused" : "Stopped";
}

QString MprisPlayer::loopStatus() const { return m_player->repeat() ? "Playlist" : "None"; }
void MprisPlayer::setLoopStatus(const QString &status) { m_player->setRepeat(status != "None"); }
bool MprisPlayer::shuffle() const { return m_player->shuffle(); }
void MprisPlayer::setShuffle(bool on) { m_player->setShuffle(on); }
double MprisPlayer::volume() const { return m_player->volume() / 100.0; }
void MprisPlayer::setVolume(double volume) { m_player->setVolume(qRound(volume * 100)); }
qlonglong MprisPlayer::position() const { return m_player->position() * 1000; }
bool MprisPlayer::canPlay() const { return m_player->playlist()->count() > 0; }

QDBusObjectPath MprisPlayer::trackId() const {
    return QDBusObjectPath(QString("/org/omamp/track/%1").arg(qMax(0, m_player->currentIndex())));
}

QVariantMap MprisPlayer::metadata() const {
    const int row = m_player->currentIndex();
    if (row < 0)
        return {{"mpris:trackid", QVariant::fromValue(QDBusObjectPath("/org/mpris/MediaPlayer2/TrackList/NoTrack"))}};
    QVariantMap map{{"mpris:trackid", QVariant::fromValue(trackId())},
                    {"mpris:length", qlonglong(m_player->duration() * 1000)},
                    {"xesam:title", m_player->title()},
                    {"xesam:url", QUrl::fromLocalFile(m_player->playlist()->at(row).path).toString()}};
    if (!m_player->artist().isEmpty())
        map.insert("xesam:artist", QStringList{m_player->artist()});
    if (!m_player->album().isEmpty())
        map.insert("xesam:album", m_player->album());
    // The cover at 600 px, as a file:// URL, once it is known to be this
    // track's (while the next one is looked for, the last one's would be
    // wrong).
    if (m_art && !m_art->loading() && m_art->hasArt())
        map.insert("mpris:artUrl", m_art->artUrl());
    return map;
}

void MprisPlayer::Next() { m_player->next(); }
void MprisPlayer::Previous() { m_player->previous(); }
void MprisPlayer::Pause() { m_player->pause(); }
void MprisPlayer::PlayPause() { m_player->toggle(); }
void MprisPlayer::Stop() { m_player->stop(); }
void MprisPlayer::Play() { m_player->play(); }
void MprisPlayer::Seek(qlonglong offsetUs) { m_player->seekBy(offsetUs / 1000); }

void MprisPlayer::SetPosition(const QDBusObjectPath &track, qlonglong positionUs) {
    if (track == trackId())
        m_player->seek(positionUs / 1000);
}

void MprisPlayer::OpenUri(const QString &uri) {
    const int before = m_player->playlist()->count();
    if (m_player->addPaths({QUrl(uri).toLocalFile()}) > 0)
        m_player->playIndex(before);
}

bool registerMpris(QObject *host, Player *player, NowPlaying *art) {
    new MprisRoot(host);
    auto *adaptor = new MprisPlayer(host, player, art);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject("/org/mpris/MediaPlayer2", host))
        return false;
    adaptor->setAnnouncing(true);
    // A second copy gets its own name, as MPRIS allows, so both stay controllable.
    if (bus.registerService("org.mpris.MediaPlayer2.omamp"))
        return true;
    return bus.registerService(QString("org.mpris.MediaPlayer2.omamp.instance%1").arg(QCoreApplication::applicationPid()));
}
