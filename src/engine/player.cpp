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

Player::Player(const QString &statePath, QObject *parent)
    : Player(chooseEngine(settingsPathFor(statePath)), statePath, parent) {}

Player::Player(AudioEngine *engine, const QString &statePath, QObject *parent)
    : QObject(parent), m_engine(engine), m_statePath(statePath), m_settingsPath(settingsPathFor(statePath)) {
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
        emit positionChanged();
    });
    connect(m_engine, &AudioEngine::durationChanged, this, [this] {
        if (m_current >= 0)
            m_playlist.updateTags(m_current, {}, {}, m_engine->duration());
        emit trackChanged();
    });
    connect(m_engine, &AudioEngine::metaDataChanged, this, &Player::readMetaData);
    connect(m_engine, &AudioEngine::stateChanged, this, &Player::stateChanged);
    connect(m_engine, &AudioEngine::advanced, this, [this] {
        // The engine moved on to the queued row by itself.
        if (m_queued < 0 || m_queued >= m_playlist.count())
            return;
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
    });
    connect(m_engine, &AudioEngine::endOfQueue, this, &Player::stop);
    connect(m_engine, &AudioEngine::trackFailed, this, [this](const QString &path, const QString &) {
        setError(tr("Can't play %1").arg(QFileInfo(path).fileName()));
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

void Player::load(int row) {
    if (row < 0 || row >= m_playlist.count())
        return;
    m_current = row;
    m_title.clear();
    m_artist.clear();
    m_album.clear();
    m_bitrate = 0;
    m_spectrum.reset();
    m_engine->load(m_playlist.at(row).path);
    requeue();
    emit trackChanged();
    saveSoon();
}

// Tells the engine which file follows the current one, so it can open it
// early and join the two without a gap.
void Player::requeue() {
    m_queued = m_current >= 0 ? nextIndex() : -1;
    m_engine->queueNext(m_queued >= 0 ? m_playlist.at(m_queued).path : QString());
}

void Player::readMetaData() {
    const QVariantMap meta = m_engine->metaData();
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
    load(after);
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
    load(before);
    if (playing)
        m_engine->play();
}

void Player::playIndex(int row) {
    if (row < 0 || row >= m_playlist.count())
        return;
    if (m_current >= 0)
        m_history << m_current;
    load(row);
    m_wantPlaying = true;
    m_engine->play();
}

void Player::seek(qint64 ms) {
    m_engine->seek(qBound<qint64>(0, ms, qMax<qint64>(0, duration())));
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
    QJsonObject root{{"tracks", QJsonArray::fromStringList(m_playlist.paths())},
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
    QStringList tracks;
    for (const auto &value : root.value("tracks").toArray())
        if (QFileInfo::exists(value.toString()))
            tracks << value.toString();
    m_playlist.addPaths(tracks);
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
    s.eqEnabled = conf.value("eqEnabled", s.eqEnabled).toBool();
    s.eqPreampDb = conf.value("eqPreampDb", s.eqPreampDb).toDouble();
    const QStringList bands = conf.value("eqBandsDb").toString().split(',', Qt::SkipEmptyParts);
    if (bands.size() == Equalizer::Bands)
        for (int i = 0; i < Equalizer::Bands; ++i)
            s.eqBandsDb[i] = bands[i].toDouble();
    conf.endGroup();
    m_engine->setSettings(s);
}
