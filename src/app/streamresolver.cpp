#include "streamresolver.h"

#include <QCoreApplication>
#include <QFileInfo>

#include "library.h"
#include "stats.h"

StreamResolver::StreamResolver(const QString &databasePath)
    : m_path(databasePath.isEmpty() ? Library::defaultPath() : databasePath) {}

StreamResolver::~StreamResolver() = default;

Library *StreamResolver::library() {
    if (m_library)
        return m_library.get();
    // Playing a stream never makes a library.
    if (!QFileInfo::exists(m_path))
        return nullptr;
    auto library = std::make_unique<Library>(m_path);
    if (!library->open())
        return nullptr;
    m_library = std::move(library);
    return m_library.get();
}

QUrl StreamResolver::resolve(const QString &path, qint64 trackId, QString *error) {
    // Spotify: the reference is enough, whether Thrum plays it itself
    // (spotifyaudio.h) or hands it to spotify_player (HandoffEngine).
    if (path.startsWith(QLatin1String("spotify://")))
        return QUrl(path);
    // A radio station: the radio picks the address (and falls back along
    // the station's list).
    if (path.startsWith(QLatin1String("radio://")) && m_radio)
        return m_radio(path, error);
    if (!path.startsWith(QLatin1String("subsonic://"))) {
        // Anything else that isn't a file: a plain http(s) address plays
        // as it is.
        const QUrl url(path);
        if (AudioEngine::isNetworkUrl(url))
            return url;
        if (error)
            *error = QCoreApplication::translate("StreamResolver", "not a library track");
        return {};
    }
    auto hit = m_cache.constFind(path);
    if (hit != m_cache.constEnd() && hit->age.isValid() && hit->age.elapsed() < kCacheMs)
        return hit->url;

    Library *lib = library();
    if (!lib) {
        if (error)
            *error = QCoreApplication::translate("StreamResolver", "the library isn't there");
        return {};
    }
    const qint64 id = trackId > 0 ? trackId : Stats::trackIdForPath(*lib, path);
    if (!id) {
        if (error)
            *error = QCoreApplication::translate("StreamResolver", "no longer in the library");
        return {};
    }
    QString why;
    const QUrl url = lib->playableUrl(id, &why);
    if (!url.isValid() || url.isEmpty()) {
        if (error)
            *error = AudioEngine::redact(why);
        m_cache.remove(path);
        return {};
    }
    Cached entry;
    entry.url = url;
    entry.age.start();
    m_cache.insert(path, entry);
    return url;
}
