#pragma once

#include <QString>
#include <QtNumeric>
#include <QVariantMap>
#include <QVector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

// One file decoded with FFmpeg into interleaved float stereo at the file's
// own sample rate. Mono plays at full level on both sides; more than two
// channels are mixed down by libswresample.
//
// Encoder delay and padding are left to FFmpeg, which trims them while
// decoding: the LAME/Xing header of MP3s, Opus pre-skip and end trimming
// from the last Ogg granule, and MP4 edit lists (AAC priming). The tests
// check each of these sample for sample.
//
// Not thread-safe; one thread uses a decoder at a time.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    bool open(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_codec != nullptr; }

    // Up to frames frames into out (stereo, interleaved). 0 means the end.
    int read(float *out, int frames);
    bool atEnd() const { return m_eof && m_fifo.size() == m_head; }
    // Decodes the first frame ahead of time, so a queued track starts at once.
    void prime() {
        if (m_codec && m_fifo.size() == m_head && !m_eof)
            decodeMore();
    }
    // Sample-accurate where the format allows: decodes from the nearest
    // earlier keyframe and drops what comes before ms.
    bool seek(qint64 ms);

    QString path() const { return m_path; }
    int sampleRate() const { return m_rate; }
    // The file's own channel count, for display; output is always stereo.
    int sourceChannels() const { return m_sourceChannels; }
    qint64 durationMs() const { return m_durationMs; }
    int bitrateKbps() const { return m_bitrateKbps; }
    // title, artist, albumArtist, album, bitrate.
    QVariantMap metaData() const { return m_meta; }
    // A tag from the container or the stream, matched without case.
    QString tag(const char *key) const;

    struct ReplayGain {
        double trackGain = qQNaN(), trackPeak = qQNaN();
        double albumGain = qQNaN(), albumPeak = qQNaN();
    };
    ReplayGain replayGain() const { return m_rg; }

    // Parses "-6.20 dB" style values. NaN when missing or unreadable.
    static double parseGain(const QString &text);

private:
    bool decodeMore();
    void convert(AVFrame *frame);
    void fail(const QString &message);

    QString m_path;
    AVFormatContext *m_format = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwrContext *m_swr = nullptr;
    AVPacket *m_packet = nullptr;
    AVFrame *m_frame = nullptr;
    int m_stream = -1;
    int m_rate = 0, m_sourceChannels = 0, m_swrChannels = 0;
    qint64 m_durationMs = 0;
    int m_bitrateKbps = 0;
    QVariantMap m_meta;
    ReplayGain m_rg;

    QVector<float> m_fifo, m_scratch;
    qsizetype m_head = 0;
    bool m_drained = false, m_eof = false;
    int m_badPackets = 0;
    // After a seek: drop decoded samples before this one (file sample index).
    qint64 m_discardBefore = -1;
};
