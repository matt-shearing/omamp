#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

class Library;

// The command palette's results (Ctrl+K): one ranked list mixing actions
// the window registers with artists, albums and tracks from the library.
//
// Setting `text` searches at once, on the calling thread; a search takes a
// few milliseconds on a 200,000-track library. Actions match fuzzily ("pp"
// finds Play/Pause); library items match by words, ignoring case and
// accents. Text starting with ">" searches actions only. Text that uses the
// query language (artist:x, rating:4..) lists the tracks it selects.
//
// activate(row) emits what the user chose; the window decides what that
// means (play, show, run). The model never changes the library.
class Palette : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(double lastSearchMs READ lastSearchMs NOTIFY searched)

public:
    // External: a result from outside the library (Spotify), added with
    // setExternalResults() once its own search answers.
    enum Kind { Action, Artist, Album, Track, Search, External };
    Q_ENUM(Kind)

    enum Role {
        KindRole = Qt::UserRole + 1, // "action", "artist", "album", "track", "search"
        KindLabelRole,               // "Action", "Artist"... translated
        TitleRole,
        SubtitleRole,                // artist and album for a track, album artist for an album
        DetailRole,                  // shortcut for an action, length for a track, year for an album
        ShortcutRole,
        ItemIdRole,                  // action id (string) or database id (number)
        ScoreRole,
        EnabledRole,
        // Where a row comes from, for section headings: an external
        // source's name ("Spotify"), "Local" for the rest while external
        // rows are listed, and empty otherwise.
        SectionRole,
    };

    struct Item {
        Kind kind = Action;
        QString title, subtitle, detail, shortcut, actionId;
        qint64 id = 0;
        double score = 0;
        bool enabled = true;
        // External rows: the source ("Spotify"), the reference handed back
        // by externalActivated (a URI, in actionId), and the kind's label.
        QString source, kindLabel;
    };

    explicit Palette(QObject *parent = nullptr);
    ~Palette() override;

    // The library to search, or nullptr for actions only. Not owned; it must
    // outlive the palette or be unset first, and be used on this thread.
    void setLibrary(Library *library);

    QString text() const { return m_text; }
    void setText(const QString &text);
    int count() const { return int(m_items.size()); }
    double lastSearchMs() const { return m_lastSearchMs; }

    // Actions. id is what actionTriggered() sends back; keywords are other
    // words that should find it ("pause", "stop"); shortcut is shown as is.
    Q_INVOKABLE void addAction(const QString &id, const QString &title, const QStringList &keywords = {},
                               const QString &shortcut = {});
    Q_INVOKABLE void removeAction(const QString &id);
    // A disabled action is still listed (greyed) so people learn it exists,
    // but activating it does nothing.
    Q_INVOKABLE void setActionEnabled(const QString &id, bool enabled);
    Q_INVOKABLE QStringList actionIds() const;

    // Runs the search again, for example after a scan changed the library.
    Q_INVOKABLE void refresh();
    // The row's roles as a map, or an empty map.
    Q_INVOKABLE QVariantMap get(int row) const;
    // Emits the signal for the row's kind, then activated(). Returns false
    // for a row that does not exist or a disabled action.
    Q_INVOKABLE bool activate(int row);

    // Results from an external source for a given text. They show while
    // the palette's text is that text, after the library's rows. An empty
    // list takes the source's rows away.
    void setExternalResults(const QString &source, const QString &forText, const QList<Item> &items);
    Q_INVOKABLE bool hasExternalRows() const;

    // How many of each kind a search lists at most.
    void setLimits(int actions, int artists, int albums, int tracks);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Exposed for tests: how well text matches candidate, 0 (no match) to
    // 100 (the same), ignoring case and accents. fuzzy allows the letters to
    // be spread out ("pp" in "Play/Pause").
    static double score(const QString &text, const QString &candidate, bool fuzzy = false);

signals:
    void textChanged();
    void countChanged();
    void searched();
    void actionTriggered(const QString &id);
    void artistActivated(qint64 artistId, const QString &name);
    void albumActivated(qint64 albumId, const QString &title, const QString &albumArtist);
    void trackActivated(qint64 trackId);
    void searchRequested(const QString &text);
    void activated(const QVariantMap &item);
    void externalActivated(const QString &source, const QString &reference);

private:
    struct ActionEntry {
        QString id, title, shortcut;
        QStringList keywords;
        bool enabled = true;
        qint64 lastUsed = 0; // a counter, not a time: higher is more recent
    };
    struct Named {
        qint64 id;
        QString name, folded, extra, extraFolded;
        int year = 0;
        int tracks = 0;
    };

    void search();
    void searchActions(const QString &text, QList<Item> *out) const;
    void searchLibrary(const QString &text, QList<Item> *out);
    void loadNames();

    Library *m_library = nullptr;
    QString m_text;
    QList<Item> m_items;
    QList<ActionEntry> m_actions;
    struct ExternalSet {
        QString text;
        QList<Item> items;
    };
    QHash<QString, ExternalSet> m_external;
    qint64 m_useCounter = 0;
    double m_lastSearchMs = 0;
    int m_maxActions = 6, m_maxArtists = 4, m_maxAlbums = 4, m_maxTracks = 8;

    // Artist and album names, folded for matching, loaded once and reloaded
    // when the tables change. 50,000 albums take a few megabytes.
    QList<Named> m_artists, m_albums;
    QString m_namesSignature;
    QElapsedTimer m_namesChecked;
};
