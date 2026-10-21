#pragma once

#include <QPointer>

#include "audioengine.h"

// Playback that happens outside Thrum: another program plays the track and
// Thrum follows it (Spotify, through spotify_player's Connect device).
// HandoffEngine hands queue entries it handles() to it and everything else
// to the real engine. Implementations report as an engine would; they never
// make sound in Thrum.
class ExternalPlayback : public QObject {
    Q_OBJECT
public:
    explicit ExternalPlayback(QObject *parent = nullptr) : QObject(parent) {}

    // Whether this queue entry plays elsewhere (spotify://track/<id>).
    virtual bool handles(const QString &path) const = 0;
    // Makes path the current track, stopped. Nothing plays yet.
    virtual void load(const QString &path) = 0;
    // Thrum moved on to a track of its own: stop playing ours, if playing.
    virtual void unload() = 0;
    virtual QString source() const = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(qint64 ms) = 0;

    virtual qint64 position() const = 0;
    virtual qint64 duration() const = 0;
    virtual AudioEngine::State state() const = 0;
    virtual bool buffering() const { return false; }
    // Thrum's volume, 0..1 linear as AudioEngine takes it, for the outside
    // player's own volume. Players that can't take it ignore it.
    virtual void setVolume(qreal linear) { Q_UNUSED(linear); }
    // title, artist, album, as AudioEngine::metaData().
    virtual QVariantMap metaData() const = 0;

signals:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void stateChanged();
    void bufferingChanged();
    void metaDataChanged();
    // The track played to its end.
    void finished();
    // The track could not be played; detail says why, for the person.
    void failed(const QString &path, const QString &detail);
};

// An AudioEngine that puts an ExternalPlayback beside the real engine, so
// the Player's one queue can mix local tracks and Spotify tracks.
//
//   local → Spotify: the real engine is given nothing to queue, so it ends
//     with endOfQueue; that is the moment Spotify starts.
//   Spotify → local: when Spotify reports the track finished, the real
//     engine loads and plays the next file.
//
// Gapless joins, crossfades, ReplayGain, the equaliser and the analyser
// stay inside the real engine: they don't cross the boundary. The volume
// slider and mute act on Thrum's own sound only.
class HandoffEngine : public AudioEngine {
    Q_OBJECT
public:
    // Takes ownership of inner. external may be null (everything plays
    // inside), and isn't owned.
    explicit HandoffEngine(AudioEngine *inner, ExternalPlayback *external = nullptr, QObject *parent = nullptr);

    void setExternal(ExternalPlayback *external);
    AudioEngine *inner() const { return m_inner; }
    // Whether the current track plays outside.
    bool externalActive() const { return m_external; }

    QString name() const override { return m_inner->name(); }
    bool gapless() const override { return m_inner->gapless(); }
    bool skipsUnplayable() const override { return m_inner->skipsUnplayable(); }

    void load(const QString &path, const QUrl &url = {}) override;
    void queueNext(const QString &path, const QUrl &url = {}) override;
    void skipTo(const QString &path, const QUrl &url = {}) override;
    QString source() const override;

    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 ms) override;

    qint64 position() const override;
    qint64 duration() const override;
    State state() const override;
    bool buffering() const override;

    // The volume reaches both sides, so the slider works whichever plays.
    void setVolume(qreal linear) override;
    void setMuted(bool muted) override;
    QVariantMap metaData() const override;
    void setSettings(const Settings &settings) override;

private:
    bool isExternal(const QString &path) const { return m_ext && m_ext->handles(path); }
    // Gives the real engine what follows, or nothing when Spotify does.
    void applyQueue();
    void connectExternal();
    void startExternal(const QString &path);
    void startInner(const QString &path, const QUrl &url);
    void onExternalFinished();

    AudioEngine *m_inner;
    QPointer<ExternalPlayback> m_ext;
    QMetaObject::Connection m_extConnections[8];
    bool m_external = false;
    qreal m_volume = 1.0;
    bool m_muted = false;
    void pushVolume();
    QString m_queuedPath;
    QUrl m_queuedUrl;
};
