#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <memory>

class Library;
class Player;
namespace Scrobble {
class Scrobbler;
}

// Counts what the player plays: play counts, skips and the play history
// (Stats::recordPlay), and the scrobble queue (Scrobble::Scrobbler).
//
// It keeps its own connection to the library, opened the first time a play
// starts, and only when the library file already exists, so the deck alone
// never makes a library. The queue is sent on a worker thread after each
// counted play and once at start-up, and only when a scrobbling service has
// credentials; nothing touches the network otherwise.
class PlayRecorder : public QObject {
    Q_OBJECT
public:
    explicit PlayRecorder(const QString &databasePath, QObject *parent = nullptr);
    ~PlayRecorder() override;

    void attach(Player *player);
    // false: never send the scrobble queue (tests).
    void setFlushing(bool on) { m_flushing = on; }
    // Sends what is queued, if a service is configured; for start-up.
    void flushIfConfigured();
    bool flushRunning() const { return m_flushThread != nullptr; }

signals:
    // A play was written: outcome is counted, skipped or ignored.
    void recorded(qint64 trackId, const QString &outcome);
    void flushed(int sent, int failed);

private:
    Library *library();
    void started(const QString &path);
    void progress(const QString &path, qint64 heardMs);
    void finished(const QString &path, qint64 heardMs, qint64 durationMs, bool userSkipped);
    void startFlush();

    QString m_path;
    std::unique_ptr<Library> m_library;
    std::unique_ptr<Scrobble::Scrobbler> m_scrobbler;
    QPointer<Player> m_player;
    QString m_playPath;
    qint64 m_trackId = 0;
    bool m_flushing = true, m_flushAgain = false;
    QThread *m_flushThread = nullptr;
};
