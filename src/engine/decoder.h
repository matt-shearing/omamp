#pragma once

#include <QString>
#include <QUrl>
#include <QtNumeric>
#include <QVariantMap>
#include <QVector>
#include <functional>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

// One file or stream decoded with FFmpeg into interleaved float stereo at
// its own sample rate. Mono plays at full level on both sides; more than
// two channels are mixed down by libswresample.
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

    bool open(const QString &path, QString *error = nullptr) { return open(path, QUrl(), error); }
    // Opens url when it is valid (an http(s) stream: with a read timeout,
    // reconnection and read-ahead), else path. path() stays path either
    // way, and no error message quotes the URL.
    bool open(const QString &path, const QUrl &url, QString *error = nullptr);
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
    // earlier keyframe and drops what comes before ms. A stream whose
    // server won't serve byte ranges is read forward to ms, or opened again
    // from the start when ms lies behind.
    bool seek(qint64 ms);

    // Checked while FFmpeg waits on the network and between packets:
    // returning true abandons the open, read or seek at once. Set before
    // open(); called on whichever thread is using the decoder.
    void setInterrupt(std::function<bool()> check) { m_interrupt = std::move(check); }
    bool isNetwork() const { return m_network; }
    // Whether FFmpeg can seek in the source: always for a local file, and
    // for a stream when the server serves byte ranges.
    bool seekable() const;
    // Set when a network stream broke off before its end: the reason, with
    // any URL secrets removed. Empty after a clean end.
    QString readError() const { return m_readError; }

    QString path() const { return m_path; }
    int sampleRate() const { return m_rate; }
    // The file's own channel count, for display; output is always stereo.
    int sourceChannels() const { return m_sourceChannels; }
    qint64 durationMs() const { return m_durationMs; }
    int bitrateKbps() const { return m_bitrateKbps; }
    // title, artist, albumArtist, album, bitrate, and for a live stream
    // streamTitle: the ICY title the station sends now ("Artist - Title").
    QVariantMap metaData() const { return m_meta; }
    // A live radio stream (a radio:// queue entry): no length, no seeking,
    // reconnected when it drops, and its ICY titles read as they change.
    bool isLive() const { return m_live; }
    QString streamTitle() const { return m_meta.value(QStringLiteral("streamTitle")).toString(); }
    // True once after the ICY title changed; the native engine then tells
    // the window at the moment that audio is heard.
    bool takeStreamTitleChanged() {
        const bool changed = m_titleChanged;
        m_titleChanged = false;
        return changed;
    }
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
    // Reads FFmpeg's latest ICY metadata block from the HTTP layer.
    void pollIcy();
    bool interrupted() const { return m_interrupt && m_interrupt(); }
    static int interruptCallback(void *opaque);

    QString m_path;
    QUrl m_url;
    bool m_network = false;
    bool m_live = false, m_titleChanged = false;
    int m_packetsSinceIcy = 0;
    std::function<bool()> m_interrupt;
    QString m_readError;
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
    // The file sample just after the last one decoded, so a seek forward in
    // a stream that can't seek knows whether it may read on.
    qint64 m_decodedTo = 0;
};
