#pragma once

#include <QObject>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class LibraryController;

namespace Identify {
struct Result;
}
namespace Lyrics {
struct Parsed;
}

// The window's side of the web (Phase 4): identifying tracks against
// MusicBrainz with a review before anything is written, fetching missing
// covers, and lyrics for the info panel. QML sees it as `library.web`.
//
// Every look-up blocks, so each runs on a worker thread with its own
// Library and HttpClient. Identify and the cover fetches go through the
// controller's job slot (the jobs bar, Cancel, and a queue behind a running
// scan); writes are journalled, so Ctrl+Z undoes them. Lyrics have their own
// small thread, so the info panel never waits behind a scan.
class WebController : public QObject {
    Q_OBJECT
    // Identify: "idle", "working" (looking up or previewing), "ready", "applying".
    Q_PROPERTY(QString identifyState READ identifyState NOTIFY identifyChanged)
    // One entry per would-be album; see clusterMap() in the .cpp for the keys.
    Q_PROPERTY(QVariantList clusters READ clusters NOTIFY identifyChanged)
    // Notes for the whole run (offline, a service error), translated where known.
    Q_PROPERTY(QStringList identifyNotes READ identifyNotes NOTIFY identifyChanged)
    Q_PROPERTY(bool identifyOffline READ identifyOffline NOTIFY identifyChanged)
    // True when an AcoustID application key is set (setting or environment).
    Q_PROPERTY(bool acoustIdKeySet READ acoustIdKeySet NOTIFY identifyChanged)
    Q_PROPERTY(int identifyFiles READ identifyFiles NOTIFY identifyChanged)

    // Covers.
    Q_PROPERTY(QString coverState READ coverState NOTIFY coverChanged) // idle, working, choose
    Q_PROPERTY(QVariantList coverCandidates READ coverCandidates NOTIFY coverChanged)
    Q_PROPERTY(QString coverAlbum READ coverAlbum NOTIFY coverChanged)

    // Lyrics for one track, as the info panel's Lyrics tab shows them.
    Q_PROPERTY(qint64 lyricsTrack READ lyricsTrack NOTIFY lyricsChanged)
    Q_PROPERTY(QString lyricsState READ lyricsState NOTIFY lyricsChanged) // idle, loading, found, none, error
    Q_PROPERTY(QVariantList lyricsLines READ lyricsLines NOTIFY lyricsChanged) // [{time, text}]
    Q_PROPERTY(bool lyricsSynced READ lyricsSynced NOTIFY lyricsChanged)
    Q_PROPERTY(QString lyricsSource READ lyricsSource NOTIFY lyricsChanged) // sidecar, embedded, lrclib
    Q_PROPERTY(QString lyricsError READ lyricsError NOTIFY lyricsChanged)
    Q_PROPERTY(bool lyricsInstrumental READ lyricsInstrumental NOTIFY lyricsChanged)

public:
    explicit WebController(LibraryController *library);
    ~WebController() override;

    QString identifyState() const { return m_identifyState; }
    QVariantList clusters() const;
    QStringList identifyNotes() const { return m_notes; }
    bool identifyOffline() const;
    bool acoustIdKeySet() const;
    int identifyFiles() const;

    QString coverState() const { return m_coverState; }
    QVariantList coverCandidates() const { return m_coverCandidates; }
    QString coverAlbum() const { return m_coverAlbumName; }

    qint64 lyricsTrack() const { return m_lyricsTrack; }
    QString lyricsState() const { return m_lyricsState; }
    QVariantList lyricsLines() const { return m_lyricsLines; }
    bool lyricsSynced() const { return m_lyricsSynced; }
    QString lyricsSource() const { return m_lyricsSource; }
    QString lyricsError() const { return m_lyricsError; }
    bool lyricsInstrumental() const { return m_lyricsInstrumental; }

    // ---- Identify ----------------------------------------------------------
    // Clusters the tracks and finds release candidates for each, then shows
    // what the best one would change (a dry run). Returns false when there
    // is nothing to identify.
    Q_INVOKABLE bool identify(const QVariantList &trackIds);
    // Picks another of a cluster's candidates and shows its changes.
    Q_INVOKABLE void chooseCandidate(int cluster, int candidate);
    // "Use another release": a MusicBrainz release ID or a link to one.
    // Returns false (with a toast) when text holds no release ID.
    Q_INVOKABLE bool useRelease(int cluster, const QString &text);
    Q_INVOKABLE void setIncluded(int cluster, bool on);
    // Writes the chosen release of every included cluster, each as one
    // journalled batch. options: coverEmbed, coverFile, writeGenres, writeLabel.
    Q_INVOKABLE bool applyIdentify(const QVariantMap &options);
    Q_INVOKABLE void clearIdentify();
    // The apply options as the settings have them.
    Q_INVOKABLE QVariantMap identifyOptions() const;
    // Pulls a release MBID out of text ("…/release/<id>" or the bare id); empty if none.
    static QString releaseIdFrom(const QString &text);

    // ---- Covers ------------------------------------------------------------
    // Fetches covers for every album the "missing artwork" check lists.
    Q_INVOKABLE bool findMissingCovers();
    // One album: looks for candidates; one is set at once, several are
    // offered in coverCandidates to choose from.
    Q_INVOKABLE bool findCover(qint64 albumId);
    Q_INVOKABLE bool chooseCover(int index);
    Q_INVOKABLE void clearCovers();

    // ---- Lyrics ------------------------------------------------------------
    // Reads the track's lyrics (file, sidecar, cache) on the lyrics thread;
    // fetch also asks LRCLIB when there are none.
    Q_INVOKABLE void loadLyrics(qint64 trackId, bool fetch = false);
    // The line to highlight at a position, or -1 (plain lyrics, or before the first line).
    Q_INVOKABLE int lyricsLineAt(qint64 positionMs) const;
    // Saves the lyrics shown: "lrc" writes a .lrc beside the file, "tag"
    // writes them into the file. Journalled and undoable.
    Q_INVOKABLE bool saveLyrics(const QString &mode);

    // Tests: where the cover candidates' previews are saved.
    void setPreviewDir(const QString &dir) { m_previewDir = dir; }

signals:
    void identifyChanged();
    void coverChanged();
    void lyricsChanged();
    // A short note for the toast.
    void message(const QString &text);
    // Identify finished writing (tests wait on this).
    void identifyApplied(int changed, int failed);
    void coversFetched(int fetched, int notFound);

private:
    void preview(const QList<int> &clusters);
    void finishLyrics(qint64 trackId, const QVariantMap &entry);
    QString previewDir() const;

    LibraryController *m_controller;
    std::shared_ptr<Identify::Result> m_result;
    QList<int> m_chosen;     // per cluster: the candidate chosen
    QList<bool> m_included;  // per cluster
    QList<QVariantList> m_changes; // per cluster: the dry run's changes
    QStringList m_previewErrors;
    QStringList m_notes;
    QString m_identifyState = "idle";

    QString m_coverState = "idle";
    QVariantList m_coverCandidates;
    qint64 m_coverAlbum = 0;
    QString m_coverAlbumName, m_previewDir;

    QThread *m_lyricsThread = nullptr;
    qint64 m_lyricsTrack = 0, m_lyricsWanted = 0;
    bool m_lyricsFetchWanted = false;
    QString m_lyricsState = "idle", m_lyricsSource, m_lyricsError, m_lyricsPlain, m_lyricsSyncedText;
    QVariantList m_lyricsLines;
    std::unique_ptr<Lyrics::Parsed> m_parsed;
    bool m_lyricsSynced = false, m_lyricsInstrumental = false;
};
