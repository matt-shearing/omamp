#include "player.h"

#include "portal.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaMetaData>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QUrl>
#include <cmath>

Player::Player(const QString &statePath, QObject *parent) : QObject(parent), m_statePath(statePath) {
    m_player.setAudioOutput(&m_output);
    m_player.setAudioBufferOutput(&m_bufferOutput);

    connect(&m_bufferOutput, &QAudioBufferOutput::audioBufferReceived, this, [this](const QAudioBuffer &buffer) {
        m_spectrum.feed(buffer);
        const auto format = buffer.format();
        if (format.sampleRate() != m_sampleRate || format.channelCount() != m_channels) {
            m_sampleRate = format.sampleRate();
            m_channels = format.channelCount();
            emit formatChanged();
        }
    });

    // The bars fall back between buffers and settle to zero when stopped.
    m_spectrumTimer.setInterval(33);
    connect(&m_spectrumTimer, &QTimer::timeout, this, [this] {
        m_spectrum.decay();
        emit levelsChanged();
    });
    m_spectrumTimer.start();

    connect(&m_player, &QMediaPlayer::positionChanged, this, &Player::positionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, [this] {
        if (m_current >= 0)
            m_playlist.updateTags(m_current, {}, {}, m_player.duration());
        emit trackChanged();
    });
    connect(&m_player, &QMediaPlayer::metaDataChanged, this, &Player::readMetaData);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &Player::stateChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            const int after = nextIndex();
            if (after < 0) {
                stop();
                return;
            }
            m_history << m_current;
            load(after);
            m_player.play();
        } else if (status == QMediaPlayer::InvalidMedia) {
            setError(tr("Can't play %1").arg(QFileInfo(m_playlist.at(m_current).path).fileName()));
        }
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString &text) {
        if (error != QMediaPlayer::NoError)
            setError(text);
    });

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &Player::save);
    connect(&m_playlist, &Playlist::countChanged, this, &Player::saveSoon);

    m_output.setVolume(QAudio::convertVolume(m_volume / 100.0, QAudio::LogarithmicVolumeScale,
                                             QAudio::LinearVolumeScale));
    restore();
}

Player::~Player() {
    if (m_saveTimer.isActive())
        save();
}

void Player::setPicker(FolderPicker *picker) {
    m_picker = picker;
    connect(picker, &FolderPicker::picked, this, [this](const QStringList &paths) {
        const bool wasEmpty = m_playlist.count() == 0;
        if (addPaths(paths) == 0)
            setError(tr("No music found there"));
        else if (wasEmpty)
            playIndex(0);
    });
    connect(picker, &FolderPicker::failed, this, &Player::setError);
}

QString Player::state() const {
    switch (m_player.playbackState()) {
    case QMediaPlayer::PlayingState: return "playing";
    case QMediaPlayer::PausedState: return "paused";
    default: return "stopped";
    }
}

qint64 Player::duration() const {
    if (m_player.duration() > 0)
        return m_player.duration();
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
    m_output.setVolume(QAudio::convertVolume(volume / 100.0, QAudio::LogarithmicVolumeScale,
                                             QAudio::LinearVolumeScale));
    emit volumeChanged();
    saveSoon();
}

void Player::setShuffle(bool on) {
    if (on == m_shuffle)
        return;
    m_shuffle = on;
    emit modesChanged();
    saveSoon();
}

void Player::setRepeat(bool on) {
    if (on == m_repeat)
        return;
    m_repeat = on;
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
    m_player.setSource(QUrl::fromLocalFile(m_playlist.at(row).path));
    emit trackChanged();
    saveSoon();
}

void Player::readMetaData() {
    const QMediaMetaData meta = m_player.metaData();
    m_title = meta.stringValue(QMediaMetaData::Title);
    m_artist = meta.stringValue(QMediaMetaData::ContributingArtist);
    if (m_artist.isEmpty())
        m_artist = meta.stringValue(QMediaMetaData::AlbumArtist);
    m_album = meta.stringValue(QMediaMetaData::AlbumTitle);
    m_bitrate = qRound(meta.value(QMediaMetaData::AudioBitRate).toInt() / 1000.0);
    if (m_current >= 0)
        m_playlist.updateTags(m_current, m_title, m_artist, m_player.duration());
    emit trackChanged();
}

void Player::play() {
    if (m_playlist.count() == 0)
        return;
    if (m_current < 0 || m_player.source().isEmpty())
        load(qMax(0, m_current));
    m_player.play();
}

void Player::pause() { m_player.pause(); }

void Player::toggle() {
    if (m_player.playbackState() == QMediaPlayer::PlayingState)
        pause();
    else
        play();
}

void Player::stop() {
    m_player.stop();
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
    const bool playing = m_player.playbackState() == QMediaPlayer::PlayingState;
    m_history << m_current;
    load(after);
    if (playing)
        m_player.play();
}

void Player::previous() {
    if (m_playlist.count() == 0)
        return;
    // Like most players: a few seconds in, previous restarts the track.
    if (m_player.position() > 3000) {
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
    const bool playing = m_player.playbackState() == QMediaPlayer::PlayingState;
    load(before);
    if (playing)
        m_player.play();
}

void Player::playIndex(int row) {
    if (row < 0 || row >= m_playlist.count())
        return;
    if (m_current >= 0)
        m_history << m_current;
    load(row);
    m_player.play();
}

void Player::seek(qint64 ms) {
    m_player.setPosition(qBound<qint64>(0, ms, qMax<qint64>(0, duration())));
    emit seeked(m_player.position());
}

void Player::seekBy(qint64 ms) { seek(m_player.position() + ms); }

int Player::addPaths(const QStringList &paths) {
    const int added = m_playlist.addPaths(paths);
    if (added > 0 && m_current < 0)
        load(0);
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
        m_player.setSource({});
        m_current = -1;
        if (m_playlist.count() > 0)
            load(qMin(row, m_playlist.count() - 1));
        else
            emit trackChanged();
    } else if (row < m_current) {
        --m_current;
        emit trackChanged();
    }
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
    emit trackChanged();
    saveSoon();
}

void Player::clear() {
    stop();
    m_player.setSource({});
    m_playlist.clear();
    m_history.clear();
    m_current = -1;
    emit trackChanged();
    saveSoon();
}

void Player::openFolder() {
    if (m_picker)
        m_picker->pickFolders();
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
