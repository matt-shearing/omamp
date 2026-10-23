#include "spotifyaudio.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::mutex g_installMutex;
std::shared_ptr<SpotifyAudio> g_installed;

QString tr(const char *text) { return QObject::tr(text); }

SpotifyAudio::Answer failure(const QString &error) {
    SpotifyAudio::Answer a;
    a.header = {{"ok", false}, {"error", error}};
    return a;
}

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

bool readExact(int fd, char *to, size_t size) {
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::read(fd, to + done, size - done);
        if (n > 0)
            done += size_t(n);
        else if (n < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return true;
}

// How long a failed start is remembered, so a missing sign-in doesn't start
// the sidecar again for every track of a list.
constexpr qint64 kFailureMemoryMs = 10000;
// The largest frame believed: a read of 16384 frames and its header.
constexpr quint32 kMaxFrameBytes = 16 * 1024 * 1024;

} // namespace

SpotifyAudio::SpotifyAudio(const QString &program, const QString &credentials)
    : m_program(program), m_credentials(credentials) {}

SpotifyAudio::~SpotifyAudio() { shutdown(); }

std::shared_ptr<SpotifyAudio> SpotifyAudio::installed() {
    std::lock_guard l(g_installMutex);
    return g_installed;
}

void SpotifyAudio::install(std::shared_ptr<SpotifyAudio> audio) {
    std::lock_guard l(g_installMutex);
    g_installed = std::move(audio);
}

QString SpotifyAudio::trackId(const QString &path) {
    static const QLatin1String prefix("spotify://track/");
    if (!path.startsWith(prefix))
        return {};
    QString id = path.mid(prefix.size());
    const qsizetype query = id.indexOf('?');
    if (query >= 0)
        id.truncate(query);
    return id;
}

bool SpotifyAudio::canPlay(const QString &path) {
    if (trackId(path).isEmpty())
        return false;
    const auto audio = installed();
    return audio && audio->enabled();
}

void SpotifyAudio::setEnabled(bool on) { m_enabled = on; }

qint64 SpotifyAudio::newStream() {
    std::lock_guard l(m_mutex);
    return m_nextStream++;
}

quint64 SpotifyAudio::generation() const {
    std::lock_guard l(m_mutex);
    return m_generation;
}

int SpotifyAudio::starts() const {
    std::lock_guard l(m_mutex);
    return m_starts;
}

pid_t SpotifyAudio::pid() const {
    std::lock_guard l(m_mutex);
    return m_pid;
}

bool SpotifyAudio::sendLine(const QByteArray &line) {
    std::lock_guard w(m_writeMutex);
    if (m_toChild < 0)
        return false;
    qsizetype done = 0;
    while (done < line.size()) {
        const ssize_t n = ::send(m_toChild, line.constData() + done, size_t(line.size() - done), MSG_NOSIGNAL);
        if (n > 0)
            done += n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return true;
}

// The reader thread: starts the sidecar, reads its frames until it closes
// its output, then reaps it. The sidecar is this thread's child, and
// PR_SET_PDEATHSIG ties it to this thread, which lives exactly as long.
void SpotifyAudio::run(std::vector<std::string> args) {
    int toChild[2] = {-1, -1}, fromChild[2] = {-1, -1};
    const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    bool made = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, toChild) == 0 &&
                ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fromChild) == 0 && devnull >= 0;
    std::vector<char *> argv;
    for (std::string &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t parent = ::getpid();
    pid_t pid = made ? ::fork() : -1;
    if (pid == 0) {
        // Only async-signal-safe calls until exec.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent)
            ::_exit(127);
        ::dup2(toChild[1], 0);
        ::dup2(fromChild[1], 1);
        ::dup2(devnull, 2);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    for (int fd : {toChild[1], fromChild[1], devnull})
        if (fd >= 0)
            ::close(fd);
    if (pid < 0) {
        for (int fd : {toChild[0], fromChild[0]})
            if (fd >= 0)
                ::close(fd);
        std::lock_guard l(m_mutex);
        m_failure = tr("Couldn't start the Spotify player.");
        m_starting = false;
        m_cv.notify_all();
        return;
    }
    {
        std::lock_guard w(m_writeMutex);
        m_toChild = toChild[0];
    }
    {
        std::lock_guard l(m_mutex);
        m_pid = pid;
        m_running = true;
        // shutdown() came while it was starting.
        if (m_closed)
            ::kill(pid, SIGKILL);
        m_cv.notify_all();
    }

    const int in = fromChild[0];
    while (true) {
        quint32 size = 0;
        if (!readExact(in, reinterpret_cast<char *>(&size), sizeof size) || size > kMaxFrameBytes)
            break;
        QByteArray body(qsizetype(size), Qt::Uninitialized);
        if (!readExact(in, body.data(), size))
            break;
        const qsizetype nl = body.indexOf('\n');
        Answer answer;
        answer.header = QJsonDocument::fromJson(nl >= 0 ? body.left(nl) : body).object();
        if (nl >= 0)
            answer.data = body.mid(nl + 1);
        std::lock_guard l(m_mutex);
        if (!m_ready && answer.header.contains(QStringLiteral("ready"))) {
            if (answer.header.value(QStringLiteral("ready")).toBool())
                m_ready = true;
            else
                m_failure = answer.header.value(QStringLiteral("error")).toString(tr("Couldn't sign in to Spotify."));
            m_starting = false;
            m_cv.notify_all();
            continue;
        }
        const qint64 id = answer.header.value(QStringLiteral("id")).toInteger(-1);
        // An answer nobody waits for any more (an abandoned wait) is dropped.
        if (m_waiting.count(id)) {
            m_answers[id] = std::move(answer);
            m_cv.notify_all();
        }
    }
    ::close(in);
    {
        std::lock_guard w(m_writeMutex);
        ::close(m_toChild);
        m_toChild = -1;
    }
    {
        std::lock_guard l(m_mutex);
        if (m_starting && m_failure.isEmpty())
            m_failure = tr("The Spotify player stopped while signing in.");
        m_starting = false;
        m_running = false;
        m_ready = false;
        m_cv.notify_all();
    }
    // It closed its output: it is leaving. A moment to go, then a kill.
    int status = 0;
    pid_t gone = 0;
    for (int i = 0; i < 100 && (gone = ::waitpid(pid, &status, WNOHANG)) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (gone == 0) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
    std::lock_guard l(m_mutex);
    m_pid = 0;
    m_cv.notify_all();
}

bool SpotifyAudio::ensureStarted(std::unique_lock<std::mutex> &lock, const Interrupt &interrupt, QString *error) {
    if (m_closed) {
        *error = tr("Thrum is closing.");
        return false;
    }
    if (m_running && m_ready)
        return true;
    if (!m_starting) {
        if (!m_failure.isEmpty() && now() - m_failedAt < kFailureMemoryMs) {
            *error = m_failure;
            return false;
        }
        // The last sidecar's reader finishes once it is reaped.
        std::thread old = std::move(m_reader);
        if (old.joinable()) {
            lock.unlock();
            old.join();
            lock.lock();
        }
        if (m_closed || m_starting || (m_running && m_ready))
            return ensureStarted(lock, interrupt, error);
        m_starting = true;
        m_failure.clear();
        ++m_starts;
        ++m_generation;
        std::vector<std::string> args = {m_program.toStdString(), "--credentials", m_credentials.toStdString(),
                                         "audio"};
        m_reader = std::thread(&SpotifyAudio::run, this, std::move(args));
    }
    // Signing in takes about a second.
    const qint64 deadline = now() + 30000;
    while (m_starting) {
        if (interrupt && interrupt()) {
            *error = tr("interrupted");
            return false;
        }
        if (now() >= deadline) {
            m_failure = tr("The Spotify player didn't sign in to Spotify in time.");
            if (m_pid > 0)
                ::kill(m_pid, SIGKILL);
            break;
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (m_running && m_ready)
        return true;
    if (m_failure.isEmpty())
        m_failure = tr("Couldn't start the Spotify player.");
    m_failedAt = now();
    *error = m_failure;
    return false;
}

SpotifyAudio::Answer SpotifyAudio::ask(QJsonObject request, int timeoutMs, const Interrupt &interrupt) {
    std::unique_lock lock(m_mutex);
    QString error;
    if (!ensureStarted(lock, interrupt, &error))
        return failure(error);
    const qint64 id = m_nextId++;
    request.insert(QStringLiteral("id"), id);
    m_waiting.insert(id);
    lock.unlock();
    const bool sent = sendLine(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    lock.lock();
    Answer answer = failure(tr("The Spotify player stopped."));
    if (sent) {
        const qint64 deadline = now() + timeoutMs;
        while (true) {
            auto it = m_answers.find(id);
            if (it != m_answers.end()) {
                answer = std::move(it->second);
                m_answers.erase(it);
                break;
            }
            if (!m_running)
                break;
            if (interrupt && interrupt()) {
                answer = failure(tr("interrupted"));
                break;
            }
            if (now() >= deadline) {
                answer = failure(tr("Spotify didn't answer in time."));
                break;
            }
            m_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    }
    m_waiting.erase(id);
    m_answers.erase(id);
    return answer;
}

void SpotifyAudio::tell(QJsonObject request) {
    {
        std::lock_guard l(m_mutex);
        if (!m_running || !m_ready)
            return;
        request.insert(QStringLiteral("id"), m_nextId++);
    }
    sendLine(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void SpotifyAudio::shutdown() {
    std::unique_lock lock(m_mutex);
    m_closed = true;
    if (m_running) {
        lock.unlock();
        sendLine("{\"cmd\":\"quit\"}\n");
        {
            std::lock_guard w(m_writeMutex);
            if (m_toChild >= 0)
                ::shutdown(m_toChild, SHUT_WR);
        }
        lock.lock();
        const qint64 deadline = now() + 1500;
        while (m_running && now() < deadline)
            m_cv.wait_for(lock, std::chrono::milliseconds(20));
        if (m_running && m_pid > 0)
            ::kill(m_pid, SIGKILL);
    } else if (m_starting && m_pid > 0) {
        ::kill(m_pid, SIGKILL);
    }
    std::thread reader = std::move(m_reader);
    lock.unlock();
    if (reader.joinable())
        reader.join();
}

// --- SpotifyStream ----------------------------------------------------------------

SpotifyStream::SpotifyStream(std::shared_ptr<SpotifyAudio> audio) : m_audio(std::move(audio)) {}

SpotifyStream::~SpotifyStream() {
    if (m_open && m_audio->generation() == m_generation)
        m_audio->tell({{"cmd", "close"}, {"stream", m_stream}});
}

bool SpotifyStream::open(const QString &path, const SpotifyAudio::Interrupt &interrupt, QString *error) {
    m_interrupt = interrupt;
    const QString id = SpotifyAudio::trackId(path);
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (id.isEmpty())
        return fail(QObject::tr("not a Spotify track"));
    if (!m_audio->enabled())
        return fail(QObject::tr("Spotify is off. Switch it on in Settings → Spotify."));
    m_stream = m_audio->newStream();
    const SpotifyAudio::Answer a = m_audio->ask(
        {{"cmd", "open"}, {"stream", m_stream}, {"track", id}, {"position_ms", 0}}, 30000, m_interrupt);
    if (!a.ok()) {
        const QString reason = a.header.value("reason").toString();
        if (reason == QLatin1String("premium"))
            return fail(QObject::tr("Playing Spotify in Thrum needs Spotify Premium."));
        if (reason == QLatin1String("unavailable"))
            return fail(QObject::tr("This track isn't available on Spotify here."));
        return fail(a.error());
    }
    m_generation = m_audio->generation();
    m_open = true;
    const QJsonObject &h = a.header;
    m_rate = h.value("rate").toInt(44100);
    m_durationMs = h.value("duration_ms").toInteger();
    m_bitrate = h.value("bitrate").toInt();
    QStringList artists;
    for (const QJsonValue &v : h.value("artists").toArray())
        artists << v.toString();
    QStringList albumArtists;
    for (const QJsonValue &v : h.value("album_artists").toArray())
        albumArtists << v.toString();
    m_meta = {{"title", h.value("title").toString()},
              {"artist", artists.join(QStringLiteral(", "))},
              {"albumArtist", albumArtists.join(QStringLiteral(", "))},
              {"album", h.value("album").toString()},
              {"bitrate", m_bitrate}};
    auto number = [&](const char *key) { return h.contains(key) ? h.value(key).toDouble() : qQNaN(); };
    trackGain = number("track_gain_db");
    trackPeak = number("track_peak");
    albumGain = number("album_gain_db");
    albumPeak = number("album_peak");
    return true;
}

bool SpotifyStream::fetch() {
    if (!m_open || m_end)
        return false;
    if (m_audio->generation() != m_generation) {
        m_readError = QObject::tr("The Spotify player stopped.");
        m_end = true;
        return false;
    }
    const SpotifyAudio::Answer a =
        m_audio->ask({{"cmd", "read"}, {"stream", m_stream}, {"frames", chunkFrames}}, 20000, m_interrupt);
    if (!a.ok()) {
        // An interrupted read leaves the track as it was: a seek or a skip
        // follows. Anything else ends it.
        if (!(m_interrupt && m_interrupt())) {
            m_readError = a.error();
            m_end = true;
        }
        return false;
    }
    const qsizetype frames = qMin<qsizetype>(a.header.value("frames").toInteger(), a.data.size() / qsizetype(2 * sizeof(float)));
    if (m_head > 0 && m_head == m_buffer.size()) {
        m_buffer.clear();
        m_head = 0;
    }
    const size_t at = m_buffer.size();
    m_buffer.resize(at + size_t(frames) * 2);
    std::memcpy(m_buffer.data() + at, a.data.constData(), size_t(frames) * 2 * sizeof(float));
    if (a.header.value("end").toBool())
        m_end = true;
    return frames > 0;
}

int SpotifyStream::read(float *out, int frames) {
    int done = 0;
    while (done < frames) {
        const size_t available = (m_buffer.size() - m_head) / 2;
        if (available == 0) {
            if (!fetch() && (m_buffer.size() - m_head) / 2 == 0)
                break;
            continue;
        }
        const int take = int(std::min<size_t>(available, size_t(frames - done)));
        std::copy(m_buffer.data() + m_head, m_buffer.data() + m_head + size_t(take) * 2, out + size_t(done) * 2);
        m_head += size_t(take) * 2;
        done += take;
    }
    if (m_head == m_buffer.size()) {
        m_buffer.clear();
        m_head = 0;
    }
    return done;
}

void SpotifyStream::prime() {
    if (m_buffer.size() == m_head && !m_end)
        fetch();
}

bool SpotifyStream::seek(qint64 ms) {
    if (!m_open || m_audio->generation() != m_generation)
        return false;
    const SpotifyAudio::Answer a = m_audio->ask(
        {{"cmd", "seek"}, {"stream", m_stream}, {"position_ms", qMax<qint64>(0, ms)}}, 20000, m_interrupt);
    if (!a.ok())
        return false;
    m_buffer.clear();
    m_head = 0;
    m_end = false;
    m_readError.clear();
    return true;
}
