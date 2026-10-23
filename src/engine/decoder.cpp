#include "decoder.h"

#include "spotifyaudio.h"

#include <QRegularExpression>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "audioengine.h"
#include "streaming.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {
// FFmpeg prints every damaged frame to stderr. Quiet unless asked.
// Decoders are opened on more than one thread (the next track opens in the
// background), so this runs exactly once.
// When asked for, FFmpeg's log goes to stderr through redact(): at verbose
// levels FFmpeg quotes the URLs it opens, and a stream URL carries the
// server's token.
void redactingLog(void *avcl, int level, const char *fmt, va_list args) {
    if (level > av_log_get_level())
        return;
    char line[2048];
    static int prefix = 1;
    av_log_format_line2(avcl, level, fmt, args, line, sizeof line, &prefix);
    fputs(AudioEngine::redact(QString::fromUtf8(line)).toUtf8().constData(), stderr);
}

void quietFfmpegOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!qEnvironmentVariableIsSet("THRUM_FFMPEG_LOG"))
            av_log_set_level(AV_LOG_FATAL);
        else
            av_log_set_callback(redactingLog);
    });
}

QString averror(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return AudioEngine::redact(QString::fromUtf8(text));
}
} // namespace

Decoder::Decoder() { quietFfmpegOnce(); }

int Decoder::interruptCallback(void *opaque) { return static_cast<Decoder *>(opaque)->interrupted() ? 1 : 0; }

void Decoder::pollIcy() {
    if (!m_format || !m_format->pb)
        return;
    uint8_t *packet = nullptr;
    if (av_opt_get(m_format->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &packet) < 0 || !packet)
        return;
    const QByteArray block(reinterpret_cast<const char *>(packet));
    av_free(packet);
    if (block.isEmpty())
        return;
    // "StreamTitle='Artist - Title';": servers write UTF-8 or Latin-1.
    QString text = QString::fromUtf8(block);
    if (text.contains(QChar::ReplacementCharacter))
        text = QString::fromLatin1(block);
    const int start = text.indexOf(QLatin1String("StreamTitle='"));
    if (start < 0)
        return;
    int end = text.indexOf(QLatin1String("';"), start + 13);
    if (end < 0)
        end = text.lastIndexOf('\'');
    const QString title = end > start + 13 ? text.mid(start + 13, end - start - 13).trimmed() : QString();
    if (title != streamTitle()) {
        m_meta.insert(QStringLiteral("streamTitle"), title);
        m_titleChanged = true;
    }
}

bool Decoder::atEnd() const {
    if (m_spotify)
        return m_spotify->atEnd();
    return m_eof && m_fifo.size() == m_head;
}

void Decoder::prime() {
    if (m_spotify) {
        m_spotify->prime();
        return;
    }
    if (m_codec && m_fifo.size() == m_head && !m_eof)
        decodeMore();
}

bool Decoder::openSpotify(QString *error) {
    const auto audio = SpotifyAudio::installed();
    if (!audio) {
        if (error)
            *error = QObject::tr("Thrum can't play Spotify itself here.");
        return false;
    }
    m_spotify = std::make_unique<SpotifyStream>(audio);
    if (!m_spotify->open(m_path, [this] { return interrupted(); }, error)) {
        m_spotify.reset();
        return false;
    }
    m_rate = m_spotify->sampleRate();
    m_sourceChannels = 2;
    m_durationMs = m_spotify->durationMs();
    m_bitrateKbps = m_spotify->bitrateKbps();
    m_meta = m_spotify->metaData();
    m_rg.trackGain = m_spotify->trackGain;
    m_rg.trackPeak = m_spotify->trackPeak;
    m_rg.albumGain = m_spotify->albumGain;
    m_rg.albumPeak = m_spotify->albumPeak;
    return true;
}

bool Decoder::seekable() const {
    if (m_spotify)
        return true;
    if (!m_format || !m_format->pb)
        return false;
    return (m_format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
}

Decoder::~Decoder() { close(); }

void Decoder::close() {
    m_spotify.reset();
    if (m_swr)
        swr_free(&m_swr);
    if (m_codec)
        avcodec_free_context(&m_codec);
    if (m_format)
        avformat_close_input(&m_format);
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_frame)
        av_frame_free(&m_frame);
    m_stream = -1;
    m_fifo.clear();
    m_head = 0;
    m_eof = m_drained = false;
}

QString Decoder::tag(const char *key) const {
    if (!m_format)
        return {};
    // Ogg and FLAC keep tags on the stream, most others on the container.
    const AVDictionaryEntry *entry = av_dict_get(m_format->metadata, key, nullptr, 0);
    if (!entry && m_stream >= 0)
        entry = av_dict_get(m_format->streams[m_stream]->metadata, key, nullptr, 0);
    return entry ? QString::fromUtf8(entry->value).trimmed() : QString();
}

double Decoder::parseGain(const QString &text) {
    static const QRegularExpression number(QStringLiteral("^\\s*([+-]?[0-9]*[.,]?[0-9]+)"));
    const auto match = number.match(text);
    if (!match.hasMatch())
        return qQNaN();
    bool ok = false;
    const double value = match.captured(1).replace(',', '.').toDouble(&ok);
    return ok ? value : qQNaN();
}

bool Decoder::open(const QString &path, const QUrl &url, QString *error) {
    close();
    m_path = path;
    m_url = url;
    m_network = AudioEngine::isNetworkUrl(url);
    m_live = m_network && Streaming::isLive(path);
    m_titleChanged = false;
    m_packetsSinceIcy = 0;
    m_readError.clear();
    m_decodedTo = 0;
    if (!SpotifyAudio::trackId(path).isEmpty())
        return openSpotify(error);
    auto failWith = [&](const QString &message) {
        if (error)
            *error = AudioEngine::redact(message);
        close();
        return false;
    };

    m_format = avformat_alloc_context();
    if (!m_format)
        return failWith(QStringLiteral("out of memory"));
    // Lets a skip, a seek or quitting abandon a stream that is waiting on
    // the network, instead of waiting out the timeout.
    m_format->interrupt_callback.callback = &Decoder::interruptCallback;
    m_format->interrupt_callback.opaque = this;

    AVDictionary *options = nullptr;
    if (m_network) {
        const Streaming::Options o = Streaming::options();
        const QByteArray timeout = QByteArray::number(qint64(o.rwTimeoutMs) * 1000);
        // A read that waits this long fails (rw_timeout for any protocol,
        // timeout for the TCP connect and reads), so a dead server ends the
        // track rather than hanging the worker.
        av_dict_set(&options, "rw_timeout", timeout.constData(), 0);
        av_dict_set(&options, "timeout", timeout.constData(), 0);
        // A dropped connection is opened again where it broke off (with a
        // Range request) when the server allows it, a few times with
        // growing waits, and then fails.
        av_dict_set(&options, "reconnect", "1", 0);
        // Not for a stream without ranges: FFmpeg would ask for the rest
        // with a Range the server ignores, take the file's start for its
        // continuation and play on through garbage (the streaming tests
        // caught it). Such a stream fails instead, and the Player moves on.
        // A live radio stream has no position to get wrong: reconnecting
        // simply picks the broadcast up again.
        av_dict_set(&options, "reconnect_streamed", m_live ? "1" : "0", 0);
        // Ask for the station's song titles inside the stream (ICY). FFmpeg
        // takes them out of the audio and keeps the latest for pollIcy().
        if (m_live)
            av_dict_set(&options, "icy", "1", 0);
        av_dict_set(&options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", QByteArray::number(o.reconnectDelayMaxS).constData(), 0);
        av_dict_set(&options, "reconnect_delay_total_max", QByteArray::number(o.reconnectDelayMaxS * 3).constData(),
                    0);
        av_dict_set(&options, "user_agent", Streaming::userAgent().toUtf8().constData(), 0);
    }
    const QByteArray location = Streaming::ffmpegLocation(path, url);
    int r = avformat_open_input(&m_format, location.constData(), nullptr, &options);
    av_dict_free(&options);
    if (r < 0) {
        // avformat_open_input frees the context when it fails.
        m_format = nullptr;
        return failWith(interrupted() ? QStringLiteral("cancelled") : averror(r));
    }
    r = avformat_find_stream_info(m_format, nullptr);
    if (r < 0)
        return failWith(averror(r));
    const AVCodec *codec = nullptr;
    m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (m_stream < 0 || !codec)
        return failWith(QStringLiteral("no audio stream"));
    // Only the audio stream is read; skipping the others saves work on
    // files with cover art or video.
    for (unsigned i = 0; i < m_format->nb_streams; ++i)
        if (int(i) != m_stream)
            m_format->streams[i]->discard = AVDISCARD_ALL;

    AVStream *stream = m_format->streams[m_stream];
    m_codec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codec, stream->codecpar);
    m_codec->pkt_timebase = stream->time_base;
    r = avcodec_open2(m_codec, codec, nullptr);
    if (r < 0)
        return failWith(averror(r));
    if (m_codec->sample_rate <= 0)
        return failWith(QStringLiteral("unknown sample rate"));

    AVChannelLayout in;
    if (m_codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || m_codec->ch_layout.nb_channels <= 0)
        av_channel_layout_default(&in, qMax(1, m_codec->ch_layout.nb_channels));
    else
        av_channel_layout_copy(&in, &m_codec->ch_layout);
    m_sourceChannels = in.nb_channels;
    // Mono stays mono through swresample and is copied to both sides below;
    // swresample's own upmix would lower it by 3 dB.
    m_swrChannels = in.nb_channels == 1 ? 1 : 2;
    AVChannelLayout out;
    av_channel_layout_default(&out, m_swrChannels);
    r = swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_FLT, m_codec->sample_rate, &in, m_codec->sample_fmt,
                            m_codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in);
    av_channel_layout_uninit(&out);
    if (r < 0 || swr_init(m_swr) < 0)
        return failWith(QStringLiteral("cannot convert the sample format"));

    m_rate = m_codec->sample_rate;
    if (m_format->duration > 0)
        m_durationMs = m_format->duration / (AV_TIME_BASE / 1000);
    else if (stream->duration > 0)
        m_durationMs = av_rescale_q(stream->duration, stream->time_base, {1, 1000});
    const qint64 bitrate = stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate : m_format->bit_rate;
    m_bitrateKbps = int(qRound64(bitrate / 1000.0));

    QString artist = tag("artist");
    QString albumArtist = tag("album_artist");
    if (albumArtist.isEmpty())
        albumArtist = tag("albumartist");
    m_meta = {{"title", tag("title")},
              {"artist", artist},
              {"albumArtist", albumArtist},
              {"album", tag("album")},
              {"bitrate", m_bitrateKbps}};
    if (m_live) {
        // No length: the station never ends.
        m_durationMs = 0;
        m_meta.insert(QStringLiteral("streamTitle"), QString());
        pollIcy();
    }

    m_rg = {};
    m_rg.trackGain = parseGain(tag("REPLAYGAIN_TRACK_GAIN"));
    m_rg.trackPeak = parseGain(tag("REPLAYGAIN_TRACK_PEAK"));
    m_rg.albumGain = parseGain(tag("REPLAYGAIN_ALBUM_GAIN"));
    m_rg.albumPeak = parseGain(tag("REPLAYGAIN_ALBUM_PEAK"));
    // Opus files may carry R128 gains instead: Q7.8 dB relative to
    // -23 LUFS, which is 5 dB below ReplayGain's reference.
    if (std::isnan(m_rg.trackGain) && !tag("R128_TRACK_GAIN").isEmpty())
        m_rg.trackGain = tag("R128_TRACK_GAIN").toInt() / 256.0 + 5.0;
    if (std::isnan(m_rg.albumGain) && !tag("R128_ALBUM_GAIN").isEmpty())
        m_rg.albumGain = tag("R128_ALBUM_GAIN").toInt() / 256.0 + 5.0;

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_badPackets = 0;
    m_discardBefore = -1;
    return true;
}

// Decodes until at least one more frame of audio is in the FIFO, or the end.
bool Decoder::decodeMore() {
    while (!m_eof) {
        int r = avcodec_receive_frame(m_codec, m_frame);
        if (r == 0) {
            convert(m_frame);
            av_frame_unref(m_frame);
            return true;
        }
        if (r == AVERROR_EOF) {
            m_eof = true;
            break;
        }
        if (r != AVERROR(EAGAIN)) {
            // A decoder that cannot go on: treat what we have as the end.
            m_eof = true;
            break;
        }
        if (m_drained) {
            m_eof = true;
            break;
        }
        r = av_read_frame(m_format, m_packet);
        if (m_live && r >= 0 && ++m_packetsSinceIcy >= 16) {
            m_packetsSinceIcy = 0;
            pollIcy();
        }
        if (r < 0) {
            // A stream that broke off (FFmpeg's reconnects failed or timed
            // out) is reported, unless a skip or seek abandoned the read.
            // Local files keep treating any read error as the end.
            if (m_network && !interrupted() && m_readError.isEmpty()) {
                // FFmpeg may call a connection that closed early the end,
                // and the clean end of a chunked answer an I/O error. So
                // what counts is whether the stream is all here: by the
                // length the server gave, else by the length of the audio;
                // knowing neither, only a real error (a timeout) counts.
                bool known = false, cut = false;
                const int64_t size = m_format->pb ? avio_size(m_format->pb) : -1;
                const int64_t at = m_format->pb ? avio_tell(m_format->pb) : -1;
                if (size > 0 && at >= 0) {
                    known = true;
                    cut = at < size;
                } else if (m_durationMs > 0 && m_rate > 0) {
                    known = true;
                    cut = m_decodedTo * 1000 / m_rate < m_durationMs - 1000;
                }
                if (known ? cut : (r != AVERROR_EOF && r != AVERROR(EIO)))
                    m_readError = r != AVERROR_EOF ? averror(r) : QStringLiteral("the stream broke off");
            }
            // The end of the file, or a read error: drain the decoder so the
            // last frames (and FFmpeg's end trimming) come out.
            avcodec_send_packet(m_codec, nullptr);
            m_drained = true;
            continue;
        }
        if (m_packet->stream_index != m_stream) {
            av_packet_unref(m_packet);
            continue;
        }
        r = avcodec_send_packet(m_codec, m_packet);
        av_packet_unref(m_packet);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            // One damaged packet is skipped; a run of them ends the track.
            if (++m_badPackets > 100)
                m_eof = true;
        } else {
            m_badPackets = 0;
        }
    }
    // Whatever swresample still holds.
    if (m_swr) {
        const int pending = int(swr_get_out_samples(m_swr, 0));
        if (pending > 0) {
            m_scratch.resize(pending * m_swrChannels);
            uint8_t *outPlanes[1] = {reinterpret_cast<uint8_t *>(m_scratch.data())};
            const int got = swr_convert(m_swr, outPlanes, pending, nullptr, 0);
            for (int i = 0; i < got; ++i) {
                const float l = m_scratch[i * m_swrChannels];
                m_fifo << l << (m_swrChannels == 1 ? l : m_scratch[i * m_swrChannels + 1]);
            }
        }
    }
    return false;
}

void Decoder::convert(AVFrame *frame) {
    const int capacity = int(swr_get_out_samples(m_swr, frame->nb_samples));
    m_scratch.resize(qMax(capacity, 1) * m_swrChannels);
    uint8_t *outPlanes[1] = {reinterpret_cast<uint8_t *>(m_scratch.data())};
    const int got = swr_convert(m_swr, outPlanes, capacity, const_cast<const uint8_t **>(frame->extended_data),
                                frame->nb_samples);
    if (got <= 0)
        return;

    int skip = 0;
    const qint64 pts = frame->best_effort_timestamp;
    qint64 first = 0;
    const bool timed = pts != AV_NOPTS_VALUE;
    if (timed) {
        const AVStream *stream = m_format->streams[m_stream];
        const qint64 start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
        first = av_rescale_q(pts - start, stream->time_base, {1, m_rate});
        m_decodedTo = first + got;
    } else {
        m_decodedTo += got;
    }
    if (m_discardBefore >= 0) {
        if (timed) {
            if (first + got <= m_discardBefore)
                return;
            skip = int(qBound<qint64>(0, m_discardBefore - first, qint64(got)));
        }
        m_discardBefore = -1;
    }

    // Compact the FIFO now and then rather than on every read.
    if (m_head > 0 && m_head * 2 > m_fifo.size()) {
        m_fifo.remove(0, m_head);
        m_head = 0;
    }
    const qsizetype at = m_fifo.size();
    m_fifo.resize(at + qsizetype(got - skip) * 2);
    float *dst = m_fifo.data() + at;
    if (m_swrChannels == 1) {
        for (int i = skip; i < got; ++i) {
            *dst++ = m_scratch[i];
            *dst++ = m_scratch[i];
        }
    } else {
        std::copy(m_scratch.constData() + skip * 2, m_scratch.constData() + got * 2, dst);
    }
}

int Decoder::read(float *out, int frames) {
    if (m_spotify) {
        const int n = m_spotify->read(out, frames);
        if (n == 0)
            m_readError = m_spotify->readError();
        return n;
    }
    if (!m_codec)
        return 0;
    int done = 0;
    while (done < frames) {
        const qsizetype available = (m_fifo.size() - m_head) / 2;
        if (available == 0) {
            if (m_eof || !decodeMore()) {
                if ((m_fifo.size() - m_head) / 2 == 0)
                    break;
            }
            continue;
        }
        const int take = int(qMin<qsizetype>(available, frames - done));
        std::copy(m_fifo.constData() + m_head, m_fifo.constData() + m_head + take * 2, out + done * 2);
        m_head += take * 2;
        done += take;
    }
    if (m_head == m_fifo.size()) {
        m_fifo.clear();
        m_head = 0;
    }
    return done;
}

bool Decoder::seek(qint64 ms) {
    if (m_spotify)
        return m_spotify->seek(ms);
    if (!m_codec || m_live)
        return false;
    AVStream *stream = m_format->streams[m_stream];
    const qint64 start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    // Lossy codecs need a few frames before the target to rebuild their
    // state (MDCT overlap, the MP3 bit reservoir, Opus pre-roll); decoding
    // from there and dropping up to the target keeps the first sample clean.
    qint64 preroll = 0;
    const AVCodecDescriptor *codec = avcodec_descriptor_get(m_codec->codec_id);
    if (!codec || !(codec->props & AV_CODEC_PROP_LOSSLESS)) {
        preroll = 150;
        if (stream->codecpar->seek_preroll > 0)
            preroll = qMax<qint64>(preroll, av_rescale(stream->codecpar->seek_preroll, 1000, m_rate) + 20);
    }
    const qint64 target =
        start + av_rescale_q(qMax<qint64>(0, ms - preroll), {1, 1000}, stream->time_base);
    int r = av_seek_frame(m_format, m_stream, target, AVSEEK_FLAG_BACKWARD);
    if (r < 0)
        r = av_seek_frame(m_format, m_stream, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    if (r < 0 && m_network && !interrupted()) {
        // The server won't serve byte ranges (a transcoded stream, say), so
        // FFmpeg can't jump. Read on to the target when it lies ahead;
        // otherwise start the stream again and read up to it.
        const qint64 wanted = av_rescale(qMax<qint64>(0, ms), m_rate, 1000);
        if (wanted < m_decodedTo || m_eof) {
            QString why;
            if (!open(m_path, m_url, &why)) {
                m_readError = why;
                return false;
            }
        }
        m_fifo.clear();
        m_head = 0;
        m_discardBefore = wanted;
        return true;
    }
    if (r < 0)
        return false;
    avcodec_flush_buffers(m_codec);
    swr_init(m_swr);
    m_fifo.clear();
    m_head = 0;
    m_eof = m_drained = false;
    m_badPackets = 0;
    m_readError.clear();
    m_discardBefore = av_rescale(qMax<qint64>(0, ms), m_rate, 1000);
    return true;
}
