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
#include "spotifympris.h"
#include "spotifyrequests.h"
#include "spotifysession.h"

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
    // The picture of a queue entry (spotify://track/<id>, or a whole
    // playlist, album or artist), for the deck and MPRIS. Safe from any
    // thread.
    QString fileForPath(const QString &path) const;
    // Remembers a track's album, so its queue entry finds the album cover.
    void remember(const Spotify::Item &item);
    // Fetches the picture if it isn't cached; artReady() when it lands.
    void want(const Spotify::Item &item);
    // Where the oEmbed lookups go; "off" turns them off.
    static QString oembedBase();
    // The title Spotify's public oEmbed page gives a link (an artist's name,
    // say), or empty; done runs on this object's thread.
    void lookUpTitle(const QString &webUrl, std::function<void(const QString &)> done);

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
//
// Rows are chosen as in the library's track list: a click chooses one,
// Shift+click a range from the last plain or Ctrl click, Ctrl+click adds or
// takes away one; Shift+Up/Down extends, Ctrl+A chooses every row, Esc
// clears. The current row is the keyboard cursor.
class SpotifyItemsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    // Tracks among the chosen rows (the current row when none are chosen).
    Q_PROPERTY(int chosenTrackCount READ chosenTrackCount NOTIFY selectionChanged)
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
        SelectedRole, // "rowSelected", as the library's track list
        CurrentRole,  // "rowCurrent"
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

    // Choosing rows, as TrackListModel does for the library.
    int currentRow() const { return m_current; }
    void setCurrentRow(int row);
    int selectedCount() const { return m_selected.size(); }
    int chosenTrackCount() const;
    Q_INVOKABLE void select(int row, int modifiers = 0);
    Q_INVOKABLE void moveCurrent(int delta, int modifiers = 0);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(int row) const { return m_selected.contains(row); }
    // The chosen rows in list order; the current row when none are chosen.
    Q_INVOKABLE QList<int> chosenRows() const;
    QList<Spotify::Item> chosenItems() const;
    // The tracks among them, in list order.
    QList<Spotify::Item> chosenTracks() const;

signals:
    void countChanged();
    void currentRowChanged();
    void selectionChanged();

private:
    void selectionDataChanged();

    SpotifyArt *m_art;
    QList<Spotify::Item> m_items;
    QStringList m_sections;
    QSet<int> m_selected;
    int m_current = -1, m_anchor = -1;
};

// The handover: a Spotify track's turn in Now Playing. Omamp tells the
// backend to play it on its Spotify Connect device, pauses its own engine
// (HandoffEngine does), and follows the position, a pause from elsewhere
// and the end of the track through the backend's MPRIS service, which costs
// Spotify nothing. Only when that service isn't there does it read the
// backend's playback state, a Web API request each time, and then slowly.
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
    // Whether the entry is a whole playlist, album or artist that Spotify
    // plays as one (spotify://playlist/<id>), and which.
    bool isContext() const { return m_context; }
    Spotify::Item context() const { return m_contextItem; }
    // How often the state is read through the backend's command line while
    // a track plays and MPRIS isn't there, and how long a start may take
    // before it counts as failed (tests shorten them).
    void setTimings(int pollMs, int startTimeoutMs);
    int pollMs() const { return m_pollMs; }
    // Reads the state now (tests, and after a command): from MPRIS when it
    // is there, else through the command line.
    void pollNow();
    // Whether the playing track is followed through MPRIS.
    bool followsMpris() const;
    // Whether a playback state shows this track: by id when it has one,
    // else (spotify_player's MPRIS has none) by title and length.
    bool isOurs(const Spotify::Playback &p) const;

private:
    void setState(AudioEngine::State state);
    void onMpris();
    void startFollowing();
    void readState();
    void onPolled(const Spotify::Playback &p, const QString &error);
    void fail(const QString &detail);
    void setBase(qint64 ms);
    void finishTrack();

    // A context entry's current track changed: show it.
    void takeTrack(const Spotify::Playback &p);

    SpotifyController *m_c;
    QString m_path;
    Spotify::Item m_track;
    // A context entry: Spotify plays the playlist, album or artist itself,
    // and m_track is whichever of its tracks plays now.
    bool m_context = false;
    bool m_shuffle = false;
    Spotify::Item m_contextItem;
    // What MPRIS showed before the start, so the start is seen as a change.
    QString m_beforeTitle;
    bool m_beforePlaying = false;
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
    // m_poll reads the command line (no MPRIS); m_position reads MPRIS's
    // position now and then, which the bus answers without Spotify.
    QTimer m_poll, m_tick, m_position;
    int m_pollMs = 5000, m_startTimeoutMs = 12000;
    // When following MPRIS, a track with no known name is confirmed by one
    // command-line read.
    bool m_askedOnce = false;
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
    // Read Spotify's own playlists and artists' top tracks the way Spotify's
    // own app does, through omamp-spotify-session.
    Q_PROPERTY(bool sessionReads READ sessionReads WRITE setSessionReads NOTIFY settingsChanged)
    Q_PROPERTY(bool sessionReaderInstalled READ sessionReaderInstalled NOTIFY settingsChanged)
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
    // The list has taken longer than a few seconds.
    Q_PROPERTY(bool loadingSlow READ loadingSlow NOTIFY loadingChanged)
    Q_PROPERTY(int loadingSeconds READ loadingSeconds NOTIFY loadingChanged)
    // Spotify answered "too many requests" lately; Omamp waits retryIn
    // seconds before it asks again.
    Q_PROPERTY(bool rateLimited READ rateLimited NOTIFY loadingChanged)
    Q_PROPERTY(int retryIn READ retryIn NOTIFY loadingChanged)
    // The list on show is an older copy, being read again.
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY loadingChanged)
    // How playback is followed: mpris, polling (the command line, slowly)
    // or none.
    Q_PROPERTY(QString followMode READ followMode NOTIFY statusChanged)
    // The Web API client ID spotify_player uses: own, desktop (Spotify's
    // own app's ID, set in app.toml), default (ncspot's, built in) or
    // command (client_id_command).
    Q_PROPERTY(QString clientIdKind READ clientIdKind NOTIFY clientIdChanged)
    Q_PROPERTY(QString clientId READ clientId NOTIFY clientIdChanged)
    Q_PROPERTY(QString appConfigPath READ appConfigPath CONSTANT)
    Q_PROPERTY(bool clientIdBusy READ clientIdBusy NOTIFY clientIdChanged)
    Q_PROPERTY(QString clientIdMessage READ clientIdMessage NOTIFY clientIdChanged)
    Q_PROPERTY(QString viewError READ viewError NOTIFY viewChanged)
    // The kind of thing the view shows (playlist, album, artist), or empty
    // for a library section or a search.
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY viewChanged)
    // A playlist whose tracks Spotify won't let your app read quickly: it
    // plays as a whole, and its tracks are listed only when asked for.
    Q_PROPERTY(bool viewRestricted READ viewRestricted NOTIFY viewChanged)
    // A sentence on why the view looks as it does, or empty.
    Q_PROPERTY(QString viewNote READ viewNote NOTIFY viewChanged)
    // What Spotify refused your client ID today, learned from
    // spotify_player's log ("artist-top-tracks", "playlist:<id>", …).
    Q_PROPERTY(QStringList refused READ refused NOTIFY refusedChanged)
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
    // Why Spotify tracks can't play now ("Spotify is off — …"), or empty
    // when they can. Playlists dim and skip their Spotify entries then.
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availabilityChanged)
    // Whether the backend can make a new Spotify playlist
    // (spotify_player's `playlist new`).
    Q_PROPERTY(bool canCreatePlaylists READ canCreatePlaylists NOTIFY settingsChanged)

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
    // Waits for the queue (tests).
    void waitForIdle();
    Spotify::Requests *requests() { return &m_requests; }
    Spotify::MprisWatch *mpris() { return &m_mpris; }
    // How long lists stay fresh, and how long to leave spotify_player alone
    // after starting it (it reads the whole library then). Tests shorten.
    void setCacheTtlMs(int ms) { m_cacheTtlMs = ms; }
    void setWarmupMs(int ms) { m_warmupMs = ms; }
    // After how long a list counts as slow to load.
    void setSlowMs(int ms) { m_slowMs = ms; }

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
    bool sessionReads() const { return m_settings.sessionReads; }
    void setSessionReads(bool on);
    bool sessionReaderInstalled() const { return !Spotify::SessionReader::findProgram().isEmpty(); }
    // The session reader in use, or null (tests).
    Spotify::SessionReader *sessionReader() const { return m_session.get(); }
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
    bool loadingSlow() const { return m_loadingSlow; }
    int loadingSeconds() const { return m_loading && m_loadClock.isValid() ? int(m_loadClock.elapsed() / 1000) : 0; }
    bool rateLimited() const { return m_rateLimited; }
    int retryIn() const;
    bool refreshing() const { return m_loading && m_items.count() > 0; }
    QString followMode() const;
    QString clientIdKind() const;
    QString clientId() const;
    QString appConfigPath() const;
    bool clientIdBusy() const { return m_clientIdBusy; }
    QString clientIdMessage() const { return m_clientIdMessage; }
    QString viewError() const { return m_view.error; }
    QString viewKind() const { return m_view.section == "tracks" ? m_view.container.kind : QString(); }
    bool viewRestricted() const { return m_view.restricted && !m_view.showTracks; }
    QString viewNote() const;
    QStringList refused() const { return m_refusals.keys(); }
    Spotify::Refusals *refusals() { return &m_refusals; }
    // Whether a playlist's tracks are out of quick reach: Spotify made it,
    // or Spotify refused your client ID for it today.
    bool isRestricted(const Spotify::Item &item) const;
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
    QString unavailableReason() const;
    bool canCreatePlaylists() const { return m_settings.enabled && m_backend->caps().playlists; }

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
    // Reads the list on show again, past the cache (the refresh button).
    Q_INVOKABLE void reload();
    // Stops waiting for the list on show.
    Q_INVOKABLE void cancelLoad();
    // Uses your own Spotify app's client ID: writes it into spotify_player's
    // app.toml (backed up first, nothing else changed), stops the running
    // spotify_player, and leaves it for you to sign in again, which starts
    // it. false, with clientIdMessage, when the ID isn't one.
    Q_INVOKABLE bool useOwnClientId(const QString &id);
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
    // The playlist or artist on show, as one Now Playing entry that Spotify
    // plays (shuffled or not), without reading its tracks.
    Q_INVOKABLE void playView(bool shuffle);
    // Adds it after the playing entry (next) or at the end.
    Q_INVOKABLE void enqueueView(bool next);
    // Lists a restricted playlist's tracks after all, slowly.
    Q_INVOKABLE void showRestrictedTracks();
    Q_INVOKABLE void addBlendRowToPlaylist(int row, const QString &playlistId);

    // The chosen rows of a list (the view's or the search strip's). One
    // row acts as that row does (a playlist or album row plays all of its
    // tracks); several rows act on the tracks among them.
    // how: play, next or append.
    Q_INVOKABLE void playChosen(SpotifyItemsModel *model, const QString &how);
    // Adds the chosen rows to one of your Spotify playlists.
    Q_INVOKABLE void addChosenToSpotifyPlaylist(SpotifyItemsModel *model, const QString &playlistId);
    // Makes a Spotify playlist (spotify_player's `playlist new`) and adds
    // the chosen rows to it.
    Q_INVOKABLE void addChosenToNewSpotifyPlaylist(SpotifyItemsModel *model, const QString &name);
    // Asks for the chosen tracks to go into an Omamp playlist: emits
    // omampEntriesReady with them, at once for track rows, or once an
    // album's or playlist's tracks are read. false when there is nothing.
    Q_INVOKABLE bool addChosenToOmampPlaylist(SpotifyItemsModel *model, qint64 playlistId);
    // The chosen rows' open.spotify.com links, one a line, put on the
    // clipboard; returns the text.
    Q_INVOKABLE QString copyChosenLinks(SpotifyItemsModel *model);
    // A playlist entry's map for a track: {path, title, artist, album, art, durationMs}.
    static QVariantMap entryFor(const Spotify::Item &track);
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
    // A playlist, album or artist seen in a list, by kind and id.
    Spotify::Item knownContext(const QString &kind, const QString &id) const;
    void rememberTracks(const QList<Spotify::Item> &tracks);
    // Queues a command on the control lane (play, pause, seek, state).
    void runControl(std::function<void(Spotify::Backend &)> work);
    void request(Spotify::Requests::Lane lane, const QString &key, Spotify::Requests::Work work,
                 Spotify::Requests::Done done);
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
    void clientIdChanged();
    void artRevisionChanged();
    void refusedChanged();
    // A picture for the playing track arrived (NowPlaying looks again).
    void playingArtChanged();
    // The window should show the Spotify view (a palette choice opened it).
    void showRequested();
    void settingsRequested();
    void availabilityChanged();
    // Spotify tracks to add to an Omamp playlist (entryFor maps), which
    // PlaylistsController stores.
    void omampEntriesReady(qint64 playlistId, const QVariantList &entries);

private:
    enum Mode { Play, Next, Append };
    struct Status {
        // known: the status has been read since Spotify was switched on.
        bool known = false;
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
        // A playlist out of quick reach, and whether its tracks were asked
        // for anyway.
        bool restricted = false;
        bool showTracks = false;
        QString key() const;
    };
    struct Cached {
        QList<Spotify::Item> items;
        QStringList sections;
        Spotify::SearchResults search;
        QElapsedTimer age;
    };

    void saveSettings();
    void makeBackend();
    // retry: an automatic retry after a rate limit, which keeps counting.
    void load(const View &view, bool force = false, bool retry = false);
    // A list through the cache: from it when fresh, else from the backend.
    // done gets the list and an error.
    void fetchList(const View &view, bool force, std::function<void(const Spotify::Result &)> done);
    int ttlFor(const View &view) const;
    void checkRateLimit(bool failed, const QString &error);
    void setRateLimited(bool on, const QDateTime &until = {});
    void fetchDevicesAndAccount(int generation);
    void followMpris();
    void noteMpris();
    void playFrom(SpotifyItemsModel *model, int row, Mode mode);
    void queue(const QList<Spotify::Item> &tracks, int start, Mode mode);
    void queueContext(const Spotify::Item &context, Mode mode, bool shuffle);
    void rememberNames(const QList<Spotify::Item> &items);
    // Fills in an artist's name from what Omamp has seen.
    Spotify::Item withName(Spotify::Item item) const;
    // Opens a playlist, album or artist in the view, looking up an
    // artist's name first when it isn't known.
    void openContainer(const Spotify::Item &item, bool keepBack);
    void learnRefusals(const Spotify::LogNews &news);
    void setLoading(bool on);
    void pollNowPlaying();
    void paletteTextChanged();
    void runPaletteSearch();
    void runBlend();
    void addToPlaylist(const Spotify::Item &item, const QString &playlistId);

    QString m_settingsPath;
    Spotify::Settings m_settings;
    std::shared_ptr<Spotify::Backend> m_backend;
    // omamp-spotify-session, kept running while Omamp runs once asked.
    std::shared_ptr<Spotify::SessionReader> m_session;
    QPointer<Player> m_player;
    QPointer<Palette> m_palette;
    SpotifyArt m_art;
    SpotifyItemsModel m_items, m_blend;
    Spotify::Requests m_requests;
    Spotify::MprisWatch m_mpris;
    SpotifyPlayback m_playback;
    std::function<bool(const QStringList &)> m_launch;

    Status m_status;
    bool m_checking = false;
    int m_statusGeneration = 0;
    QTimer m_signInWait;
    int m_signInChecks = 0;

    View m_view;
    QList<View> m_back;
    bool m_loading = false;
    bool m_loadingSlow = false;
    QElapsedTimer m_loadClock;
    QTimer m_loadTick;
    int m_viewGeneration = 0;
    // The view was asked for while not on screen: it loads when shown.
    bool m_viewStale = true;
    QHash<QString, Cached> m_cache;
    int m_cacheTtlMs = 10 * 60 * 1000;
    int m_warmupMs = 6000;
    int m_slowMs = 4000;
    // Rate limits, read from spotify_player's log.
    bool m_rateLimited = false;
    QDateTime m_retryUntil;
    QString m_logFile;
    qint64 m_logOffset = 0;
    int m_autoRetries = 0;
    QTimer m_retryTimer;
    QList<Spotify::Item> m_playlists;
    QSet<QString> m_liked;
    bool m_likedKnown = false;
    QHash<QString, Spotify::Item> m_known;
    // Playlists, albums and artists seen, by "<kind>:<id>".
    QHash<QString, Spotify::Item> m_contexts;
    Spotify::Refusals m_refusals;

    QVariantMap m_nowPlaying;
    bool m_viewVisible = false;
    QElapsedTimer m_nowRead;

    QTimer m_blendTimer, m_paletteTimer;
    QString m_blendText, m_blendError, m_paletteText;
    bool m_blendLoading = false;
    int m_blendGeneration = 0, m_paletteGeneration = 0;

    QString m_message;
    bool m_clientIdBusy = false;
    QString m_clientIdMessage;
    int m_artRevision = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
