#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>

#include "portal.h"
#include "radio.h"

class Library;
class Player;
class QNetworkReply;

// The channel picker's list: one group of stations (Nightride FM, Rekt
// Network, Favourites, My stations), each with what it is playing now.
class RadioStationsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        KeyRole = Qt::UserRole + 1,
        NameRole,
        FullNameRole,
        NetworkRole,
        NetworkNameRole,
        DescriptionRole,
        ColorRole,
        FavouriteRole,
        NowPlayingRole,
        PlayingRole,
        ArtUrlRole,
        BuiltinRole,
        HomepageRole,
        DetailRole,
    };

    explicit RadioStationsModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_rows.size(); }

    Q_INVOKABLE QString keyAt(int row) const { return row >= 0 && row < m_rows.size() ? m_rows[row].key : QString(); }
    Q_INVOKABLE int rowOf(const QString &key) const;
    const Radio::Station &at(int row) const { return m_rows.at(row); }
    QList<Radio::Station> stations() const { return m_rows; }

    // The controller fills these.
    void reset(const QList<Radio::Station> &rows);
    std::function<QString(const Radio::Station &)> nowPlaying;
    std::function<QString(const Radio::Station &)> artUrl;
    QString playingKey;
    void refreshRole(int role);
    void refreshKey(const QString &key, int role);

signals:
    void countChanged();

private:
    QList<Radio::Station> m_rows;
};

// Internet radio for the window (QML: `radio`): the stations, tuning in,
// the live song on Nightride channels (their now-playing feed) and other
// stations (ICY titles), switching channels, favourites, radio-browser.info
// search, and station lists in and out.
//
// It keeps its own connection to the library file, which it creates the
// first time the radio is used (stations live in the library database).
// Radio is never counted as a library play; songs heard go to radio_heard,
// and to the scrobble queue only when Settings turn that on.
class RadioController : public QObject {
    Q_OBJECT
    Q_PROPERTY(RadioStationsModel *stations READ stations CONSTANT)
    // nightride, rekt, favourites, mine or recent.
    Q_PROPERTY(QString group READ group WRITE setGroup NOTIFY groupChanged)
    Q_PROPERTY(QVariantList groups READ groups CONSTANT)
    // The station playing (or paused), as a map; empty when radio isn't on.
    Q_PROPERTY(bool active READ active NOTIFY currentChanged)
    Q_PROPERTY(QString currentKey READ currentKey NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)
    // "Artist – Title" on the current station, or "".
    Q_PROPERTY(QString nowPlaying READ nowPlaying NOTIFY nowPlayingChanged)
    // The window sets this while the picker shows, so every channel's
    // now-playing line stays live.
    Q_PROPERTY(bool pickerVisible READ pickerVisible WRITE setPickerVisible NOTIFY pickerVisibleChanged)
    Q_PROPERTY(bool feedConnected READ feedConnected NOTIFY feedChanged)
    // Find stations (radio-browser.info).
    Q_PROPERTY(QVariantList results READ results NOTIFY searchChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
    Q_PROPERTY(QString searchError READ searchError NOTIFY searchChanged)
    Q_PROPERTY(QString searchedFor READ searchedFor NOTIFY searchChanged)
    // Off by default: radio songs aren't the listener's choices.
    Q_PROPERTY(bool scrobble READ scrobble WRITE setScrobble NOTIFY scrobbleChanged)

public:
    // An empty path means the default library file.
    explicit RadioController(const QString &databasePath = {}, QObject *parent = nullptr);
    ~RadioController() override;

    void setPlayer(Player *player);
    // For StreamResolver::setRadioResolver: a station's address, the next
    // one along after a failure.
    QUrl resolve(const QString &path, QString *error);

    RadioStationsModel *stations() { return &m_model; }
    QString group() const { return m_group; }
    void setGroup(const QString &group);
    QVariantList groups() const;
    bool active() const { return m_current.isValid(); }
    QString currentKey() const { return m_current.key; }
    QVariantMap current() const;
    QString nowPlaying() const;
    bool pickerVisible() const { return m_pickerVisible; }
    void setPickerVisible(bool on);
    bool feedConnected() const { return m_feedConnected; }
    QVariantList results() const;
    bool searching() const { return m_searchThread != nullptr; }
    QString searchError() const { return m_searchError; }
    QString searchedFor() const { return m_searchedFor; }
    bool scrobble() const;
    void setScrobble(bool on);

    // Plays a station (by key, "radio://key", Nightride id or name). The
    // station takes the place of a station already playing, or goes in after
    // the current track, so the rest of Now Playing stays.
    Q_INVOKABLE bool tune(const QString &station);
    Q_INVOKABLE bool tuneRow(int row) { return tune(m_model.keyAt(row)); }
    // The next or previous channel in the playing station's group.
    Q_INVOKABLE bool step(int delta);
    Q_INVOKABLE bool toggleFavourite(const QString &key);
    Q_INVOKABLE bool isFavourite(const QString &key) const;
    // Adds a stream address (a .pls or .m3u address is read for its
    // stream). Returns the new station's key, or "" with a toast.
    Q_INVOKABLE void addUrl(const QString &url, const QString &name, bool play);
    Q_INVOKABLE bool removeStation(const QString &key);
    Q_INVOKABLE bool renameStation(const QString &key, const QString &name);
    Q_INVOKABLE void search(const QString &text);
    // Adds the result at index to My stations; plays it when asked.
    Q_INVOKABLE QString addResult(int index, bool play);
    // Plays a result without keeping it.
    Q_INVOKABLE bool previewResult(int index);
    Q_INVOKABLE void importLists();
    Q_INVOKABLE void exportList();
    Q_INVOKABLE QVariantMap importFile(const QString &file);
    Q_INVOKABLE QVariantMap exportFile(const QString &file, const QString &group = QStringLiteral("all"));
    // What a station plays now, as one line ("Artist – Title"), from the feed.
    Q_INVOKABLE QString nowPlayingFor(const QString &key) const;
    // Every station as {key, fullName, network}, for the palette.
    Q_INVOKABLE QVariantList allStations();
    Q_INVOKABLE QVariantList recentlyHeard(int limit = 50);
    Q_INVOKABLE int countIn(const QString &group);
    // Looks for channels nightride.fm added (at most once a day unless forced).
    Q_INVOKABLE void refreshCatalog(bool force = false);
    // Reads the stations again (after a change made elsewhere: the CLI).
    Q_INVOKABLE void reload();

    // Tests: the cache folder for tiles and favicons, and the URI handler
    // MPRIS and the command line use.
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }
    bool openUri(const QString &uri);
    Library *library();

signals:
    void groupChanged();
    void currentChanged();
    void nowPlayingChanged();
    void pickerVisibleChanged();
    void feedChanged();
    void searchChanged();
    void scrobbleChanged();
    void stationsChanged();
    void toast(const QString &text);
    // A radio song went into the scrobble queue; the play recorder sends it.
    void scrobbleQueued();
    // A song started on the current station (tests).
    void songChanged(const QString &artist, const QString &title);

private:
    void ensureSeeded();
    void onTrackChanged();
    void onPosition();
    void onEngineMeta();
    void onLiveFailed(const QString &path, const QString &detail);
    void leaveStation();
    void applySong(const QString &artist, const QString &title);
    void finishSong();
    void updateFeed();
    void connectFeed();
    void onFeedData(const QByteArray &data);
    void updateIcyReader();
    QString tilePath(const Radio::Station &station);
    void fetchArt(const Radio::Station &station);
    void runInThread(const QString &name, std::function<void()> work);
    void pushLiveInfo();

    QString m_dbPath, m_cacheDir;
    std::unique_ptr<Library> m_library;
    bool m_seeded = false;
    QPointer<Player> m_player;
    RadioStationsModel m_model;
    QString m_group = QStringLiteral("nightride");

    Radio::Station m_current;
    int m_attempt = 0;
    qint64 m_lastPos = 0, m_listenStart = 0;
    // The song on the current station.
    QString m_songArtist, m_songTitle;
    qint64 m_heardId = 0, m_songStartPos = 0, m_songStarted = 0;
    bool m_songFromStart = false, m_firstSong = true;

    QHash<QString, Radio::FeedRow> m_feed;
    QSet<QString> m_unknownFeedIds;
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_feedReply, m_icyReply;
    Radio::SseParser m_sse;
    Radio::IcyParser m_icy;
    QTimer m_feedRetry;
    int m_feedBackoffMs = 2000;
    bool m_feedConnected = false, m_pickerVisible = false;

    QList<Radio::Station> m_results;
    QSet<QString> m_resultsKnown;
    QString m_searchError, m_searchedFor;
    QThread *m_searchThread = nullptr;
    int m_searchGen = 0;
    QList<QThread *> m_threads;
    bool m_catalogRunning = false;

    QHash<QString, QString> m_tiles;
    FolderPicker m_importPicker, m_exportPicker;
};
