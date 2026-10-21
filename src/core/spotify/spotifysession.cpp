#include "spotifysession.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace Spotify {

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Spotify", text); }

QJsonObject failure(const QString &error) { return {{"ok", false}, {"error", error}}; }

} // namespace

SessionReader::SessionReader(const QString &program, const QString &credentials)
    : m_program(program), m_credentials(credentials) {
    m_thread.setObjectName("spotify-session");
    m_context = new QObject;
    m_context->moveToThread(&m_thread);
    m_thread.start();
}

SessionReader::~SessionReader() {
    // The process belongs to the reader's thread, so it is stopped there.
    QMetaObject::invokeMethod(
        m_context,
        [this] {
            if (m_process) {
                m_process->write("{\"cmd\":\"quit\"}\n");
                m_process->closeWriteChannel();
                if (!m_process->waitForFinished(1500)) {
                    m_process->kill();
                    m_process->waitForFinished(1000);
                }
                delete m_process;
                m_process = nullptr;
            }
        },
        Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_context;
}

QString SessionReader::findProgram() {
    static const char *name = "thrum-spotify-session";
    if (qEnvironmentVariableIsSet("THRUM_SPOTIFY_SESSION")) {
        const QString env = qEnvironmentVariable("THRUM_SPOTIFY_SESSION");
        if (env.isEmpty() || env == "off")
            return {};
        if (QFileInfo(env).isAbsolute())
            return QFileInfo(env).isExecutable() ? env : QString();
        return QStandardPaths::findExecutable(env);
    }
    // Beside Thrum first (the package and bin/install-user put it there),
    // so a stray copy elsewhere doesn't win.
    if (QCoreApplication::instance()) {
        const QString beside = QCoreApplication::applicationDirPath() + '/' + name;
        if (QFileInfo(beside).isExecutable())
            return beside;
    }
    return QStandardPaths::findExecutable(name);
}

QJsonObject SessionReader::ask(const QJsonObject &request, int timeoutMs) {
    QMutexLocker lock(&m_mutex);
    const CancelFlag cancel = CancelScope::current();
    QJsonObject answer;
    QMetaObject::invokeMethod(
        m_context, [&] { answer = askHere(request, timeoutMs, cancel); }, Qt::BlockingQueuedConnection);
    return answer;
}

bool SessionReader::readLine(QByteArray *line, qint64 deadline, const CancelFlag &cancel) {
    while (!m_process->canReadLine()) {
        if (m_process->state() != QProcess::Running) {
            m_process->waitForReadyRead(0);
            if (!m_process->canReadLine())
                return false;
            break;
        }
        if ((cancel && cancel->load()) || QDateTime::currentMSecsSinceEpoch() >= deadline)
            return false;
        m_process->waitForReadyRead(100);
    }
    *line = m_process->readLine().trimmed();
    return true;
}

bool SessionReader::ensureStarted(QString *error) {
    if (m_process && m_process->state() == QProcess::Running)
        return true;
    delete m_process;
    m_process = new QProcess;
    m_process->setProgram(m_program);
    m_process->setArguments({"--credentials", m_credentials, "serve"});
    // Its log (librespot's) isn't wanted; its answers are on stdout.
    m_process->setStandardErrorFile(QProcess::nullDevice());
    m_process->start();
    ++m_starts;
    if (!m_process->waitForStarted(5000)) {
        *error = tr("Couldn't start %1.").arg(QFileInfo(m_program).fileName());
        return false;
    }
    QByteArray line;
    // Signing in takes about a second.
    if (!readLine(&line, QDateTime::currentMSecsSinceEpoch() + 25000, {})) {
        *error = tr("%1 didn't sign in to Spotify in time.").arg(QFileInfo(m_program).fileName());
        m_process->kill();
        m_process->waitForFinished(1000);
        return false;
    }
    const QJsonObject ready = QJsonDocument::fromJson(line).object();
    if (!ready.value("ready").toBool()) {
        *error = ready.value("error").toString(tr("%1 couldn't sign in to Spotify.").arg(QFileInfo(m_program).fileName()));
        m_process->waitForFinished(1000);
        return false;
    }
    return true;
}

QJsonObject SessionReader::askHere(const QJsonObject &request, int timeoutMs, const CancelFlag &cancel) {
    if (m_program.isEmpty())
        return failure(tr("thrum-spotify-session isn't installed."));
    QString error;
    if (!ensureStarted(&error))
        return failure(error);
    QJsonObject r = request;
    const int id = m_next++;
    r.insert("id", id);
    m_process->write(QJsonDocument(r).toJson(QJsonDocument::Compact) + '\n');
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    QByteArray line;
    while (readLine(&line, deadline, cancel)) {
        const QJsonObject answer = QJsonDocument::fromJson(line).object();
        // An answer to a request given up on earlier is passed over.
        if (answer.value("id").toInt() == id)
            return answer;
    }
    if (cancel && cancel->load())
        return failure(tr("cancelled"));
    if (m_process->state() != QProcess::Running)
        return failure(tr("thrum-spotify-session stopped."));
    return failure(tr("thrum-spotify-session didn't answer in time."));
}

// --- The catalogue --------------------------------------------------------------

SessionCatalogue::SessionCatalogue(Backend *backend, std::shared_ptr<SessionReader> reader)
    : m_web(backend), m_reader(std::move(reader)) {}

namespace {

QList<Item> tracksIn(const QJsonObject &answer) {
    QList<Item> out;
    for (const QJsonValue &v : answer.value("tracks").toArray()) {
        const Item t = parseTrack(v.toObject());
        if (t.isValid())
            out << t;
    }
    return out;
}

} // namespace

QList<Item> SessionCatalogue::playlistTracks(const Item &playlist, QString *error) {
    const QJsonObject answer = m_reader->ask({{"cmd", "playlist"}, {"playlist", playlist.id}},
                                             Backend::longTimeoutMs());
    if (answer.value("ok").toBool())
        return tracksIn(answer);
    if (CancelScope::cancelled()) {
        if (error)
            *error = tr("cancelled");
        return {};
    }
    const int status = answer.value("status").toInt();
    // Spotify not having the playlist, or not letting you see it, is final.
    if (status == 404 || status == 403) {
        if (error)
            *error = status == 404 ? tr("Spotify has no such playlist.") : tr("Spotify doesn't let you open this playlist.");
        return {};
    }
    // Anything else: the Web API, unless that would only wait on the shared
    // client ID (Spotify's own playlists).
    if (playlist.spotifyMade()) {
        if (error)
            *error = tr("Couldn't read this playlist the way Spotify's own app does: %1")
                         .arg(answer.value("error").toString());
        return {};
    }
    return m_web.playlistTracks(playlist, error);
}

QList<Item> SessionCatalogue::artistPage(const Item &given, QStringList *sections, QString *error) {
    Item artist = given;
    const QJsonObject answer = m_reader->ask({{"cmd", "artist-top"}, {"artist", artist.id}}, Backend::longTimeoutMs());
    const QList<Item> top = answer.value("ok").toBool() ? tracksIn(answer) : QList<Item>();
    if (artist.name.isEmpty())
        artist.name = answer.value("artist").toObject().value("name").toString();
    // The albums (and, without the session, the tracks) from search.
    QStringList searchSections;
    QString searchError;
    const QList<Item> searched = m_web.artistPage(artist, &searchSections, &searchError);
    if (top.isEmpty() && searched.isEmpty()) {
        if (error)
            *error = !searchError.isEmpty() ? searchError : answer.value("error").toString();
        return {};
    }
    QList<Item> items;
    QStringList heads;
    if (!top.isEmpty()) {
        for (const Item &t : top) {
            items << t;
            heads << tr("Popular tracks");
        }
    }
    for (int i = 0; i < searched.size(); ++i) {
        const bool track = searched.at(i).kind == "track";
        if (track && !top.isEmpty())
            continue;
        items << searched.at(i);
        heads << searchSections.value(i);
    }
    if (sections)
        *sections = heads;
    return items;
}

} // namespace Spotify
