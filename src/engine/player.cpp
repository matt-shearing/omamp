#include "player.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>
#include <cmath>

#include "equalizer.h"
#include "nativeengine.h"
#include "qtengine.h"
#include "streaming.h"

namespace {
// The settings file beside the state file: ~/.config/omamp/omamp.conf.
QString settingsPathFor(const QString &statePath) {
    return statePath.isEmpty() ? QString() : QFileInfo(statePath).absolutePath() + "/omamp.conf";
}

AudioEngine *chooseEngine(const QString &settingsPath) {
    QString choice = qEnvironmentVariable("OMAMP_ENGINE").trimmed().toLower();
    if (choice.isEmpty() && !settingsPath.isEmpty())
        choice = QSettings(settingsPath, QSettings::IniFormat).value("playback/engine").toString().trimmed().toLower();
    if (choice == "native")
        return new NativeEngine;
    return new QtEngine;
}
} // namespace

AudioEngine *Player::createEngine(const QString &statePath) { return chooseEngine(settingsPathFor(statePath)); }

Player::Player(const QString &statePath, QObject *parent)
    : Player(chooseEngine(settingsPathFor(statePath)), statePath, Resolver(), parent) {}

Player::Player(const QString &statePath, Resolver resolver, QObject *parent)
    : Player(chooseEngine(settingsPathFor(statePath)), statePath, std::move(resolver), parent) {}

Player::Player(AudioEngine *engine, const QString &statePath, QObject *parent)
    : Player(engine, statePath, Resolver(), parent) {}

Player::Player(AudioEngine *engine, const QString &statePath, Resolver resolver, QObject *parent)
    : QObject(parent), m_engine(engine), m_resolver(std::move(resolver)), m_statePath(statePath),
      m_settingsPath(settingsPathFor(statePath)) {
    m_engine->setParent(this);
    connectEngine();

    // The bars fall back between buffers and settle to zero when stopped.
    m_spectrumTimer.setInterval(33);
    connect(&m_spectrumTimer, &QTimer::timeout, this, [this] {
        m_spectrum.decay();
        emit levelsChanged();
    });
    m_spectrumTimer.start();

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &Player::save);
    connect(&m_playlist, &Playlist::countChanged, this, &Player::saveSoon);
    // Whatever changes the list may change what comes next.
    connect(&m_playlist, &Playlist::countChanged, this, &Player::requeue);

    m_engine->setVolume(QAudio::convertVolume(m_volume / 100.0, QAudio::LogarithmicVolumeScale,
                                              QAudio::LinearVolumeScale));
    restoreSettings();
    restore();
}

void Player::connectEngine() {
    connect(m_engine, &AudioEngine::pcmTapped, this, [this](const QAudioBuffer &buffer) { m_spectrum.feed(buffer); });
    connect(m_engine, &AudioEngine::formatChanged, this, [this](int rate, int channels) {
        if (rate != m_sampleRate || channels != m_channels) {
            m_sampleRate = rate;
            m_channels = channels;
            emit formatChanged();
        }
    });
    connect(m_engine, &AudioEngine::positionChanged, this, [this](qint64 ms) {
        // Playing again means the run of broken files is over.
        if (ms > 0)
            m_badRun = 0;
        countHeard(ms);
        emit positionChanged();
    });
    connect(m_engine, &AudioEngine::durationChanged, this, [this] {
        if (m_current >= 0)
            m_playlist.updateTags(m_current, {}, {}, m_engine->duration());
        emit trackChanged();
    });
    connect(m_engine, &AudioEngine::metaDataChanged, this, &Player::readMetaData);
    connect(m_engine, &AudioEngine::stateChanged, this, [this] {
        if (m_engine->state() == AudioEngine::Playing)
            beginPlay();
        emit stateChanged();
    });
    connect(m_engine, &AudioEngine::advanced, this, [this] {
        // The engine moved on to the queued row by itself.
        if (m_queued < 0 || m_queued >= m_playlist.count())
            return;
        // The track played to its end: what was left after the last
        // position report was heard too.
        if (!m_playPath.isEmpty() && m_current >= 0) {
            const qint64 length = m_playlist.at(m_current).durationMs;
            if (length > m_lastPos)
                m_heardMs += length - m_lastPos;
        }
        finishPlay(false);
        m_history << m_current;
        m_current = m_queued;
        m_title.clear();
        m_artist.clear();
        m_album.clear();
        m_bitrate = 0;
        m_spectrum.reset();
        emit trackChanged();
        saveSoon();
        requeue();
        if (m_engine->state() == AudioEngine::Playing)
            beginPlay();
    });
    connect(m_engine, &AudioEngine::endOfQueue, this, [this] {
        if (!m_playPath.isEmpty()) {
            const qint64 length = duration();
            if (length > m_lastPos)
                m_heardMs += length - m_lastPos;
        }
        finishPlay(false);
        stop();
    });
    connect(m_engine, &AudioEngine::bufferingChanged, this, &Player::bufferingChanged);
    connect(m_engine, &AudioEngine::trackFailed, this, [this](const QString &path, const QString &detail) {
        // A stream says why (the server refused, the connection broke);
        // a local file's FFmpeg error says nothing the name doesn't.
        QString why = m_resolveErrors.value(path);
        if (why.isEmpty() && Playlist::isReference(path))
            why = detail;
        why = AudioEngine::redact(why);
        if (Streaming::isLive(path)) {
            // The radio decides: another address for the station, or a
            // message. A live station never makes the list move on.
            emit liveStreamFailed(path, why);
            return;
        }
        setError(why.isEmpty() ? tr("Can't play %1").arg(displayName(path))
                               : tr("Can't play %1: %2").arg(displayName(path), why));
        if (!m_engine->skipsUnplayable() || !m_wantPlaying)
            return;
        // Skip to the next playable file, but give up after trying them all.
        if (++m_badRun < m_playlist.count()) {
            const int after = nextIndex();
            if (after >= 0 && after != m_current) {
                m_history << m_current;
                load(after);
                m_engine->play();
                return;
            }
        }
        m_badRun = 0;
        m_wantPlaying = false;
    });
    connect(m_engine, &AudioEngine::errorOccurred, this, &Player::setError);
}

Player::~Player() {
    if (m_saveTimer.isActive())
        save();
}

void Player::addPicked(const QStringList &paths) {
    const bool wasEmpty = m_playlist.count() == 0;
    if (addPaths(paths) == 0)
        setError(tr("No music found there"));
    else if (wasEmpty)
        playIndex(0);
}

QString Player::state() const {
    switch (m_engine->state()) {
    case AudioEngine::Playing: return "playing";
    case AudioEngine::Paused: return "paused";
    default: return "stopped";
    }
}

qint64 Player::duration() const {
    if (m_engine->duration() > 0)
        return m_engine->duration();
    return m_current >= 0 ? m_playlist.at(m_current).durationMs : 0;
}

QString Player::title() const {
    if (!m_title.isEmpty())
        return m_title;
    return m_current >= 0 ? m_playlist.at(m_current).title : QString();
}

QString Player::artist() const {
    if (!m_artist.isEmpty())
        return m_artist;
    return m_current >= 0 ? m_playlist.at(m_current).artist : QString();
}

void Player::setVolume(int volume) {
    volume = qBound(0, volume, 100);
    if (volume == m_volume)
        return;
    m_volume = volume;
    // The slider moves in steps that sound even, not in raw amplitude.
    m_engine->setVolume(QAudio::convertVolume(volume / 100.0, QAudio::LogarithmicVolumeScale,
                                              QAudio::LinearVolumeScale));
    emit volumeChanged();
    saveSoon();
}

void Player::setShuffle(bool on) {
    if (on == m_shuffle)
        return;
    m_shuffle = on;
    requeue();
    emit modesChanged();
    saveSoon();
}

void Player::setRepeat(bool on) {
    if (on == m_repeat)
        return;
    m_repeat = on;
    requeue();
    emit modesChanged();
    saveSoon();
}

void Player::load(int row, bool skip) {
    if (row < 0 || row >= m_playlist.count())
        return;
    finishPlay(m_skipping);
    m_current = row;
    m_title.clear();
    m_artist.clear();
    m_album.clear();
    m_bitrate = 0;
    m_spectrum.reset();
    const QString path = m_playlist.at(row).path;
    const QUrl url = urlFor(row);
    if (skip && m_engine->state() == AudioEngine::Playing) {
        // The engine may fade from the old track into this one without
        // stopping, so the new play is counted from here.
        m_engine->skipTo(path, url);
        requeue();
        emit trackChanged();
        saveSoon();
        if (m_engine->state() == AudioEngine::Playing)
            beginPlay();
        return;
    }
    m_engine->load(path, url);
    requeue();
    emit trackChanged();
    saveSoon();
}

void Player::setResolver(Resolver resolver) {
    m_resolver = std::move(resolver);
    m_queuedPath.clear();
    m_queuedUrl.clear();
    m_resolveErrors.clear();
    // A stream loaded before there was a resolver is loaded again with it.
    if (m_current >= 0 && Playlist::isReference(m_playlist.at(m_current).path) &&
        m_engine->state() != AudioEngine::Playing)
        load(m_current);
    else
        requeue();
}

QUrl Player::urlFor(int row) {
    if (row < 0 || row >= m_playlist.count())
        return {};
    const Playlist::Track &t = m_playlist.at(row);
    if (!Playlist::isReference(t.path))
        return {};
    m_resolveErrors.remove(t.path);
    QString error;
    QUrl url;
    if (m_resolver)
        url = m_resolver(t.path, t.trackId, &error);
    else if (AudioEngine::isNetworkUrl(QUrl(t.path)))
        url = QUrl(t.path);
    else
        error = tr("no library to play it from");
    if (!url.isValid() || url.isEmpty()) {
        // The engine then fails on the bare reference, and the message
        // gives this reason.
        m_resolveErrors.insert(t.path, AudioEngine::redact(error.isEmpty() ? tr("not available") : error));
        return {};
    }
    return url;
}

QString Player::displayName(const QString &path) const {
    // A local file by its name, as always; a stream by its title, since
    // its path is only a server's song id.
    if (!Playlist::isReference(path))
        return QFileInfo(path).fileName();
    for (int i = 0; i < m_playlist.count(); ++i) {
        const Playlist::Track &t = m_playlist.at(i);
        if (t.path == path && !t.title.isEmpty())
            return t.artist.isEmpty() ? t.title : t.artist + " - " + t.title;
    }
    return AudioEngine::redact(path);
}

// Tells the engine which file follows the current one, so it can open it
// early and join the two without a gap.
void Player::requeue() {
    m_queued = m_current >= 0 ? nextIndex() : -1;
    const QString path = m_queued >= 0 ? m_playlist.at(m_queued).path : QString();
    // Requeueing the same track (the list changed around it) reuses the
    // URL rather than asking the resolver again.
    if (path != m_queuedPath || path.isEmpty()) {
        m_queuedPath = path;
        m_queuedUrl = m_queued >= 0 ? urlFor(m_queued) : QUrl();
    }
    m_engine->queueNext(path, m_queuedUrl);
}

void Player::readMetaData() {
    const QVariantMap meta = m_engine->metaData();
    if (live()) {
        // The radio sets a station's title and artist (setLiveInfo); the
        // queue row keeps the station's name.
        const int bitrate = meta.value("bitrate").toInt();
        if (bitrate != m_bitrate) {
            m_bitrate = bitrate;
            emit trackChanged();
        }
        return;
    }
    m_title = meta.value("title").toString();
    m_artist = meta.value("artist").toString();
    if (m_artist.isEmpty())
        m_artist = meta.value("albumArtist").toString();
    m_album = meta.value("album").toString();
    m_bitrate = meta.value("bitrate").toInt();
    if (m_current >= 0)
        m_playlist.updateTags(m_current, m_title, m_artist, m_engine->duration());
    emit trackChanged();
}

void Player::play() {
    if (m_playlist.count() == 0)
        return;
    if (m_current < 0 || m_engine->source().isEmpty())
        load(qMax(0, m_current));
    m_wantPlaying = true;
    m_engine->play();
}

void Player::pause() {
    m_wantPlaying = false;
    m_engine->pause();
}

void Player::toggle() {
    if (m_engine->state() == AudioEngine::Playing)
        pause();
    else
        play();
}

void Player::stop() {
    finishPlay(false);
    m_wantPlaying = false;
    m_engine->stop();
    m_spectrum.reset();
    emit levelsChanged();
}

int Player::nextIndex() {
    const int count = m_playlist.count();
    if (count == 0)
        return -1;
    if (m_shuffle) {
        if (count == 1)
            return m_repeat ? 0 : -1;
        int pick = m_current;
        while (pick == m_current)
            pick = QRandomGenerator::global()->bounded(count);
        return pick;
    }
    if (m_current + 1 < count)
        return m_current + 1;
    return m_repeat ? 0 : -1;
}

void Player::next() {
    int after = nextIndex();
    // Pressing next on the last track wraps, like Winamp, even without repeat.
    if (after < 0)
        after = m_playlist.count() > 0 ? 0 : -1;
    if (after < 0)
        return;
    const bool playing = m_engine->state() == AudioEngine::Playing;
    m_history << m_current;
    m_skipping = true;
    load(after, playing);
    m_skipping = false;
    if (playing)
        m_engine->play();
}

void Player::previous() {
    if (m_playlist.count() == 0)
        return;
    // Like most players: a few seconds in, previous restarts the track.
    if (m_engine->position() > 3000) {
        seek(0);
        return;
    }
    int before;
    if (m_shuffle && !m_history.isEmpty())
        before = m_history.takeLast();
    else
        before = m_current > 0 ? m_current - 1 : m_playlist.count() - 1;
    if (before < 0 || before >= m_playlist.count())
        before = 0;
    const bool playing = m_engine->state() == AudioEngine::Playing;
    m_skipping = true;
    load(before, playing);
    m_skipping = false;
    if (playing)
        m_engine->play();
}

void Player::playIndex(int row) {
    if (row < 0 || row >= m_playlist.count())
        return;
    if (m_current >= 0)
        m_history << m_current;
    m_skipping = true;
    load(row, m_engine->state() == AudioEngine::Playing);
    m_skipping = false;
    m_wantPlaying = true;
    m_engine->play();
}

bool Player::live() const {
    return m_current >= 0 && m_current < m_playlist.count() && Streaming::isLive(m_playlist.at(m_current).path);
}

bool Player::playsElsewhere() const {
    return m_current >= 0 && m_current < m_playlist.count() && m_playlist.at(m_current).path.startsWith("spotify:");
}

void Player::setLiveInfo(const QString &title, const QString &artist, const QString &album) {
    if (!live() || (title == m_title && artist == m_artist && album == m_album))
        return;
    m_title = title;
    m_artist = artist;
    m_album = album;
    emit trackChanged();
}

void Player::seek(qint64 ms) {
    // A live station has nowhere to go.
    if (live())
        return;
    const qint64 target = qBound<qint64>(0, ms, qMax<qint64>(0, duration()));
    // A seek is not listening: count from where it lands.
    m_lastPos = target;
    m_engine->seek(target);
    emit seeked(m_engine->position());
}

void Player::seekBy(qint64 ms) { seek(m_engine->position() + ms); }

int Player::addPaths(const QStringList &paths) {
    const int added = m_playlist.addPaths(paths);
    if (added > 0 && m_current < 0)
        load(0);
    return added;
}

int Player::playTracks(const QList<Playlist::Track> &tracks, int start) {
    if (tracks.isEmpty())
        return 0;
    // Playing something else is a skip of what was playing.
    finishPlay(true);
    stop();
    m_engine->load({});
    m_history.clear();
    m_current = -1;
    m_playlist.replaceTracks(tracks);
    playIndex(qBound(0, start, int(tracks.size()) - 1));
    saveSoon();
    return tracks.size();
}

int Player::queueNext(const QList<Playlist::Track> &tracks) {
    const int at = m_current + 1;
    const int added = m_playlist.insertTracks(at, tracks);
    // Rows remembered for shuffle's "previous" move down with the insert.
    for (int &row : m_history)
        if (row >= at)
            row += added;
    if (added > 0 && m_current < 0)
        load(0);
    saveSoon();
    return added;
}

int Player::appendTracks(const QList<Playlist::Track> &tracks) {
    const int added = m_playlist.insertTracks(m_playlist.count(), tracks);
    if (added > 0 && m_current < 0)
        load(0);
    saveSoon();
    return added;
}

int Player::addUrls(const QVariantList &urls) {
    QStringList paths;
    for (const auto &url : urls)
        paths << url.toUrl().toLocalFile();
    const bool wasEmpty = m_playlist.count() == 0;
    const int added = addPaths(paths);
    if (added == 0)
        setError(tr("No music in what was dropped"));
    else if (wasEmpty)
        playIndex(0);
    return added;
}

void Player::remove(int row) {
    if (row < 0 || row >= m_playlist.count())
        return;
    const bool wasCurrent = row == m_current;
    m_playlist.remove(row);
    m_history.clear();
    if (wasCurrent) {
        stop();
        m_engine->load({});
        m_current = -1;
        if (m_playlist.count() > 0)
            load(qMin(row, m_playlist.count() - 1));
        else
            emit trackChanged();
    } else if (row < m_current) {
        --m_current;
        emit trackChanged();
    }
    requeue();
    saveSoon();
}

void Player::move(int from, int to) {
    if (from < 0 || to < 0 || from >= m_playlist.count() || to >= m_playlist.count() || from == to)
        return;
    m_playlist.move(from, to);
    if (m_current == from)
        m_current = to;
    else if (from < m_current && to >= m_current)
        --m_current;
    else if (from > m_current && to <= m_current)
        ++m_current;
    m_history.clear();
    requeue();
    emit trackChanged();
    saveSoon();
}

void Player::clear() {
    stop();
    m_engine->load({});
    m_playlist.clear();
    m_history.clear();
    m_current = -1;
    emit trackChanged();
    saveSoon();
}

void Player::openFolder() {
    if (isSignalConnected(QMetaMethod::fromSignal(&Player::folderRequested)))
        emit folderRequested();
    else
        setError(tr("No folder picker is available"));
}

void Player::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Player::dismissError() { setError({}); }

void Player::saveSoon() {
    if (!m_statePath.isEmpty())
        m_saveTimer.start();
}

// The playlist, the track and the settings come back on the next start.
// Written beside the target and renamed over it, so a crash never leaves
// half a file.
void Player::save() {
    if (m_statePath.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_statePath).absolutePath());
    // Local files by path, as always. A library reference (a track on a
    // Subsonic server) is kept with its tags, since nothing on disk can
    // give them back, and never with its stream URL, which carries the
    // server's token: that is asked for again when the track plays.
    QJsonArray tracks;
    for (int i = 0; i < m_playlist.count(); ++i) {
        const Playlist::Track &t = m_playlist.at(i);
        if (!Playlist::isReference(t.path)) {
            tracks.append(t.path);
            continue;
        }
        // A URL someone queued with a secret in it isn't written down.
        if (AudioEngine::redact(t.path) != t.path)
            continue;
        QJsonObject entry{{"path", t.path}, {"title", t.title}, {"artist", t.artist},
                          {"durationMs", t.durationMs}};
        if (t.trackId > 0)
            entry.insert("trackId", t.trackId);
        tracks.append(entry);
    }
    QJsonObject root{{"tracks", tracks},
                     {"current", m_current},
                     {"volume", m_volume},
                     {"shuffle", m_shuffle},
                     {"repeat", m_repeat}};
    QSaveFile file(m_statePath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson());
    file.commit();
}

void Player::restore() {
    if (m_statePath.isEmpty())
        return;
    QFile file(m_statePath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    // In order: runs of local files that still exist, and references with
    // the tags they were saved with.
    QStringList files;
    auto addFiles = [&] {
        if (!files.isEmpty())
            m_playlist.addPaths(files);
        files.clear();
    };
    for (const auto &value : root.value("tracks").toArray()) {
        if (value.isObject()) {
            const QJsonObject o = value.toObject();
            Playlist::Track t;
            t.path = o.value("path").toString();
            // A URL with a token in it is never restored as a reference.
            if (!Playlist::isReference(t.path) || AudioEngine::redact(t.path) != t.path)
                continue;
            t.title = o.value("title").toString();
            t.artist = o.value("artist").toString();
            t.durationMs = qint64(o.value("durationMs").toDouble());
            t.trackId = qint64(o.value("trackId").toDouble());
            addFiles();
            m_playlist.insertTracks(m_playlist.count(), {t});
        } else if (QFileInfo::exists(value.toString())) {
            files << value.toString();
        }
    }
    addFiles();
    setVolume(root.value("volume").toInt(m_volume));
    setShuffle(root.value("shuffle").toBool());
    setRepeat(root.value("repeat").toBool());
    const int current = root.value("current").toInt(-1);
    if (current >= 0 && current < m_playlist.count())
        load(current);
    else if (m_playlist.count() > 0)
        load(0);
    m_saveTimer.stop();
}

QString Player::replayGainMode() const {
    switch (m_engine->settings().replayGain) {
    case AudioEngine::ReplayGainMode::Track: return "track";
    case AudioEngine::ReplayGainMode::Album: return "album";
    default: return "off";
    }
}

QList<qreal> Player::eqFrequencies() const {
    const auto &f = Equalizer::frequencies();
    return QList<qreal>(f.begin(), f.end());
}

QStringList Player::eqPresets() const { return Equalizer::presetNames(); }

void Player::changeSettings(const std::function<void(AudioEngine::Settings &)> &change) {
    AudioEngine::Settings settings = m_engine->settings();
    change(settings);
    settings.crossfadeMs = qBound(0, settings.crossfadeMs, 12000);
    settings.replayGainPreampDb = qBound(-15.0, settings.replayGainPreampDb, 15.0);
    settings.eqPreampDb = qBound(-12.0, settings.eqPreampDb, 12.0);
    settings.eqBandsDb.resize(Equalizer::Bands);
    for (double &band : settings.eqBandsDb)
        band = qBound(-12.0, band, 12.0);
    const AudioEngine::Settings before = m_engine->settings();
    if (settings.crossfadeMs == before.crossfadeMs && settings.replayGain == before.replayGain &&
        settings.replayGainPreampDb == before.replayGainPreampDb && settings.eqEnabled == before.eqEnabled &&
        settings.eqPreampDb == before.eqPreampDb && settings.eqBandsDb == before.eqBandsDb &&
        settings.clipPrevention == before.clipPrevention)
        return;
    m_engine->setSettings(settings);
    saveSettings();
    emit playbackSettingsChanged();
}

void Player::setCrossfadeSeconds(qreal seconds) {
    changeSettings([&](AudioEngine::Settings &s) { s.crossfadeMs = int(qRound(seconds * 1000)); });
}

void Player::setReplayGainMode(const QString &mode) {
    const QString m = mode.toLower();
    changeSettings([&](AudioEngine::Settings &s) {
        s.replayGain = m == "track"   ? AudioEngine::ReplayGainMode::Track
                       : m == "album" ? AudioEngine::ReplayGainMode::Album
                                      : AudioEngine::ReplayGainMode::Off;
    });
}

void Player::setReplayGainPreamp(qreal db) {
    changeSettings([&](AudioEngine::Settings &s) { s.replayGainPreampDb = db; });
}

void Player::setEqEnabled(bool on) {
    changeSettings([&](AudioEngine::Settings &s) { s.eqEnabled = on; });
}

void Player::setEqPreamp(qreal db) {
    changeSettings([&](AudioEngine::Settings &s) { s.eqPreampDb = db; });
}

void Player::setEqBands(const QList<qreal> &bands) {
    changeSettings([&](AudioEngine::Settings &s) { s.eqBandsDb = bands; });
}

void Player::setEqBand(int band, qreal db) {
    if (band < 0 || band >= Equalizer::Bands)
        return;
    changeSettings([&](AudioEngine::Settings &s) {
        s.eqBandsDb.resize(Equalizer::Bands);
        s.eqBandsDb[band] = db;
    });
}

bool Player::applyEqPreset(const QString &name) {
    const QList<double> gains = Equalizer::preset(name);
    if (gains.isEmpty())
        return false;
    changeSettings([&](AudioEngine::Settings &s) { s.eqBandsDb = gains; });
    return true;
}

// Playback settings live in omamp.conf, beside the state file, where the
// window keeps its own; the engine choice is read from there at start.
void Player::saveSettings() {
    if (m_settingsPath.isEmpty())
        return;
    const AudioEngine::Settings s = m_engine->settings();
    QSettings conf(m_settingsPath, QSettings::IniFormat);
    conf.beginGroup("playback");
    conf.setValue("crossfadeMs", s.crossfadeMs);
    conf.setValue("replayGain", replayGainMode());
    conf.setValue("replayGainPreampDb", s.replayGainPreampDb);
    conf.setValue("clipPrevention", s.clipPrevention);
    conf.setValue("eqEnabled", s.eqEnabled);
    conf.setValue("eqPreampDb", s.eqPreampDb);
    QStringList bands;
    for (double band : s.eqBandsDb)
        bands << QString::number(band);
    conf.setValue("eqBandsDb", bands.join(','));
    conf.endGroup();
}

void Player::restoreSettings() {
    if (m_settingsPath.isEmpty() || !QFileInfo::exists(m_settingsPath))
        return;
    QSettings conf(m_settingsPath, QSettings::IniFormat);
    conf.beginGroup("playback");
    AudioEngine::Settings s = m_engine->settings();
    s.crossfadeMs = qBound(0, conf.value("crossfadeMs", s.crossfadeMs).toInt(), 12000);
    const QString mode = conf.value("replayGain", "off").toString();
    s.replayGain = mode == "track"   ? AudioEngine::ReplayGainMode::Track
                   : mode == "album" ? AudioEngine::ReplayGainMode::Album
                                     : AudioEngine::ReplayGainMode::Off;
    s.replayGainPreampDb = conf.value("replayGainPreampDb", s.replayGainPreampDb).toDouble();
    s.clipPrevention = conf.value("clipPrevention", s.clipPrevention).toBool();
    s.eqEnabled = conf.value("eqEnabled", s.eqEnabled).toBool();
    s.eqPreampDb = conf.value("eqPreampDb", s.eqPreampDb).toDouble();
    const QStringList bands = conf.value("eqBandsDb").toString().split(',', Qt::SkipEmptyParts);
    if (bands.size() == Equalizer::Bands)
        for (int i = 0; i < Equalizer::Bands; ++i)
            s.eqBandsDb[i] = bands[i].toDouble();
    conf.endGroup();
    m_engine->setSettings(s);
}

// ---- Play statistics ---------------------------------------------------------

void Player::beginPlay() {
    if (m_current < 0 || m_current >= m_playlist.count() || !m_playPath.isEmpty())
        return;
    m_playPath = m_playlist.at(m_current).path;
    m_heardMs = 0;
    m_lastPos = m_engine->position();
    m_nextProgress = 15000;
    emit playStarted(m_playPath);
}

void Player::countHeard(qint64 position) {
    if (!m_playPath.isEmpty() && m_engine->state() == AudioEngine::Playing) {
        // Forward moves while playing are listening; a jump back (a new
        // track, a restart) is not, and seeks reset m_lastPos themselves.
        const qint64 delta = position - m_lastPos;
        if (delta > 0)
            m_heardMs += delta;
        if (m_heardMs >= m_nextProgress) {
            m_nextProgress = (m_heardMs / 15000 + 1) * 15000;
            emit playProgress(m_playPath, m_heardMs);
        }
    }
    m_lastPos = position;
}

void Player::finishPlay(bool userSkipped) {
    if (m_playPath.isEmpty())
        return;
    const QString path = m_playPath;
    const qint64 heard = m_heardMs;
    qint64 length = 0;
    // The playing row still holds the length (load() finishes first).
    if (m_current >= 0 && m_current < m_playlist.count() && m_playlist.at(m_current).path == path)
        length = m_playlist.at(m_current).durationMs;
    if (length <= 0 && m_engine->source() == path)
        length = m_engine->duration();
    m_playPath.clear();
    m_heardMs = 0;
    emit playFinished(path, heard, length, userSkipped);
}

void Player::setClipPrevention(bool on) {
    changeSettings([&](AudioEngine::Settings &s) { s.clipPrevention = on; });
}

QString Player::preferredEngine() const {
    if (m_settingsPath.isEmpty())
        return m_preferredEngine.isEmpty() ? engineName() : m_preferredEngine;
    const QString chosen =
        QSettings(m_settingsPath, QSettings::IniFormat).value("playback/engine").toString().trimmed().toLower();
    return chosen == "native" ? "native" : chosen == "qt" ? "qt" : engineName();
}

void Player::setPreferredEngine(const QString &name) {
    const QString n = name.trimmed().toLower() == "native" ? "native" : "qt";
    if (m_settingsPath.isEmpty()) {
        if (n == m_preferredEngine)
            return;
        m_preferredEngine = n;
    } else {
        QSettings conf(m_settingsPath, QSettings::IniFormat);
        if (conf.value("playback/engine").toString() == n)
            return;
        conf.setValue("playback/engine", n);
    }
    emit playbackSettingsChanged();
}

QVariantMap Player::trackAt(int row) const {
    if (row < 0 || row >= m_playlist.count())
        return {};
    const Playlist::Track &t = m_playlist.at(row);
    return {{"path", t.path}, {"title", t.title}, {"artist", t.artist}, {"durationMs", t.durationMs}};
}
