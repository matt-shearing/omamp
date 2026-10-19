#include "nativeengine.h"

#include <QAudioFormat>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "decoder.h"
#include "equalizer.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace native {

// Something that happens at a point in the output stream: a track starts,
// a file fails, the queue runs out. The main thread acts on it once the
// sink has taken that frame.
struct Marker {
    enum Type { TrackStarted, Failed, EndOfQueue };
    Type type = TrackStarted;
    qint64 frame = 0;
    bool advance = false; // the queued track took over
    QString path, detail;
    QVariantMap meta;
    qint64 durationMs = 0;
    int rate = 0, channels = 0;
};

// Float stereo between the worker (writer) and the sink (reader), with
// absolute frame counters so positions and markers line up. A flush bumps
// the generation, and anything the worker had rendered for an older
// generation is dropped on arrival.
class Ring {
public:
    explicit Ring(int capacityFrames) : m_cap(capacityFrames), m_buf(size_t(capacityFrames) * 2) {}

    quint64 gen() {
        std::lock_guard l(m_mutex);
        return m_gen;
    }
    qint64 startFor(quint64 gen) {
        std::lock_guard l(m_mutex);
        return gen == m_gen ? m_w : -1;
    }
    void addMarker(const Marker &marker, quint64 gen) {
        std::lock_guard l(m_mutex);
        if (gen == m_gen)
            m_markers.push_back(marker);
    }

    // Blocks for space. Returns early on a flush, a wake() or quit.
    int write(const float *data, int frames, quint64 gen) {
        std::unique_lock l(m_mutex);
        int done = 0;
        while (done < frames) {
            m_cv.wait(l, [&] { return m_gen != gen || m_wake || m_quit || m_w - m_r < m_cap; });
            if (m_gen != gen || m_quit)
                break;
            if (m_wake) {
                m_wake = false;
                break;
            }
            const int n = int(std::min<qint64>(m_cap - (m_w - m_r), frames - done));
            for (int i = 0; i < n; ++i) {
                const size_t at = size_t((m_w + i) % m_cap) * 2;
                m_buf[at] = data[(done + i) * 2];
                m_buf[at + 1] = data[(done + i) * 2 + 1];
            }
            m_w += n;
            done += n;
        }
        return done;
    }

    qint64 read(float *out, qint64 frames, bool *jumped) {
        std::lock_guard l(m_mutex);
        *jumped = m_jumped;
        m_jumped = false;
        const qint64 n = std::min(frames, m_w - m_r);
        for (qint64 i = 0; i < n; ++i) {
            const size_t at = size_t((m_r + i) % m_cap) * 2;
            out[i * 2] = m_buf[at];
            out[i * 2 + 1] = m_buf[at + 1];
        }
        m_r += n;
        if (n > 0)
            m_cv.notify_all();
        return n;
    }

    qint64 consumed() {
        std::lock_guard l(m_mutex);
        return m_r;
    }
    qint64 available() {
        std::lock_guard l(m_mutex);
        return m_w - m_r;
    }

    // Drops everything unread and every marker not yet delivered. Returns
    // the frame where the new audio will start.
    qint64 flush(quint64 newGen) {
        std::lock_guard l(m_mutex);
        m_w = m_r;
        m_gen = newGen;
        m_jumped = true;
        m_markers.clear();
        m_cv.notify_all();
        return m_r;
    }

    std::vector<Marker> takeReached(qint64 upTo) {
        std::lock_guard l(m_mutex);
        std::vector<Marker> reached;
        while (!m_markers.empty() && m_markers.front().frame <= upTo) {
            reached.push_back(std::move(m_markers.front()));
            m_markers.pop_front();
        }
        return reached;
    }

    void wake() {
        std::lock_guard l(m_mutex);
        m_wake = true;
        m_cv.notify_all();
    }
    void quit() {
        std::lock_guard l(m_mutex);
        m_quit = true;
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    const qint64 m_cap;
    std::vector<float> m_buf;
    qint64 m_w = 0, m_r = 0;
    quint64 m_gen = 1;
    bool m_wake = false, m_quit = false, m_jumped = false;
    std::deque<Marker> m_markers;
};

// The sink's side: takes audio from the ring and applies the pause ramp and
// the volume, both smoothed per sample. After a jump (seek, skip, stop) or
// an underrun the last sample decays to zero instead of stepping, so there
// is no click.
class Reader : public PcmReader {
public:
    Reader(Ring *ring, int rate)
        : m_ring(ring), m_rampStep(1.0f / float(rate / 100)), m_volStep(1.0f / float(rate / 50)),
          m_decayFactor(float(std::exp(-1.0 / (0.0015 * rate)))), m_tap(size_t(kTap) * 2, 0.f) {}

    std::atomic<float> target{0}, volume{1};
    std::atomic<bool> muted{false}, silent{true};

    qint64 available() override { return m_ring->available(); }

    qint64 readPcm(float *out, qint64 frames) override {
        const float goal = target.load();
        qint64 got = 0;
        bool jumped = false;
        if (m_ramp > 0 || goal > 0) {
            qint64 want = frames;
            // Ramping down: take only what the ramp needs, so a pause
            // doesn't swallow audio the listener never hears.
            if (goal < m_ramp)
                want = std::min<qint64>(frames, qint64(std::ceil((m_ramp - goal) / m_rampStep)));
            got = m_ring->read(out, want, &jumped);
        } else {
            // Still drain a pending jump flag so it can't apply later.
            float none[2];
            m_ring->read(none, 0, &jumped);
        }
        if (jumped)
            m_decay[0] = m_last[0], m_decay[1] = m_last[1];
        storeTap(out, got);

        const float vol = muted.load() ? 0.f : volume.load();
        for (qint64 i = 0; i < got; ++i) {
            if (m_ramp < goal)
                m_ramp = std::min(goal, m_ramp + m_rampStep);
            else if (m_ramp > goal)
                m_ramp = std::max(goal, m_ramp - m_rampStep);
            if (m_vol < vol)
                m_vol = std::min(vol, m_vol + m_volStep);
            else if (m_vol > vol)
                m_vol = std::max(vol, m_vol - m_volStep);
            const float g = m_ramp * m_vol;
            for (int c = 0; c < 2; ++c) {
                float s = out[i * 2 + c] * g + m_decay[c];
                m_decay[c] *= m_decayFactor;
                out[i * 2 + c] = std::clamp(s, -1.f, 1.f);
            }
        }
        if (got > 0)
            m_last[0] = out[got * 2 - 2], m_last[1] = out[got * 2 - 1];
        if (got < frames) {
            // Underrun or paused: glide from the last sample to silence, and
            // if this is an underrun, fade the audio back in when it returns.
            m_decay[0] = m_last[0], m_decay[1] = m_last[1];
            if (goal > 0)
                m_ramp = 0;
            for (qint64 i = got; i < frames; ++i)
                for (int c = 0; c < 2; ++c) {
                    out[i * 2 + c] = m_decay[c];
                    m_decay[c] *= m_decayFactor;
                }
            m_last[0] = m_decay[0], m_last[1] = m_decay[1];
            if (std::abs(m_decay[0]) < 1e-6f && std::abs(m_decay[1]) < 1e-6f)
                m_decay[0] = m_decay[1] = m_last[0] = m_last[1] = 0;
        }
        silent = m_ramp <= 0 && goal <= 0;
        return got;
    }

    // The last frames read, before volume, for the analyser.
    QByteArray tap(int frames, qint64 *count) {
        std::lock_guard l(m_tapMutex);
        *count = m_tapCount;
        frames = int(std::min<qint64>({qint64(frames), m_tapCount, qint64(kTap)}));
        QByteArray bytes(frames * 2 * int(sizeof(float)), Qt::Uninitialized);
        auto *dst = reinterpret_cast<float *>(bytes.data());
        for (int i = 0; i < frames; ++i) {
            const size_t at = size_t((m_tapCount - frames + i) % kTap) * 2;
            dst[i * 2] = m_tap[at];
            dst[i * 2 + 1] = m_tap[at + 1];
        }
        return bytes;
    }

private:
    static constexpr int kTap = 2048;
    void storeTap(const float *data, qint64 frames) {
        std::lock_guard l(m_tapMutex);
        for (qint64 i = 0; i < frames; ++i) {
            const size_t at = size_t((m_tapCount + i) % kTap) * 2;
            m_tap[at] = data[i * 2];
            m_tap[at + 1] = data[i * 2 + 1];
        }
        m_tapCount += frames;
    }

    Ring *m_ring;
    const float m_rampStep, m_volStep, m_decayFactor;
    float m_ramp = 0, m_vol = 1;
    float m_decay[2] = {0, 0}, m_last[2] = {0, 0};
    std::mutex m_tapMutex;
    std::vector<float> m_tap;
    qint64 m_tapCount = 0;
};

// Float stereo from one rate to another.
class Resampler {
public:
    Resampler(int from, int to) : m_from(from) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&m_swr, &stereo, AV_SAMPLE_FMT_FLT, to, &stereo, AV_SAMPLE_FMT_FLT, from, 0, nullptr);
        swr_init(m_swr);
    }
    ~Resampler() { swr_free(&m_swr); }
    int from() const { return m_from; }

    void push(const float *in, int frames, std::vector<float> &out) {
        const int capacity = swr_get_out_samples(m_swr, frames);
        if (capacity <= 0)
            return;
        const size_t at = out.size();
        out.resize(at + size_t(capacity) * 2);
        auto *dst = reinterpret_cast<uint8_t *>(out.data() + at);
        const auto *src = reinterpret_cast<const uint8_t *>(in);
        const int got = swr_convert(m_swr, &dst, capacity, in ? &src : nullptr, frames);
        out.resize(at + size_t(std::max(got, 0)) * 2);
    }
    void flush(std::vector<float> &out) { push(nullptr, 0, out); }

private:
    SwrContext *m_swr = nullptr;
    int m_from;
};

// One track in the worker: its decoder, the resampler to the output rate,
// and what has been decoded but not yet mixed.
struct Source {
    QString path; // the queue's name for the track; never the URL
    Decoder dec;
    bool ok = false;
    QString error;
    // Set when the open is abandoned (the track was dequeued, or the
    // engine is closing), so a server that hangs can't hold anything up.
    std::shared_ptr<std::atomic<bool>> cancel;
    int outRate = 48000;
    std::unique_ptr<Resampler> rs;
    std::vector<float> fifo;
    size_t head = 0;
    bool outEnd = false, started = false;
    qint64 emitted = 0; // frames handed to the mixer: the track position
    float gain = 1;
    // The track that follows without a gap. At the end, this track's
    // resampler passes to it instead of being flushed, so the resampler's
    // filter runs straight across the join.
    Source *handoff = nullptr;

    // interrupt is asked, while the decoder waits on the network, whether
    // to give up (see Worker::interruptFor).
    static std::unique_ptr<Source> open(const QString &path, const QUrl &url, int outRate,
                                        std::shared_ptr<std::atomic<bool>> cancel,
                                        std::function<bool(const Source *)> interrupt) {
        auto s = std::make_unique<Source>();
        s->path = path;
        s->outRate = outRate;
        s->cancel = cancel ? std::move(cancel) : std::make_shared<std::atomic<bool>>(false);
        Source *raw = s.get();
        s->dec.setInterrupt([raw, interrupt] { return raw->cancel->load() || (interrupt && interrupt(raw)); });
        s->ok = s->dec.open(path, url, &s->error);
        if (s->ok) {
            if (s->dec.sampleRate() != outRate)
                s->rs = std::make_unique<Resampler>(s->dec.sampleRate(), outRate);
            s->dec.prime();
        }
        return s;
    }

    qint64 available() const { return qint64(fifo.size() - head) / 2; }
    qint64 estimatedTotal() const { return dec.durationMs() * outRate / 1000; }
    bool finished() const { return outEnd && available() == 0; }

    void fill(qint64 frames) {
        float tmp[4096 * 2];
        while (available() < frames && !outEnd) {
            if (head > 0 && head * 2 > fifo.size()) {
                fifo.erase(fifo.begin(), fifo.begin() + qsizetype(head));
                head = 0;
            }
            const int n = ok ? dec.read(tmp, 4096) : 0;
            if (n > 0) {
                if (rs)
                    rs->push(tmp, n, fifo);
                else
                    fifo.insert(fifo.end(), tmp, tmp + n * 2);
                continue;
            }
            if (rs) {
                if (handoff && handoff->ok && !handoff->started && handoff->dec.sampleRate() == dec.sampleRate())
                    handoff->rs = std::move(rs);
                else
                    rs->flush(fifo);
            }
            outEnd = true;
        }
    }

    int read(float *out, int frames) {
        fill(frames);
        started = true;
        const int n = int(std::min<qint64>(frames, available()));
        const float *src = fifo.data() + head;
        if (gain == 1.f)
            std::copy(src, src + n * 2, out);
        else
            for (int i = 0; i < n * 2; ++i)
                out[i] = src[i] * gain;
        head += size_t(n) * 2;
        emitted += n;
        return n;
    }

    void seek(qint64 ms) {
        if (!ok)
            return;
        dec.seek(ms);
        fifo.clear();
        head = 0;
        outEnd = false;
        started = false;
        rs.reset();
        if (dec.sampleRate() != outRate)
            rs = std::make_unique<Resampler>(dec.sampleRate(), outRate);
        emitted = ms * outRate / 1000;
    }
};

enum CommandType { Load, Queue, Seek, Stop, Configure, SkipFade, Quit };

struct Command {
    CommandType type = Quit;
    QString path;
    QUrl url;
    // SkipFade: the track the listener is on, and how far in.
    QString fromPath;
    qint64 ms = 0;
    quint64 gen = 0;
    AudioEngine::Settings settings;
};

// Whether a command abandons what the worker is doing (and so may cut a
// network wait short).
bool jumps(CommandType type) { return type == Load || type == Seek || type == Stop || type == SkipFade; }

// Decodes, joins, crossfades, levels and equalises, a chunk at a time,
// into the ring. Everything it owns is touched only on its own thread;
// the main thread talks to it with commands.
class Worker {
public:
    Worker(Ring *ring, int rate) : m_ring(ring), m_rate(rate) {
        m_eq.setSampleRate(rate);
        m_thread = std::thread([this] { run(); });
    }
    ~Worker() {
        // Every open or read still waiting on a server gives up now.
        m_quitting = true;
        Command quit;
        quit.type = Quit;
        post(quit);
        m_ring->quit();
        m_thread.join();
    }

    void post(const Command &command) {
        if (jumps(command.type))
            m_wanted = command.gen;
        {
            std::lock_guard l(m_mutex);
            m_commands.push_back(command);
        }
        m_cv.notify_all();
        m_ring->wake();
    }

private:
    static constexpr int kChunk = 1024;

    // Asked by a decoder while it waits on the network. On the worker's own
    // thread a waiting jump (a skip, seek or stop the listener asked for)
    // abandons the wait; an open running in the background gives up only
    // when it is dequeued (its cancel flag) or the engine closes.
    bool interruptFor(const Source *) const {
        if (m_quitting)
            return true;
        if (std::this_thread::get_id() != m_threadId.load())
            return false;
        return m_wanted.load() != m_active.load();
    }
    std::function<bool(const Source *)> interrupter() {
        return [this](const Source *s) { return interruptFor(s); };
    }
    void setGen(quint64 gen) {
        m_gen = gen;
        m_active = gen;
    }

    std::unique_ptr<Source> openNow(const QString &path, const QUrl &url) {
        return Source::open(path, url, m_rate, nullptr, interrupter());
    }
    // Opened on another thread: a slow disk, NAS or server must not stall
    // the audio that is playing now.
    void openPending(const QString &path, const QUrl &url) {
        m_pendingCancel = std::make_shared<std::atomic<bool>>(false);
        m_pendingPath = path;
        m_pending = std::async(std::launch::async, &Source::open, path, url, m_rate, m_pendingCancel, interrupter());
    }

    void run() {
        m_threadId = std::this_thread::get_id();
        std::vector<float> chunk(kChunk * 2);
        while (true) {
            if (!process())
                return;
            if (!m_cur || m_ended || m_ring->gen() != m_gen) {
                std::unique_lock l(m_mutex);
                m_cv.wait(l, [&] { return !m_commands.empty(); });
                continue;
            }
            std::vector<Marker> markers;
            const int n = render(chunk.data(), kChunk, markers);
            if (m_fadeIn > 0) {
                // New audio after a jump starts from silence.
                const int k = std::min(n, m_fadeIn);
                for (int i = 0; i < k; ++i) {
                    const float g = float(NativeEngine::kJumpFadeFrames - m_fadeIn + i + 1) /
                                    float(NativeEngine::kJumpFadeFrames);
                    chunk[i * 2] *= g;
                    chunk[i * 2 + 1] *= g;
                }
                m_fadeIn -= k;
            }
            m_eq.process(chunk.data(), n);
            const qint64 start = m_ring->startFor(m_gen);
            if (start < 0)
                continue;
            for (auto &m : markers) {
                m.frame += start;
                m_ring->addMarker(m, m_gen);
            }
            int done = 0;
            while (done < n) {
                done += m_ring->write(chunk.data() + done * 2, n - done, m_gen);
                if (done < n) {
                    if (m_ring->gen() != m_gen)
                        break;
                    // Woken for a command: one that doesn't jump (queue,
                    // settings) is applied and the chunk carries on.
                    if (!process())
                        return;
                    if (m_ring->gen() != m_gen)
                        break;
                }
            }
        }
    }

    bool process() {
        std::deque<Command> commands;
        {
            std::lock_guard l(m_mutex);
            commands.swap(m_commands);
        }
        for (const Command &c : commands) {
            switch (c.type) {
            case Quit: return false;
            case Load: load(c); break;
            case Queue: queue(c.path, c.url); break;
            case Seek:
            case Stop: seek(c); break;
            case SkipFade: skipFade(c); break;
            case Configure:
                m_settings = c.settings;
                m_eq.configure(m_settings.eqEnabled, m_settings.eqPreampDb, m_settings.eqBandsDb);
                for (Source *s : {m_cur.get(), m_next.get(), m_prev.get()})
                    if (s)
                        s->gain = gainFor(*s);
                break;
            }
        }
        return true;
    }

    void load(const Command &c) {
        setGen(c.gen);
        m_next.reset();
        m_prev.reset();
        dropPending();
        m_fading = false;
        m_skipFade = false;
        m_deferred.reset();
        m_ended = false;
        m_fadeIn = NativeEngine::kJumpFadeFrames;
        m_eq.reset();
        m_cur.reset();
        if (c.path.isEmpty())
            return;
        m_cur = openNow(c.path, c.url);
        const qint64 start = m_ring->startFor(m_gen);
        Marker m = m_cur->ok ? started(*m_cur, false) : failed(*m_cur, false);
        m.frame = std::max<qint64>(start, 0);
        if (m_cur->ok)
            m_cur->gain = gainFor(*m_cur);
        else
            m_ended = true;
        m_ring->addMarker(m, m_gen);
    }

    void queue(const QString &path, const QUrl &url) {
        // A crossfade under way has already committed to its track; what
        // follows that track is queued once the fade is done.
        if (m_fading || m_skipFade) {
            m_deferred = std::make_unique<std::pair<QString, QUrl>>(path, url);
            return;
        }
        if (path.isEmpty()) {
            m_next.reset();
            dropPending();
        } else if ((m_next && m_next->path == path) || m_pendingPath == path) {
            // The same track again (its URL may differ by a fresh salt).
            return;
        } else {
            m_next.reset();
            dropPending();
            openPending(path, url);
        }
    }

    void applyDeferred() {
        if (!m_deferred || m_fading || m_skipFade)
            return;
        const auto next = std::move(m_deferred);
        queue(next->first, next->second);
    }

    void seek(const Command &c) {
        setGen(c.gen);
        // Never waits for the next track's open: a pending track has made
        // no sound, so the listener can't be on it.
        resolveNext(false);
        if (m_cur && m_cur->path != c.path) {
            if (m_prev && m_prev->path == c.path) {
                // The listener is still on the previous track: the join was
                // decoded but not yet heard.
                dropPending();
                m_next = std::move(m_cur);
                m_next->seek(0);
                m_cur = std::move(m_prev);
            } else if (m_fading && m_next && m_next->path == c.path) {
                m_prev = std::move(m_cur);
                m_cur = std::move(m_next);
            }
        } else if (m_fading && m_next) {
            m_next->seek(0);
        }
        if (m_skipFade) {
            // A skip that was still connecting is abandoned by the seek.
            m_skipFade = false;
            dropPending();
            m_next.reset();
        }
        m_fading = false;
        if (m_cur && m_cur->ok) {
            m_cur->seek(c.type == Stop ? 0 : c.ms);
            m_cur->gain = gainFor(*m_cur);
            m_ended = false;
        }
        m_fadeIn = NativeEngine::kJumpFadeFrames;
        applyDeferred();
    }

    // A skip while playing with crossfade on: the track the listener is on
    // carries on from where they are and fades out under the new one,
    // which opens in the background (a stream may take a moment).
    void skipFade(const Command &c) {
        Command at = c;
        at.type = Seek;
        at.path = c.fromPath;
        seek(at);
        if (!m_cur || !m_cur->ok || m_ended || m_cur->path != c.fromPath) {
            Command load = c;
            load.type = Load;
            this->load(load);
            return;
        }
        m_next.reset();
        dropPending();
        m_deferred.reset();
        openPending(c.path, c.url);
        // A local file opens in a moment: give it that, so the fade starts
        // where the listener pressed. A stream that takes longer fades in
        // when it arrives, with the old track playing on meanwhile.
        m_pending.wait_for(std::chrono::milliseconds(100));
        m_skipFade = true;
    }

    void resolveNext(bool block) {
        if (!m_pending.valid())
            return;
        if (!block && m_pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        m_next = m_pending.get();
        m_pendingPath.clear();
        // The source now owns its flag; dropping it is resetting m_next.
        m_pendingCancel.reset();
        if (m_next->ok)
            m_next->gain = gainFor(*m_next);
    }

    void dropPending() {
        if (m_pendingCancel)
            *m_pendingCancel = true;
        m_pendingCancel.reset();
        if (m_pending.valid())
            m_graveyard.push_back(std::move(m_pending));
        m_pendingPath.clear();
        // Abandoned opens finish on their own; forget those that have.
        m_graveyard.erase(std::remove_if(m_graveyard.begin(), m_graveyard.end(),
                                         [](std::future<std::unique_ptr<Source>> &f) {
                                             return f.wait_for(std::chrono::seconds(0)) ==
                                                    std::future_status::ready;
                                         }),
                          m_graveyard.end());
    }

    float gainFor(const Source &s) const {
        using Mode = AudioEngine::ReplayGainMode;
        if (m_settings.replayGain == Mode::Off || !s.ok)
            return 1.f;
        const Decoder::ReplayGain rg = s.dec.replayGain();
        double gain = rg.trackGain, peak = rg.trackPeak;
        if (m_settings.replayGain == Mode::Album && !std::isnan(rg.albumGain)) {
            gain = rg.albumGain;
            peak = std::isnan(rg.albumPeak) ? rg.trackPeak : rg.albumPeak;
        }
        // Untagged files play as they are; the preamp is for tagged ones.
        if (std::isnan(gain))
            return 1.f;
        double linear = std::pow(10.0, (gain + m_settings.replayGainPreampDb) / 20.0);
        if (m_settings.clipPrevention && !std::isnan(peak) && peak > 0)
            linear = std::min(linear, 1.0 / peak);
        return float(linear);
    }

    static bool sameAlbum(const Source &a, const Source &b) {
        const QVariantMap ma = a.dec.metaData(), mb = b.dec.metaData();
        const QString album = ma.value("album").toString();
        return !album.isEmpty() && album.compare(mb.value("album").toString(), Qt::CaseInsensitive) == 0 &&
               ma.value("albumArtist").toString().compare(mb.value("albumArtist").toString(),
                                                          Qt::CaseInsensitive) == 0;
    }

    static Marker started(const Source &s, bool advance) {
        Marker m;
        m.type = Marker::TrackStarted;
        m.advance = advance;
        m.path = s.path;
        m.meta = s.dec.metaData();
        m.durationMs = s.dec.durationMs();
        m.rate = s.dec.sampleRate();
        m.channels = s.dec.sourceChannels();
        return m;
    }
    static Marker failed(const Source &s, bool advance, const QString &detail = {}) {
        Marker m;
        m.type = Marker::Failed;
        m.advance = advance;
        m.path = s.path;
        m.detail = detail.isEmpty() ? s.error : detail;
        return m;
    }

    // How long a skip's crossfade lasts: the crossfade setting, at most
    // half the new track.
    qint64 skipFadeFrames(const Source &next) const {
        const qint64 cf = qint64(m_settings.crossfadeMs) * m_rate / 1000;
        const qint64 total = next.estimatedTotal();
        return std::max<qint64>(1, total > 0 ? std::min(cf, total / 2) : cf);
    }

    int render(float *out, int frames, std::vector<Marker> &markers) {
        int produced = 0;
        auto mark = [&](Marker m) {
            m.frame = produced;
            markers.push_back(std::move(m));
        };
        while (produced < frames && m_cur && !m_ended) {
            if (m_skipFade) {
                // The skip's track is ready: fade over to it from here.
                resolveNext(false);
                if (m_next) {
                    m_skipFade = false;
                    if (!m_next->ok) {
                        mark(failed(*m_next, false));
                        m_prev = std::move(m_cur);
                        m_cur = std::move(m_next);
                        m_ended = true;
                        break;
                    }
                    m_fading = true;
                    m_fadeTotal = skipFadeFrames(*m_next);
                    m_fadePos = 0;
                    mark(started(*m_next, false));
                }
            } else {
                resolveNext(false);
            }
            const bool fadeWanted = !m_fading && !m_skipFade && m_settings.crossfadeMs > 0 && m_next &&
                                    m_next->ok && !sameAlbum(*m_cur, *m_next);
            m_cur->handoff =
                (m_next && m_next->ok && !fadeWanted && !m_fading && !m_skipFade) ? m_next.get() : nullptr;

            if (fadeWanted) {
                const qint64 cf = qint64(m_settings.crossfadeMs) * m_rate / 1000;
                // Near the end, decode the rest so the fade starts exactly
                // crossfade-length before the last sample.
                if (!m_cur->outEnd && m_cur->estimatedTotal() - m_cur->emitted < cf + 2 * m_rate)
                    m_cur->fill(qint64(m_rate) * 60);
                if (m_cur->outEnd) {
                    const qint64 rem = m_cur->available();
                    const qint64 total = m_cur->emitted + rem;
                    const qint64 nextTotal = m_next->estimatedTotal() > 0 ? m_next->estimatedTotal() : 2 * cf;
                    const qint64 len = std::min({cf, total / 2, nextTotal / 2});
                    if (len > 0 && rem <= len) {
                        m_fading = true;
                        m_fadeTotal = rem;
                        m_fadePos = 0;
                        mark(started(*m_next, true));
                    } else if (len > 0) {
                        produced += m_cur->read(out + produced * 2, int(std::min<qint64>(frames - produced, rem - len)));
                        continue;
                    }
                }
            }

            if (m_fading) {
                const int n = int(std::min<qint64>(frames - produced, m_fadeTotal - m_fadePos));
                if (n > 0) {
                    // The outgoing track may run out first (a skip's fade
                    // near its end); it is silent from there.
                    m_a.assign(size_t(n) * 2, 0.f);
                    m_b.assign(size_t(n) * 2, 0.f);
                    m_cur->read(m_a.data(), n);
                    m_next->read(m_b.data(), n);
                    float *dst = out + produced * 2;
                    for (int i = 0; i < n; ++i) {
                        // Equal power: the sum stays as loud as either track.
                        const double t = (double(m_fadePos + i) + 0.5) / double(m_fadeTotal);
                        const float ga = float(std::cos(t * M_PI_2)), gb = float(std::sin(t * M_PI_2));
                        dst[i * 2] = m_a[i * 2] * ga + m_b[i * 2] * gb;
                        dst[i * 2 + 1] = m_a[i * 2 + 1] * ga + m_b[i * 2 + 1] * gb;
                    }
                    m_fadePos += n;
                    produced += n;
                }
                if (m_fadePos >= m_fadeTotal) {
                    m_prev = std::move(m_cur);
                    m_cur = std::move(m_next);
                    m_fading = false;
                    applyDeferred();
                }
                continue;
            }

            produced += m_cur->read(out + produced * 2, frames - produced);
            if (m_cur->finished()) {
                // A stream that broke off is reported where its audio ends;
                // the Player then moves on to the next track.
                if (!m_cur->dec.readError().isEmpty()) {
                    mark(failed(*m_cur, false, m_cur->dec.readError()));
                    m_ended = true;
                    break;
                }
                resolveNext(true);
                if (!m_next) {
                    Marker end;
                    end.type = Marker::EndOfQueue;
                    mark(end);
                    m_ended = true;
                    break;
                }
                // A skip still opening when the old track ran out has
                // already been announced by the Player: no advance.
                const bool advance = !m_skipFade;
                m_skipFade = false;
                if (!m_next->ok) {
                    mark(failed(*m_next, advance));
                    m_prev = std::move(m_cur);
                    m_cur = std::move(m_next);
                    m_ended = true;
                    break;
                }
                mark(started(*m_next, advance));
                m_prev = std::move(m_cur);
                m_cur = std::move(m_next);
                applyDeferred();
            }
        }
        return produced;
    }

    Ring *m_ring;
    const int m_rate;
    std::thread m_thread;
    std::atomic<std::thread::id> m_threadId{};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Command> m_commands;
    // The generation the main thread has jumped to, and the one this
    // thread is working on; while they differ, network waits give up.
    std::atomic<quint64> m_wanted{1}, m_active{1};
    std::atomic<bool> m_quitting{false};

    quint64 m_gen = 1;
    AudioEngine::Settings m_settings;
    Equalizer m_eq;
    std::unique_ptr<Source> m_cur, m_next, m_prev;
    std::future<std::unique_ptr<Source>> m_pending;
    std::shared_ptr<std::atomic<bool>> m_pendingCancel;
    std::vector<std::future<std::unique_ptr<Source>>> m_graveyard;
    QString m_pendingPath;
    // A track queued while a fade was under way, for after it.
    std::unique_ptr<std::pair<QString, QUrl>> m_deferred;
    bool m_ended = false, m_fading = false;
    // A skip is waiting for its track to open before fading over to it.
    bool m_skipFade = false;
    qint64 m_fadeTotal = 0, m_fadePos = 0;
    int m_fadeIn = 0;
    std::vector<float> m_a, m_b;
};

} // namespace native

using namespace native;

NativeEngine::NativeEngine(PcmSink *sink, QObject *parent) : AudioEngine(parent) {
    if (!sink) {
        if (qEnvironmentVariableIsSet("OMAMP_NO_AUDIO_DEVICE")) {
            auto *buffer = new BufferSink(48000, false);
            buffer->setAutoPull(1.0);
            sink = buffer;
        } else {
            sink = new DeviceSink;
        }
    }
    m_sink.reset(sink);
    m_rate = m_sink->sampleRate();
    // Half a second between decoding and the device: enough to ride out a
    // slow disk, short enough that the equaliser responds quickly.
    m_ring = std::make_unique<Ring>(m_rate / 2);
    m_reader = std::make_unique<Reader>(m_ring.get(), m_rate);
    m_worker = std::make_unique<Worker>(m_ring.get(), m_rate);
    connect(&m_timer, &QTimer::timeout, this, &NativeEngine::poll);
    m_stall.start();
    updateTimer();
}

NativeEngine::~NativeEngine() {
    m_timer.stop();
    m_sink->stop();
    m_worker.reset();
}

void NativeEngine::post(int type, const QString &path, qint64 ms, const QUrl &url, const QString &fromPath) {
    Command c;
    c.type = CommandType(type);
    c.path = path;
    c.url = url;
    c.fromPath = fromPath;
    c.ms = ms;
    c.gen = m_gen;
    c.settings = m_settings;
    m_worker->post(c);
}

void NativeEngine::flush(bool deliverFirst) {
    // A seek or stop first delivers what the listener has already reached,
    // so the worker and the window agree on which track is current.
    if (deliverFirst)
        poll();
    ++m_gen;
    m_baseFrame = m_ring->flush(m_gen);
    m_lastConsumed = m_baseFrame;
    m_stall.restart();
}

void NativeEngine::setState(State state) {
    if (state == m_state)
        return;
    m_state = state;
    updateTimer();
    if (state != Playing)
        setBuffering(false);
    m_stall.restart();
    emit stateChanged();
}

void NativeEngine::setBuffering(bool on) {
    if (on == m_buffering)
        return;
    m_buffering = on;
    emit bufferingChanged();
}

void NativeEngine::updateTimer() {
    m_timer.start(m_state == Playing ? 20 : 100);
}

void NativeEngine::load(const QString &path, const QUrl &url) {
    m_reader->target = 0;
    setState(Stopped);
    m_source = path;
    m_meta.clear();
    m_durationMs = 0;
    flush(false);
    m_posBaseMs = 0;
    post(Load, path, 0, url);
    if (m_lastEmitted != 0) {
        m_lastEmitted = 0;
        emit positionChanged(0);
    }
}

void NativeEngine::queueNext(const QString &path, const QUrl &url) { post(Queue, path, 0, url); }

void NativeEngine::skipTo(const QString &path, const QUrl &url) {
    if (m_state != Playing || m_settings.crossfadeMs <= 0 || m_source.isEmpty() || path.isEmpty()) {
        load(path, url);
        play();
        return;
    }
    // Deliver what has been heard, so m_source is the track the listener
    // is on, then restart it from what the device has been given (not what
    // was heard: the device still plays its buffer) and fade out from
    // there under the new track.
    poll();
    const QString from = m_source;
    const qint64 played = qMax<qint64>(0, m_ring->consumed() - m_baseFrame);
    qint64 at = m_posBaseMs + played * 1000 / m_rate;
    if (m_durationMs > 0)
        at = qMin(at, m_durationMs);
    m_source = path;
    m_meta.clear();
    m_durationMs = 0;
    flush(false);
    m_posBaseMs = 0;
    post(SkipFade, path, at, url, from);
    m_lastEmitted = 0;
    emit positionChanged(0);
}

void NativeEngine::play() {
    if (m_source.isEmpty())
        return;
    if (!m_sink->started() && !m_sink->start(m_reader.get())) {
        emit errorOccurred(tr("Can't open the sound output"));
        return;
    }
    m_sink->resume();
    m_reader->target = 1;
    setState(Playing);
}

void NativeEngine::pause() {
    if (m_source.isEmpty())
        return;
    m_reader->target = 0;
    setState(Paused);
}

void NativeEngine::stop() {
    if (m_state == Stopped && position() == 0)
        return;
    m_reader->target = 0;
    flush(true);
    setState(Stopped);
    m_posBaseMs = 0;
    post(Stop, m_source);
    m_lastEmitted = 0;
    emit positionChanged(0);
}

void NativeEngine::seek(qint64 ms) {
    if (m_source.isEmpty())
        return;
    ms = qMax<qint64>(0, ms);
    if (m_durationMs > 0)
        ms = qMin(ms, m_durationMs);
    flush(true);
    m_posBaseMs = ms;
    post(Seek, m_source, ms);
    m_lastEmitted = ms;
    emit positionChanged(ms);
}

qint64 NativeEngine::heardFrames() const { return m_ring->consumed() - m_sink->queuedFrames(); }

qint64 NativeEngine::position() const {
    // What the listener hears: the device's buffer is taken off, so after a
    // jump the clock waits until the new audio reaches the speakers.
    const qint64 played = qMax<qint64>(0, heardFrames() - m_baseFrame);
    const qint64 ms = m_posBaseMs + played * 1000 / m_rate;
    return m_durationMs > 0 ? qMin(ms, m_durationMs) : ms;
}

qint64 NativeEngine::consumedFrames() const { return m_ring->consumed(); }

bool NativeEngine::ramping() const { return !m_reader->silent; }

void NativeEngine::setVolume(qreal linear) { m_reader->volume = float(qBound(0.0, linear, 1.0)); }

void NativeEngine::setMuted(bool muted) { m_reader->muted = muted; }

void NativeEngine::setSettings(const Settings &settings) {
    AudioEngine::setSettings(settings);
    m_settings.crossfadeMs = qBound(0, m_settings.crossfadeMs, 12000);
    post(Configure);
}

void NativeEngine::poll() {
    const qint64 consumed = m_ring->consumed();
    // Markers are acted on when their frame is heard, not when the device
    // takes it.
    for (Marker &m : m_ring->takeReached(consumed - m_sink->queuedFrames())) {
        switch (m.type) {
        case Marker::TrackStarted:
            m_source = m.path;
            m_meta = m.meta;
            m_durationMs = m.durationMs;
            m_posBaseMs = 0;
            m_baseFrame = m.frame;
            if (m.advance)
                emit advanced();
            emit durationChanged(m_durationMs);
            emit metaDataChanged();
            emit formatChanged(m.rate, m.channels);
            break;
        case Marker::Failed:
            if (m.advance) {
                m_source = m.path;
                m_meta.clear();
                m_durationMs = 0;
                m_posBaseMs = 0;
                m_baseFrame = m.frame;
                emit advanced();
            }
            m_reader->target = 0;
            setState(Stopped);
            emit trackFailed(m.path, AudioEngine::redact(m.detail));
            break;
        case Marker::EndOfQueue:
            m_reader->target = 0;
            ++m_gen;
            m_baseFrame = m_ring->flush(m_gen);
            m_posBaseMs = 0;
            post(Stop, m_source);
            setState(Stopped);
            m_lastEmitted = 0;
            emit positionChanged(0);
            emit endOfQueue();
            break;
        }
    }

    if (m_state == Playing) {
        // Playing with nothing to play for a moment: the decoder is waiting
        // on a stream (connecting, or a slow network). The sink pads with
        // silence meanwhile, and the window says "Buffering".
        const qint64 now_consumed = m_ring->consumed();
        if (now_consumed != m_lastConsumed || m_ring->available() > 0) {
            if (now_consumed != m_lastConsumed)
                m_lastConsumed = now_consumed;
            m_stall.restart();
            setBuffering(false);
        } else if (m_stall.isValid() && m_stall.elapsed() >= kBufferingAfterMs) {
            setBuffering(true);
        }
        const qint64 now = position();
        if (now != m_lastEmitted) {
            m_lastEmitted = now;
            emit positionChanged(now);
        }
        qint64 count = 0;
        const QByteArray bytes = m_reader->tap(1024, &count);
        if (count != m_tapSeen && !bytes.isEmpty()) {
            m_tapSeen = count;
            QAudioFormat format;
            format.setSampleFormat(QAudioFormat::Float);
            format.setSampleRate(m_rate);
            format.setChannelCount(2);
            emit pcmTapped(QAudioBuffer(bytes, format));
        }
    } else if (m_reader->silent && m_sink->started()) {
        // The pause ramp has finished: let the device rest.
        m_sink->suspend();
    }
}
