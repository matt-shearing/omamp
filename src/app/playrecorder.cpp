#include "playrecorder.h"

#include "http.h"
#include "library.h"
#include "player.h"
#include "scrobble.h"
#include "stats.h"

#include <QFileInfo>

PlayRecorder::PlayRecorder(const QString &databasePath, QObject *parent)
    : QObject(parent), m_path(databasePath.isEmpty() ? Library::defaultPath() : databasePath) {}

PlayRecorder::~PlayRecorder() {
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    if (m_flushThread) {
        m_flushThread->wait();
        delete m_flushThread;
    }
}

void PlayRecorder::attach(Player *player) {
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (!player)
        return;
    connect(player, &Player::playStarted, this, &PlayRecorder::started);
    connect(player, &Player::playProgress, this, &PlayRecorder::progress);
    connect(player, &Player::playFinished, this, &PlayRecorder::finished);
}

Library *PlayRecorder::library() {
    if (m_library)
        return m_library.get();
    // A deck-only user has no library, and playing music doesn't make one.
    if (!QFileInfo::exists(m_path))
        return nullptr;
    auto library = std::make_unique<Library>(m_path);
    if (!library->open())
        return nullptr;
    m_library = std::move(library);
    m_scrobbler = std::make_unique<Scrobble::Scrobbler>(*m_library);
    return m_library.get();
}

void PlayRecorder::started(const QString &path) {
    m_playPath = path;
    m_trackId = 0;
    Library *lib = library();
    if (!lib)
        return;
    m_trackId = Stats::trackIdForPath(*lib, path);
    if (m_trackId)
        m_scrobbler->trackStarted(m_trackId);
}

void PlayRecorder::progress(const QString &path, qint64 heardMs) {
    if (path == m_playPath && m_trackId && m_scrobbler)
        m_scrobbler->trackProgress(m_trackId, heardMs);
}

void PlayRecorder::finished(const QString &path, qint64 heardMs, qint64 durationMs, bool userSkipped) {
    Library *lib = library();
    if (!lib)
        return;
    const qint64 id = path == m_playPath && m_trackId ? m_trackId : Stats::trackIdForPath(*lib, path);
    m_playPath.clear();
    m_trackId = 0;
    if (!id)
        return;
    const Stats::PlayResult result = Stats::recordPlay(*lib, id, heardMs, durationMs, userSkipped);
    if (!result.ok())
        qWarning("Can't record a play: %s", qPrintable(result.error));
    const int queued = m_scrobbler->trackFinished(id, heardMs);
    emit recorded(id, result.outcomeName());
    if (queued > 0)
        startFlush();
}

void PlayRecorder::flushIfConfigured() {
    if (library() && !m_scrobbler->enabledServices().isEmpty())
        startFlush();
}

void PlayRecorder::startFlush() {
    if (!m_flushing || !m_scrobbler || m_scrobbler->enabledServices().isEmpty())
        return;
    if (m_flushThread) {
        m_flushAgain = true;
        return;
    }
    const QString path = m_path;
    m_flushThread = QThread::create([this, path] {
        // Its own connection and HTTP client, made on this thread.
        Library library(path);
        int sent = 0, failed = 0;
        if (library.open()) {
            Net::HttpClient http(&library);
            Scrobble::Scrobbler scrobbler(library, &http);
            const Scrobble::FlushResult r = scrobbler.flush();
            sent = r.sent;
            failed = r.failed;
        }
        QMetaObject::invokeMethod(
            this,
            [this, sent, failed] {
                if (m_flushThread) {
                    m_flushThread->wait();
                    delete m_flushThread;
                    m_flushThread = nullptr;
                }
                emit flushed(sent, failed);
                if (m_flushAgain) {
                    m_flushAgain = false;
                    startFlush();
                }
            },
            Qt::QueuedConnection);
    });
    m_flushThread->setObjectName("thrum-scrobble");
    m_flushThread->start(QThread::LowPriority);
}
