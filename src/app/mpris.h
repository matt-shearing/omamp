#pragma once

#include <functional>

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QVariantMap>

class NowPlaying;
class Player;

// MPRIS lets the media keys, playerctl and the Omarchy bar see and drive the
// player like any other.
class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ yes)
    Q_PROPERTY(bool CanRaise READ yes)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ schemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimeTypes)

public:
    explicit MprisRoot(QObject *parent);
    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return "Thrum"; }
    QString desktopEntry() const { return "thrum"; }
    QStringList schemes() const { return {"file"}; }
    QStringList mimeTypes() const;

public slots:
    void Raise();
    void Quit();

signals:
    void raiseRequested();
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(bool CanGoNext READ canPlay)
    Q_PROPERTY(bool CanGoPrevious READ canPlay)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPlay)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ yes)

public:
    // art, when given, puts the cover in the metadata (mpris:artUrl).
    MprisPlayer(QObject *parent, Player *player, NowPlaying *art = nullptr);
    // PropertiesChanged goes out only once the object is on the bus, so a
    // test can read the metadata without touching the session bus.
    void setAnnouncing(bool on) { m_announcing = on; }

    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &status);
    bool shuffle() const;
    void setShuffle(bool on);
    double rate() const { return 1.0; }
    void setRate(double) {}
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const;
    bool canPlay() const;
    // Not on a live radio station.
    bool canSeek() const;
    bool yes() const { return true; }
    // OpenUri hands addresses that aren't local files (radio://…, http)
    // to this; it returns whether it took them.
    void setUriHandler(std::function<bool(const QString &)> handler) { m_uriHandler = std::move(handler); }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetUs);
    void SetPosition(const QDBusObjectPath &track, qlonglong positionUs);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong positionUs);

private:
    void announce(const QStringList &names);
    QDBusObjectPath trackId() const;

    Player *m_player;
    NowPlaying *m_art = nullptr;
    bool m_announcing = false;
    std::function<bool(const QString &)> m_uriHandler;
};

// Registers both interfaces on the session bus. Returns false when another
// copy already owns the name, which is reported but not fatal.
bool registerMpris(QObject *host, Player *player, NowPlaying *art = nullptr);

// Gives the registered player's OpenUri a handler for radio:// and http(s)
// addresses (the radio's), so `thrum radio play` and `playerctl open` work.
void setMprisUriHandler(QObject *host, std::function<bool(const QString &)> handler);
