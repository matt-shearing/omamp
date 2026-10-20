#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>

#include "handoffengine.h"
#include "spotify.h"

#include "palette.h"
#include "player.h"
class SpotifyController;

// Spotify pictures, cached in ~/.cache/omamp/spotify as <key>.jpg: an
// album's cover (album-<id>), or a playlist's or artist's picture.
// spotify_player's lists carry no pictures, so a picture comes from the
// playback state (the playing track's album) or, for anything else, from
// Spotify's public oEmbed page (no sign-in, no token), when it is shown.
class SpotifyArt : public QObject {
    Q_OBJECT
public:
    explicit SpotifyArt(const QString &cacheDir = {}, QObject *parent = nullptr);

    QString dir() const { return m_dir; }
    static QString keyFor(const Spotify::Item &item);
    // The cached picture's path, or empty. Safe from any thread.
    QString cachedFile(const Spotify::Item &item) const;
    // The picture of a queue entry (spotify://track/<id>), for the deck and
    // MPRIS. Safe from any thread.
    QString fileForPath(const QString &path) const;
    // Remembers a track's album, so its queue entry finds the album cover.
    void remember(const Spotify::Item &item);
    // Fetches the picture if it isn't cached; artReady() when it lands.
    void want(const Spotify::Item &item);
    // Where the oEmbed lookups go; "off" turns them off.
    static QString oembedBase();

signals:
    void artReady(const QString &key);

private:
    void pump();
    void download(const QString &key, const QUrl &url);
    void lookUp(const QString &key, const QString &webUrl);
    void finishOne(const QString &key);

    QString m_dir;
    QNetworkAccessManager m_net;
    mutable QMutex m_mutex;
    QHash<QString, QString> m_trackAlbum; // track id → album id
    struct Want {
        QString key;
        QUrl image;   // known picture
        QString web;  // else the open.spotify.com page to ask oEmbed about
    };
    QList<Want> m_queue;
    QSet<QString> m_pending, m_failed;
    int m_running = 0;
};

// One list of Spotify items for QML: a library section, a playlist's or
// album's tracks, or search results with section headings.
class SpotifyItemsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        ItemIdRole,
        UriRole,
        NameRole,
        ArtistsRole,
        AlbumRole,
        DurationRole,
        YearRole,
        OwnerRole,
        SectionRole,
        ArtRole,
        DetailRole,
    };
    explicit SpotifyItemsModel(SpotifyArt *art, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_items.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_items.size(); }
    // sections: a heading for each item, or none.
    void setItems(const QList<Spotify::Item> &items, const QStringList &sections = {});
    const QList<Spotify::Item> &items() const { return m_items; }
    Spotify::Item at(int row) const { return m_items.value(row); }
    Q_INVOKABLE QVariantMap get(int row) const;
    // The tracks of the list, in order.
    QList<Spotify::Item> tracks() const;
    Q_INVOKABLE int trackIndex(int row) const;
    // The art of a row is fetched when the row is shown.
    Q_INVOKABLE void wantArt(int row);
    void artArrived(const QString &key);

signals:
    void countChanged();

private:
    SpotifyArt *m_art;
    QList<Spotify::Item> m_items;
    QStringList m_sections;
};

// The handover: a Spotify track's turn in Now Playing. Omamp tells the
// backend to play it on its Spotify Connect device, pauses its own engine
// (HandoffEngine does), and polls the backend's playback state to follow
// the position, a pause from elsewhere and the end of the track.
class SpotifyPlayback : public ExternalPlayback {
    Q_OBJECT
public:
    explicit SpotifyPlayback(SpotifyController *controller);

    bool handles(const QString &path) const override { return Spotify::isQueuePath(path); }
    void load(const QString &path) override;
    void unload() override;
    QString source() const override { return m_path; }
    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 ms) override;
    qint64 position() const override;
    qint64 duration() const override { return m_track.durationMs; }
    AudioEngine::State state() const override { return m_state; }
    bool buffering() const override { return m_starting; }
    QVariantMap metaData() const override;

    QString trackId() const { return m_track.id; }
    // How often the state is read while a track plays, and how long a
    // start may take before it counts as failed (tests shorten them).
    void setTimings(int pollMs, int startTimeoutMs);
    // Reads the state now (tests, and after a command).
    void pollNow();

private:
    void setState(AudioEngine::State state);
    void onPolled(const Spotify::Playback &p, const QString &error);
    void fail(const QString &detail);
    void setBase(qint64 ms);
    void finishTrack();

    SpotifyController *m_c;
    QString m_path;
    Spotify::Item m_track;
    AudioEngine::State m_state = AudioEngine::Stopped;
    // Set once Spotify was told to play this track, until it is unloaded.
    bool m_started = false;
    // Waiting for the backend to show the track playing.
    bool m_starting = false;
    bool m_confirmed = false;
    bool m_polling = false;
    qint64 m_base = 0;
    QElapsedTimer m_since;
    QElapsedTimer m_startedAt;
    QTimer m_poll, m_tick;
    int m_pollMs = 1000, m_startTimeoutMs = 12000;
    int m_generation = 0;
    // Our own pause, so a paused state isn't taken as the end.
    bool m_pausedByUs = false;
    // Counts our commands; a state read that began before the latest one
    // is stale (the command and the read run in order on one worker).
    int m_commands = 0;
    // When the last command went out: spotify_player carries a command out
    // a moment after its CLI returns, so a state read just after it may
    // still show the old state.
    QElapsedTimer m_lastCommand;
};

// Settings → Spotify, the Spotify view and the blending, for QML as
// `spotify`. Off until "Show Spotify in Omamp" is switched on; then every
// call to the backend runs on a worker thread and answers with a signal.
class SpotifyController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString backendId READ backendId WRITE setBackendId NOTIFY settingsChanged)
    Q_PROPERTY(bool blendSearch READ blendSearch WRITE setBlendSearch NOTIFY settingsChanged)
    Q_PROPERTY(bool blendPalette READ blendPalette WRITE setBlendPalette NOTIFY settingsChanged)
    Q_PROPERTY(bool startDaemon READ startDaemon WRITE setStartDaemon NOTIFY settingsChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY settingsChanged)
    Q_PROPERTY(bool canBrowse READ canBrowse NOTIFY settingsChanged)
    Q_PROPERTY(bool installed READ installed NOTIFY statusChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY statusChanged)
    Q_PROPERTY(bool running READ running NOTIFY statusChanged)
    Q_PROPERTY(QString account READ account NOTIFY statusChanged)
    // yes, no or unknown.
    Q_PROPERTY(QString premium READ premium NOTIFY statusChanged)
    Q_PROPERTY(QString device READ device NOTIFY statusChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY statusChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY statusChanged)
    // One sentence on where things stand, for the settings page.
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY statusChanged)
    Q_PROPERTY(bool waitingForSignIn READ waitingForSignIn NOTIFY statusChanged)
    // The Spotify view.
    Q_PROPERTY(QString section READ section NOTIFY viewChanged)
    Q_PROPERTY(QString viewTitle READ viewTitle NOTIFY viewChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY viewChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString viewError READ viewError NOTIFY viewChanged)
    Q_PROPERTY(QString searchText READ searchText NOTIFY viewChanged)
    Q_PROPERTY(SpotifyItemsModel *items READ items CONSTANT)
    Q_PROPERTY(QVariantList ownPlaylists READ ownPlaylists NOTIFY playlistsChanged)
    // What the backend is playing, whoever started it: {active, playing,
    // title, artists, album, art, progressMs, durationMs, device, liked}.
    Q_PROPERTY(QVariantMap nowPlaying READ nowPlaying NOTIFY nowPlayingChanged)
    // Spotify's answers to the library search box.
    Q_PROPERTY(SpotifyItemsModel *blendResults READ blendResults CONSTANT)
    Q_PROPERTY(QString blendText READ blendText NOTIFY blendChanged)
    Q_PROPERTY(bool blendLoading READ blendLoading NOTIFY blendChanged)
    Q_PROPERTY(QString blendError READ blendError NOTIFY blendChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(int artRevision READ artRevision NOTIFY artRevisionChanged)

public:
    // settingsPath: spotify.conf (empty: ~/.config/omamp/spotify.conf).
    // artDir: the picture cache (empty: ~/.cache/omamp/spotify).
    explicit SpotifyController(const QString &settingsPath = {}, const QString &artDir = {}, QObject *parent = nullptr);
    ~SpotifyController() override;

    void setPlayer(Player *player) { m_player = player; }
    void setPalette(Palette *palette);
    SpotifyPlayback *playback() { return &m_playback; }
    SpotifyArt *art() { return &m_art; }
    // How a terminal is opened for signing in (tests capture the command).
    void setTerminalLauncher(std::function<bool(const QStringList &)> launcher) { m_launch = std::move(launcher); }
    // Waits for the worker threads (tests).
    void waitForIdle();

    bool enabled() const { return m_settings.enabled; }
    void setEnabled(bool on);
    QString backendId() const { return m_settings.backend; }
    void setBackendId(const QString &id);
    bool blendSearch() const { return m_settings.blendSearch; }
    void setBlendSearch(bool on);
    bool blendPalette() const { return m_settings.blendPalette; }
    void setBlendPalette(bool on);
    bool startDaemon() const { return m_settings.startDaemon; }
    void setStartDaemon(bool on);
    QString backendName() const { return m_backend->name(); }
    bool canBrowse() const { return m_backend->caps().library; }

    bool installed() const { return m_status.installed; }
    bool signedIn() const { return m_status.signedIn; }
    bool running() const { return m_status.running; }
    QString account() const { return m_status.account; }
    QString premium() const { return m_status.premium; }
    QString device() const { return m_status.device; }
    QString deviceName() const { return m_status.deviceName; }
    QVariantList devices() const { return m_status.devices; }
    QString statusText() const;
    bool checking() const { return m_checking; }
    bool waitingForSignIn() const { return m_signInWait.isActive(); }

    QString section() const { return m_view.section; }
    QString viewTitle() const;
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool loading() const { return m_loading; }
    QString viewError() const { return m_view.error; }
    QString searchText() const { return m_view.search; }
    SpotifyItemsModel *items() { return &m_items; }
    QVariantList ownPlaylists() const;
    QVariantMap nowPlaying() const { return m_nowPlaying; }
    SpotifyItemsModel *blendResults() { return &m_blend; }
    QString blendText() const { return m_blendText; }
    bool blendLoading() const { return m_blendLoading; }
    QString blendError() const { return m_blendError; }
    QString message() const { return m_message; }
    int artRevision() const { return m_artRevision; }

    // Status: installed, signed in, running, devices, account; starts the
    // daemon when that is switched on.
    Q_INVOKABLE void refreshStatus();
    // Opens a terminal running the backend's own sign-in.
    Q_INVOKABLE bool signIn();
    Q_INVOKABLE void startBackend();
    Q_INVOKABLE void connectDevice(const QString &id);

    // The view: playlists, liked, albums, artists or search.
    Q_INVOKABLE void openSection(const QString &section);
    // A playlist, album or artist row: its tracks.
    Q_INVOKABLE void openRow(int row);
    Q_INVOKABLE void back();
    Q_INVOKABLE void search(const QString &text);
    Q_INVOKABLE void reload();
    // Whether the view is on screen, so its "playing now" stays fresh.
    Q_INVOKABLE void setViewVisible(bool visible);

    // Now Playing. A track row plays the list from it (as the library
    // does); a playlist, album or artist row plays its tracks.
    Q_INVOKABLE void playRow(int row) { playFrom(&m_items, row, Play); }
    Q_INVOKABLE void playNextRow(int row) { playFrom(&m_items, row, Next); }
    Q_INVOKABLE void enqueueRow(int row) { playFrom(&m_items, row, Append); }
    Q_INVOKABLE void playBlendRow(int row) { playFrom(&m_blend, row, Play); }
    Q_INVOKABLE void playNextBlendRow(int row) { playFrom(&m_blend, row, Next); }
    Q_INVOKABLE void enqueueBlendRow(int row) { playFrom(&m_blend, row, Append); }
    // A row of the blend strip that isn't a track opens in the view.
    Q_INVOKABLE void openBlendRow(int row);
    // A link or URI pasted or typed: a track plays next, anything else
    // opens. Returns "played", "opened" or "" (not a Spotify link).
    Q_INVOKABLE QString activateUri(const QString &text);
    Q_INVOKABLE void addRowToPlaylist(int row, const QString &playlistId);
    Q_INVOKABLE void addBlendRowToPlaylist(int row, const QString &playlistId);
    // Liked Songs for the playing Spotify track.
    Q_INVOKABLE void setPlayingLiked(bool liked);
    Q_INVOKABLE void dismissMessage() { setMessage({}); }
    // Asks the window to open Settings → Spotify.
    Q_INVOKABLE void openSettings() { emit settingsRequested(); }

    // The library search box's text; Spotify's answers fill blendResults.
    Q_INVOKABLE void blendQuery(const QString &text);

    std::shared_ptr<Spotify::Backend> backend() const { return m_backend; }
    // A track seen in a list, search or the playback state, by id.
    Spotify::Item knownTrack(const QString &id) const { return m_known.value(id); }
    void rememberTracks(const QList<Spotify::Item> &tracks);
    // Runs work on the playback worker and hands the answer back here.
    void runControl(std::function<void(Spotify::Backend &)> work);
    QThreadPool *controlPool() { return &m_control; }
    void setMessage(const QString &message);
    void noteStarted(const Spotify::Playback &p);
    // The backend's playback state, for the view's "playing now".
    void noteNowPlaying(const Spotify::Playback &p);
    // Why a start may have failed, for the message.
    QString startFailureHint() const;

signals:
    void settingsChanged();
    void statusChanged();
    void viewChanged();
    void loadingChanged();
    void playlistsChanged();
    void nowPlayingChanged();
    void blendChanged();
    void messageChanged();
    void artRevisionChanged();
    // A picture for the playing track arrived (NowPlaying looks again).
    void playingArtChanged();
    // The window should show the Spotify view (a palette choice opened it).
    void showRequested();
    void settingsRequested();

private:
    enum Mode { Play, Next, Append };
    struct Status {
        bool installed = false, signedIn = false, running = false;
        QString account, premium = "unknown", device, deviceName;
        QVariantList devices;
        QString error;
    };
    struct View {
        QString section = "playlists";
        Spotify::Item container; // a playlist, album or artist being shown
        QString search;
        QString error;
    };

    void saveSettings();
    void makeBackend();
    void load(const View &view);
    void playFrom(SpotifyItemsModel *model, int row, Mode mode);
    void queue(const QList<Spotify::Item> &tracks, int start, Mode mode);
    void runData(std::function<void(Spotify::Backend &)> work);
    void setLoading(bool on);
    void pollNowPlaying();
    void paletteTextChanged();
    void runPaletteSearch();
    void runBlend();
    void addToPlaylist(const Spotify::Item &item, const QString &playlistId);

    QString m_settingsPath;
    Spotify::Settings m_settings;
    std::shared_ptr<Spotify::Backend> m_backend;
    QPointer<Player> m_player;
    QPointer<Palette> m_palette;
    SpotifyArt m_art;
    SpotifyItemsModel m_items, m_blend;
    SpotifyPlayback m_playback;
    QThreadPool m_control, m_data;
    std::function<bool(const QStringList &)> m_launch;

    Status m_status;
    bool m_checking = false;
    QTimer m_signInWait;
    int m_signInChecks = 0;

    View m_view;
    QList<View> m_back;
    bool m_loading = false;
    int m_viewGeneration = 0;
    QList<Spotify::Item> m_playlists;
    QSet<QString> m_liked;
    bool m_likedKnown = false;
    QHash<QString, Spotify::Item> m_known;

    QVariantMap m_nowPlaying;
    QTimer m_viewPoll;
    bool m_viewVisible = false;
    bool m_nowPolling = false;

    QTimer m_blendTimer, m_paletteTimer;
    QString m_blendText, m_blendError, m_paletteText;
    bool m_blendLoading = false;
    int m_blendGeneration = 0, m_paletteGeneration = 0;

    QString m_message;
    int m_artRevision = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
