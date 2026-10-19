#include "qtengine.h"

#include <QMediaMetaData>
#include <QUrl>

QtEngine::QtEngine(bool withDevice, QObject *parent) : AudioEngine(parent) {
    if (withDevice && !qEnvironmentVariableIsSet("OMAMP_NO_AUDIO_DEVICE"))
        m_player.setAudioOutput(&m_output);
    m_player.setAudioBufferOutput(&m_bufferOutput);

    connect(&m_bufferOutput, &QAudioBufferOutput::audioBufferReceived, this, [this](const QAudioBuffer &buffer) {
        emit pcmTapped(buffer);
        const auto format = buffer.format();
        if (format.sampleRate() != m_sampleRate || format.channelCount() != m_channels) {
            m_sampleRate = format.sampleRate();
            m_channels = format.channelCount();
            emit formatChanged(m_sampleRate, m_channels);
        }
    });
    connect(&m_player, &QMediaPlayer::positionChanged, this, &AudioEngine::positionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, &AudioEngine::durationChanged);
    connect(&m_player, &QMediaPlayer::metaDataChanged, this, &AudioEngine::metaDataChanged);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &AudioEngine::stateChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            if (m_queued.isEmpty()) {
                emit endOfQueue();
                return;
            }
            const QString path = m_queued;
            m_queued.clear();
            // Before the new source, so the Player clears the old tags
            // before the new ones arrive.
            emit advanced();
            load(path);
            m_player.play();
        } else if (status == QMediaPlayer::InvalidMedia) {
            emit trackFailed(source(), {});
        }
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString &text) {
        if (error != QMediaPlayer::NoError)
            emit errorOccurred(text);
    });
}

void QtEngine::load(const QString &path) {
    m_player.setSource(path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path));
}

AudioEngine::State QtEngine::state() const {
    switch (m_player.playbackState()) {
    case QMediaPlayer::PlayingState: return Playing;
    case QMediaPlayer::PausedState: return Paused;
    default: return Stopped;
    }
}

QVariantMap QtEngine::metaData() const {
    const QMediaMetaData meta = m_player.metaData();
    return {{"title", meta.stringValue(QMediaMetaData::Title)},
            {"artist", meta.stringValue(QMediaMetaData::ContributingArtist)},
            {"albumArtist", meta.stringValue(QMediaMetaData::AlbumArtist)},
            {"album", meta.stringValue(QMediaMetaData::AlbumTitle)},
            {"bitrate", qRound(meta.value(QMediaMetaData::AudioBitRate).toInt() / 1000.0)}};
}
