#pragma once

#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>

class LibraryController;

// The sidebar's "About the artist", "About the album" and "Credits".
// QML sees it as `library.about`.
//
// show() takes what the sidebar follows: a library track by id (its tags,
// MusicBrainz ids and folder come from the library), or names alone for a
// Spotify track or a radio song. The local part (notes packaged in the
// album's folder, the comment tag, credits from the tags) comes first; the
// web part (About::Client: MusicBrainz → Wikidata → Wikipedia) follows on
// the same low-priority thread, unless the lookups are switched off or
// Thrum is offline. A newer show() supersedes an older one; a look-up
// already running finishes into the cache and is then dropped.
class AboutController : public QObject {
    Q_OBJECT
    // The subject's key ("track:12", "names:artist|album"); "" when nothing shows.
    Q_PROPERTY(QString subject READ subject NOTIFY changed)
    // idle, loading, found, none, offline, off (lookups switched off).
    Q_PROPERTY(QString artistState READ artistState NOTIFY changed)
    Q_PROPERTY(QString albumState READ albumState NOTIFY changed)
    // About::Summary as a map: title, description, extract, pageUrl,
    // imageFile (a file:// URL), lang, found.
    Q_PROPERTY(QVariantMap artist READ artist NOTIFY changed)
    Q_PROPERTY(QVariantMap album READ album NOTIFY changed)
    // [{name, text, path}]: the album's packaged notes, then the comment tag.
    Q_PROPERTY(QVariantList notes READ notes NOTIFY changed)
    // [{role, name}]: from the tags first, then MusicBrainz.
    Q_PROPERTY(QVariantList credits READ credits NOTIFY changed)
    Q_PROPERTY(QString artistName READ artistName NOTIFY changed)
    Q_PROPERTY(QString albumName READ albumName NOTIFY changed)

public:
    explicit AboutController(LibraryController *library);
    ~AboutController() override;

    QString subject() const { return m_subject; }
    QString artistState() const { return m_artistState; }
    QString albumState() const { return m_albumState; }
    QVariantMap artist() const { return m_artist; }
    QVariantMap album() const { return m_album; }
    QVariantList notes() const { return m_notes; }
    QVariantList credits() const { return m_credits; }
    QString artistName() const { return m_artistName; }
    QString albumName() const { return m_albumName; }

    // subject: trackId (a library track), or artist, album, title; plus
    // language (an Omarchy locale, "en_GB"), online (look things up on the
    // web) and refresh (ask the web again instead of the cache).
    Q_INVOKABLE void show(const QVariantMap &subject);
    // The last subject again, asking the web afresh.
    Q_INVOKABLE void retry();
    Q_INVOKABLE void clear();

    // Tests: where pictures go, and waiting less.
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }
    void setDelay(int ms) { m_delay.setInterval(ms); }

signals:
    void changed();
    // The web part of a subject finished (tests wait on this).
    void looked(const QString &subject);

private:
    void start();
    void finishLocal(int generation, const QVariantMap &local);
    void finishWeb(int generation, const QVariantMap &web);

    LibraryController *m_library;
    QThreadPool m_pool;
    QTimer m_delay;
    std::shared_ptr<std::atomic_int> m_generation;
    QVariantMap m_wanted;
    QString m_subject, m_artistState = "idle", m_albumState = "idle", m_cacheDir;
    QString m_artistName, m_albumName;
    QVariantMap m_artist, m_album;
    QVariantList m_notes, m_credits;
};
