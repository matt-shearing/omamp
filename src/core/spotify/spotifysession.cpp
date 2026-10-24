#include "spotifysession.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace Spotify {

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Spotify", text); }

QJsonObject failure(const QString &error) { return {{"ok", false}, {"error", error}}; }

} // namespace

SessionReader::SessionReader(std::shared_ptr<SpotifyAudio> audio) : m_audio(std::move(audio)) {}

QString SessionReader::credentialsPath() { return SpotifyPlayerBackend::cacheFolder() + "/credentials.json"; }

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
    const CancelFlag cancel = CancelScope::current();
    const SpotifyAudio::Answer a =
        m_audio->ask(request, timeoutMs, [cancel] { return cancel && cancel->load(); });
    if (!a.ok() && cancel && cancel->load())
        return failure(tr("cancelled"));
    return a.header;
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

QList<Item> SessionCatalogue::albumTracks(const Item &album, QString *error) {
    const QJsonObject answer = m_reader->ask({{"cmd", "album"}, {"album", album.id}}, Backend::longTimeoutMs());
    if (answer.value("ok").toBool()) {
        QList<Item> tracks = tracksIn(answer);
        for (Item &t : tracks)
            if (t.album.isEmpty())
                t.album = album.name;
        return tracks;
    }
    if (CancelScope::cancelled()) {
        if (error)
            *error = tr("cancelled");
        return {};
    }
    return m_web.albumTracks(album, error);
}

bool SessionCatalogue::like(const QStringList &trackIds, bool on, QString *error) {
    QJsonArray ids;
    for (const QString &id : trackIds)
        ids << id;
    const QJsonObject answer = m_reader->ask({{"cmd", "like"}, {"tracks", ids}, {"on", on}}, Backend::timeoutMs());
    if (!answer.value("ok").toBool() && error)
        *error = answer.value("error").toString(tr("Couldn't change Liked Songs."));
    return answer.value("ok").toBool();
}

QHash<QString, bool> SessionCatalogue::liked(const QStringList &trackIds, QString *error) {
    QJsonArray ids;
    for (const QString &id : trackIds)
        ids << id;
    const QJsonObject answer = m_reader->ask({{"cmd", "liked"}, {"tracks", ids}}, Backend::timeoutMs());
    QHash<QString, bool> out;
    if (!answer.value("ok").toBool()) {
        if (error)
            *error = answer.value("error").toString();
        return out;
    }
    const QJsonArray found = answer.value("liked").toArray();
    for (int i = 0; i < trackIds.size() && i < found.size(); ++i)
        out.insert(trackIds.at(i), found.at(i).toBool());
    return out;
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
