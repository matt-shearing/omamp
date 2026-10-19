#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QUrl>
#include <memory>

#include "player.h"

class Library;

// The Player's resolver: turns a library track on a Subsonic server
// (subsonic://<source>/<song> in the play queue) into its signed stream URL
// with Library::playableUrl. Local files never come here.
//
// It keeps its own connection to the library, opened the first time a
// stream is asked for and only when the library file exists, as the play
// recorder does. Resolved URLs are kept in memory for a few minutes, so a
// requeue doesn't read the keyring again; they are never written anywhere.
class StreamResolver {
public:
    explicit StreamResolver(const QString &databasePath);
    ~StreamResolver();

    QUrl resolve(const QString &path, qint64 trackId, QString *error);
    // For Player::setResolver. The resolver must outlive the Player's use.
    Player::Resolver function() {
        return [this](const QString &path, qint64 trackId, QString *error) { return resolve(path, trackId, error); };
    }

    static constexpr qint64 kCacheMs = 10 * 60 * 1000;

private:
    Library *library();

    QString m_path;
    std::unique_ptr<Library> m_library;
    struct Cached {
        QUrl url;
        QElapsedTimer age;
    };
    QHash<QString, Cached> m_cache;
};
