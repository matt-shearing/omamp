#include "handoffengine.h"

#include "spotifyaudio.h"

HandoffEngine::HandoffEngine(AudioEngine *inner, ExternalPlayback *external, QObject *parent)
    : AudioEngine(parent), m_inner(inner) {
    m_inner->setParent(this);
    m_settings = m_inner->settings();

    // The real engine's reports pass through while it plays the current
    // track. The analyser's audio and the format always come from it.
    connect(m_inner, &AudioEngine::pcmTapped, this, &AudioEngine::pcmTapped);
    connect(m_inner, &AudioEngine::formatChanged, this, &AudioEngine::formatChanged);
    connect(m_inner, &AudioEngine::positionChanged, this, [this](qint64 ms) {
        if (!m_external)
            emit positionChanged(ms);
    });
    connect(m_inner, &AudioEngine::durationChanged, this, [this](qint64 ms) {
        if (!m_external)
            emit durationChanged(ms);
    });
    connect(m_inner, &AudioEngine::stateChanged, this, [this] {
        if (!m_external)
            emit stateChanged();
    });
    connect(m_inner, &AudioEngine::bufferingChanged, this, [this] {
        if (!m_external)
            emit bufferingChanged();
    });
    connect(m_inner, &AudioEngine::metaDataChanged, this, [this] {
        if (!m_external)
            emit metaDataChanged();
    });
    connect(m_inner, &AudioEngine::advanced, this, [this] {
        if (!m_external)
            emit advanced();
    });
    connect(m_inner, &AudioEngine::endOfQueue, this, [this] {
        if (m_external)
            return;
        // The real engine was given nothing more because Spotify plays
        // next: this is the handover.
        if (isExternal(m_queuedPath))
            startExternal(m_queuedPath);
        else
            emit endOfQueue();
    });
    connect(m_inner, &AudioEngine::trackFailed, this, [this](const QString &path, const QString &detail) {
        if (!m_external)
            emit trackFailed(path, detail);
    });
    connect(m_inner, &AudioEngine::errorOccurred, this, &AudioEngine::errorOccurred);
    setExternal(external);
}

void HandoffEngine::setExternal(ExternalPlayback *external) {
    for (auto &c : m_extConnections)
        disconnect(c);
    m_ext = external;
    if (m_ext) {
        connectExternal();
        m_ext->setSettings(m_settings);
        pushVolume();
    }
}

void HandoffEngine::connectExternal() {
    int i = 0;
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::positionChanged, this, [this](qint64 ms) {
        if (m_external)
            emit positionChanged(ms);
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::durationChanged, this, [this](qint64 ms) {
        if (m_external)
            emit durationChanged(ms);
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::stateChanged, this, [this] {
        if (m_external)
            emit stateChanged();
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::bufferingChanged, this, [this] {
        if (m_external)
            emit bufferingChanged();
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::metaDataChanged, this, [this] {
        if (m_external)
            emit metaDataChanged();
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::finished, this, [this] {
        if (m_external)
            onExternalFinished();
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::pcmTapped, this, [this](const QAudioBuffer &buffer) {
        if (m_external)
            emit pcmTapped(buffer);
    });
    m_extConnections[i++] = connect(m_ext, &ExternalPlayback::failed, this,
                                    [this](const QString &path, const QString &detail) {
                                        if (m_external)
                                            emit trackFailed(path, detail);
                                    });
}

void HandoffEngine::applyQueue() {
    if (m_external)
        return;
    if (isExternal(m_queuedPath))
        m_inner->queueNext({});
    else
        m_inner->queueNext(m_queuedPath, m_queuedUrl);
}

void HandoffEngine::load(const QString &path, const QUrl &url) {
    if (isExternal(path)) {
        if (!m_external) {
            // Ignore what the real engine says while it lets go.
            m_external = true;
            m_inner->stop();
            m_inner->load({});
        }
        m_ext->load(path);
        emit stateChanged();
        emit durationChanged(m_ext->duration());
        return;
    }
    if (m_external) {
        m_external = false;
        if (m_ext)
            m_ext->unload();
    }
    m_inner->load(path, url);
    applyQueue();
    emit stateChanged();
}

void HandoffEngine::queueNext(const QString &path, const QUrl &url) {
    m_queuedPath = path;
    m_queuedUrl = url;
    applyQueue();
}

void HandoffEngine::skipTo(const QString &path, const QUrl &url) {
    if (m_external || isExternal(path)) {
        load(path, url);
        play();
        return;
    }
    // Local to local keeps the real engine's crossfade.
    m_inner->skipTo(path, url);
    applyQueue();
}

void HandoffEngine::startExternal(const QString &path) {
    m_external = true;
    m_inner->load({});
    m_ext->load(path);
    pushVolume();
    // The Player takes the queued row as current and queues the next.
    emit advanced();
    m_ext->play();
    emit stateChanged();
    emit durationChanged(m_ext->duration());
}

void HandoffEngine::startInner(const QString &path, const QUrl &url) {
    m_external = false;
    if (m_ext)
        m_ext->unload();
    // The Player takes the queued row first, so the new file's tags
    // arrive after it has cleared the old ones; then the file loads and
    // gets what the Player queued after it.
    emit advanced();
    m_inner->load(path, url);
    applyQueue();
    m_inner->play();
    emit stateChanged();
}

void HandoffEngine::onExternalFinished() {
    const QString next = m_queuedPath;
    if (next.isEmpty()) {
        emit endOfQueue();
        return;
    }
    if (isExternal(next)) {
        m_ext->load(next);
        emit advanced();
        m_ext->play();
        emit stateChanged();
        emit durationChanged(m_ext->duration());
        return;
    }
    startInner(next, m_queuedUrl);
}

QString HandoffEngine::source() const { return m_external ? m_ext->source() : m_inner->source(); }

void HandoffEngine::play() {
    if (m_external)
        m_ext->play();
    else
        m_inner->play();
}

void HandoffEngine::pause() {
    if (m_external)
        m_ext->pause();
    else
        m_inner->pause();
}

void HandoffEngine::stop() {
    if (m_external)
        m_ext->stop();
    else
        m_inner->stop();
}

void HandoffEngine::seek(qint64 ms) {
    if (m_external)
        m_ext->seek(ms);
    else
        m_inner->seek(ms);
}

qint64 HandoffEngine::position() const { return m_external ? m_ext->position() : m_inner->position(); }
qint64 HandoffEngine::duration() const { return m_external ? m_ext->duration() : m_inner->duration(); }
AudioEngine::State HandoffEngine::state() const { return m_external ? m_ext->state() : m_inner->state(); }
bool HandoffEngine::buffering() const { return m_external ? m_ext->buffering() : m_inner->buffering(); }
QVariantMap HandoffEngine::metaData() const { return m_external ? m_ext->metaData() : m_inner->metaData(); }

void HandoffEngine::setSettings(const Settings &settings) {
    AudioEngine::setSettings(settings);
    m_inner->setSettings(settings);
    if (m_ext)
        m_ext->setSettings(settings);
}

bool HandoffEngine::canPlay(const QString &path) const {
    return m_inner->canPlay(path) || (m_ext && m_ext->handles(path) && m_ext->playsInThrum());
}

// --- EnginePlayback ---------------------------------------------------------------

EnginePlayback::EnginePlayback(Factory make, QObject *parent) : ExternalPlayback(parent), m_make(std::move(make)) {}

bool EnginePlayback::handles(const QString &path) const { return SpotifyAudio::canPlay(path); }

AudioEngine *EnginePlayback::ensure() {
    if (m_engine)
        return m_engine;
    m_engine = m_make();
    m_engine->setParent(this);
    m_engine->setVolume(m_volume);
    if (m_haveSettings)
        m_engine->setSettings(m_settings);
    connect(m_engine, &AudioEngine::positionChanged, this, &ExternalPlayback::positionChanged);
    connect(m_engine, &AudioEngine::durationChanged, this, &ExternalPlayback::durationChanged);
    connect(m_engine, &AudioEngine::stateChanged, this, &ExternalPlayback::stateChanged);
    connect(m_engine, &AudioEngine::bufferingChanged, this, &ExternalPlayback::bufferingChanged);
    connect(m_engine, &AudioEngine::metaDataChanged, this, &ExternalPlayback::metaDataChanged);
    connect(m_engine, &AudioEngine::pcmTapped, this, &ExternalPlayback::pcmTapped);
    // Nothing is queued in it: its track ending is the end of ours.
    connect(m_engine, &AudioEngine::endOfQueue, this, &ExternalPlayback::finished);
    connect(m_engine, &AudioEngine::trackFailed, this, &ExternalPlayback::failed);
    return m_engine;
}

void EnginePlayback::load(const QString &path) { ensure()->load(path); }

void EnginePlayback::unload() {
    if (m_engine) {
        m_engine->stop();
        m_engine->load({});
    }
}

void EnginePlayback::play() { ensure()->play(); }

void EnginePlayback::pause() {
    if (m_engine)
        m_engine->pause();
}

void EnginePlayback::stop() {
    if (m_engine)
        m_engine->stop();
}

void EnginePlayback::seek(qint64 ms) {
    if (m_engine)
        m_engine->seek(ms);
}

void EnginePlayback::setVolume(qreal linear) {
    m_volume = linear;
    if (m_engine)
        m_engine->setVolume(linear);
}

void EnginePlayback::setSettings(const AudioEngine::Settings &settings) {
    m_settings = settings;
    m_haveSettings = true;
    if (m_engine)
        m_engine->setSettings(settings);
}

void HandoffEngine::setVolume(qreal linear) {
    m_volume = linear;
    m_inner->setVolume(linear);
    pushVolume();
}

void HandoffEngine::setMuted(bool muted) {
    m_muted = muted;
    m_inner->setMuted(muted);
    pushVolume();
}

// The outside player only hears the volume while it plays Thrum's entry, so
// a Spotify volume set elsewhere is left alone the rest of the time. A
// playback in Thrum always has it, so it never starts at full volume.
void HandoffEngine::pushVolume() {
    if (m_ext && (m_external || m_ext->playsInThrum()))
        m_ext->setVolume(m_muted ? 0.0 : m_volume);
}
