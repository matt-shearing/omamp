// The library window's models: the track list (paging, sorting, searching,
// selection), the tree (nodes and their scopes) and the controller (scans
// on a worker thread, handing tracks to the player). Every database lives in
// a temporary folder, and no test starts playback.

#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>

#include "albumgridmodel.h"
#include "artprovider.h"
#include "columnbrowser.h"
#include "library.h"
#include "librarycontroller.h"
#include "nowplaying_context.h"
#include "librarytree.h"
#include "player.h"
#include "portal.h"
#include "theme.h"
#include "translator.h"
#include "tracklistmodel.h"
#include "volumes.h"

namespace {

const QStringList genres = {"Rock", "Jazz", "Pop"};

// A library of n tracks with values that are easy to count:
//   artist "Artist <i % 5>", album "<letter> Album <i / 10>" (ten tracks each),
//   year 1990 + i % 20, genre Rock/Jazz/Pop by i % 3, rating (i % 6) * 20 with
//   0 stored as NULL (unrated), folder "Artist <i % 5>/Album <i / 10>",
//   title "Track <i>" with i zero-padded.
// Rows are written straight into the tables, as the benchmark does; nothing
// is read from disk.
void fill(Library &library, const QString &root, int n, QString *prefixOut = nullptr) {
    QSqlDatabase db = library.db();
    db.transaction();
    QSqlQuery q(db);
    // The real volume holding the temporary folder, so opening the library
    // (which checks every volume) finds it online. The tracks' paths are
    // relative to that volume, under the folder.
    const VolumeInfo volume = VolumeResolver().identify(QFileInfo(root).canonicalFilePath(), false);
    QVERIFY(volume.isValid());
    q.prepare("INSERT INTO volumes (id, key, kind, label, root_path, mount_subpath, online, added) "
              "VALUES (1, ?, 'local', 'test', ?, ?, 1, 0)");
    q.addBindValue(volume.key);
    q.addBindValue(volume.rootPath);
    q.addBindValue(volume.mountSubpath);
    QVERIFY(q.exec());
    QString prefix = QDir(volume.rootPath).relativeFilePath(QFileInfo(root).canonicalFilePath());
    if (prefix == ".")
        prefix.clear();
    if (!prefix.isEmpty())
        prefix += '/';
    if (prefixOut)
        *prefixOut = prefix;
    QVERIFY(q.exec("INSERT INTO volumes (id, key, kind, label, root_path, online, added) "
                   "VALUES (2, 'mount:gone', 'network', 'NAS', '/mnt/nas', 0, 0)"));
    q.prepare("INSERT INTO folders (id, volume_id, rel_path, is_root, added) VALUES (1, 1, ?, 1, 0)");
    q.addBindValue(prefix.chopped(prefix.isEmpty() ? 0 : 1));
    QVERIFY(q.exec());
    for (int a = 0; a < 5; ++a) {
        q.prepare("INSERT INTO artists (id, name) VALUES (?, ?)");
        q.addBindValue(a + 1);
        q.addBindValue(QString("Artist %1").arg(a));
        QVERIFY(q.exec());
    }
    for (int g = 0; g < genres.size(); ++g) {
        q.prepare("INSERT INTO genres (id, name) VALUES (?, ?)");
        q.addBindValue(g + 1);
        q.addBindValue(genres[g]);
        QVERIFY(q.exec());
    }
    QSqlQuery folder(db), album(db), track(db), link(db), genre(db);
    folder.prepare("INSERT OR IGNORE INTO folders (volume_id, rel_path, is_root, added) VALUES (1, ?, 0, 0)");
    album.prepare("INSERT OR IGNORE INTO albums (id, title, album_artist, artist_key, title_key, year) "
                  "VALUES (?, ?, ?, ?, ?, ?)");
    track.prepare("INSERT INTO tracks (id, volume_id, folder_id, rel_path, file_name, format, size, mtime_ns, title, "
                  "artist, album_artist, sort_artist, album, album_id, genre, year, track_no, duration_ms, bitrate, "
                  "rating, play_count, added) VALUES (?, 1, (SELECT id FROM folders WHERE rel_path = ?), ?, ?, "
                  "'flac', 1, 1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 900, ?, ?, ?)");
    link.prepare("INSERT INTO track_artists (track_id, artist_id, role) VALUES (?, ?, ?)");
    genre.prepare("INSERT INTO track_genres (track_id, genre_id) VALUES (?, ?)");
    for (int i = 0; i < n; ++i) {
        const int id = i + 1;
        const QString artist = QString("Artist %1").arg(i % 5);
        const int albumNo = i / 10;
        // Albums start with a letter from A to E, so the Album node has letters.
        const QString albumName = QString("%1 Album %2").arg(QChar('A' + albumNo % 5)).arg(albumNo);
        const QString dir = prefix + QString("%1/Album %2").arg(artist).arg(albumNo);
        const QString name = QString("Track %1").arg(i, 5, 10, QChar('0'));
        folder.addBindValue(dir);
        QVERIFY(folder.exec());
        album.addBindValue(albumNo + 1);
        album.addBindValue(albumName);
        album.addBindValue(artist);
        album.addBindValue(artist.toLower());
        album.addBindValue(albumName.toLower());
        album.addBindValue(1990 + i % 20);
        QVERIFY(album.exec());
        const int rating = (i % 6) * 20;
        const QVariantList values = {id, dir, dir + '/' + name + ".flac", name, name, artist, artist, artist,
                                     albumName, albumNo + 1, genres[i % 3], 1990 + i % 20, i % 10 + 1,
                                     (60 + i % 300) * 1000,
                                     rating ? QVariant(rating) : QVariant(QMetaType(QMetaType::Int)), i % 7, 1000 + i};
        for (const QVariant &v : values)
            track.addBindValue(v);
        QVERIFY2(track.exec(), qPrintable(track.lastError().text()));
        link.addBindValue(id);
        link.addBindValue(i % 5 + 1);
        link.addBindValue("artist");
        QVERIFY(link.exec());
        genre.addBindValue(id);
        genre.addBindValue(i % 3 + 1);
        QVERIFY(genre.exec());
    }
    QVERIFY(db.commit());
}

int count(Library &library, const QString &where) {
    QSqlQuery q(library.db());
    if (!q.exec("SELECT count(*) FROM tracks t WHERE " + where) || !q.next())
        return -1;
    return q.value(0).toInt();
}

QString cell(TrackListModel &model, int row, int column) {
    return model.data(model.index(row, column), Qt::DisplayRole).toString();
}

} // namespace

class LibraryWindowTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Library> m_library;
    QString m_prefix; // the temporary folder's path on its volume, with a slash
    static constexpr int tracks = 1000;

private slots:
    void initTestCase() {
        m_library = std::make_unique<Library>(m_dir.filePath("library.db"));
        QString error;
        QVERIFY2(m_library->open(&error), qPrintable(error));
        fill(*m_library, m_dir.path(), tracks, &m_prefix);
    }

    // ---- Track list ----------------------------------------------------------

    void listsEveryTrackAndPagesOnDemand() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        QCOMPARE(model.rowCount(), tracks);
        QCOMPARE(model.columnCount(), int(TrackListModel::ColumnCount));
        // Library order: sort artist, album, disc, track.
        QCOMPARE(cell(model, 0, TrackListModel::Artist), QString("Artist 0"));
        QCOMPARE(cell(model, tracks - 1, TrackListModel::Artist), QString("Artist 4"));
        // Rows far apart, then every row once: pages come and go and every
        // row still matches its id.
        for (int row : {999, 0, 500, 128, 127, 129})
            QCOMPARE(model.data(model.index(row, 0), TrackListModel::TrackIdRole).toLongLong(), model.trackId(row));
        for (int row = 0; row < tracks; ++row) {
            const QString title = cell(model, row, TrackListModel::Title);
            QCOMPARE(title, QString("Track %1").arg(model.trackId(row) - 1, 5, 10, QChar('0')));
        }
        QCOMPARE(cell(model, 0, TrackListModel::Length), QString("1:00"));
        QCOMPARE(model.data(model.index(0, TrackListModel::Rating), TrackListModel::RatingRole).toInt(), -1);
        QCOMPARE(model.data(model.index(0, 0), TrackListModel::AvailableRole).toBool(), true);
        QCOMPARE(model.headerData(TrackListModel::Year, Qt::Horizontal, Qt::DisplayRole).toString(), QString("Year"));

        // rows() reads any rows in one go, in the order asked.
        const auto rows = model.rows({999, 3, 500});
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].id, model.trackId(999));
        QCOMPARE(rows[1].id, model.trackId(3));
        QVERIFY(rows[2].path.startsWith(m_dir.path() + '/'));
        QVERIFY(rows[2].path.endsWith(".flac"));
    }

    void sortsByAColumnThenReversesThenGoesBack() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        const qint64 firstInLibraryOrder = model.trackId(0);
        QSignalSpy sorted(&model, &TrackListModel::sortChanged);

        model.sortBy(TrackListModel::Title);
        QCOMPARE(model.sortColumn(), int(TrackListModel::Title));
        QCOMPARE(cell(model, 0, TrackListModel::Title), QString("Track 00000"));
        QCOMPARE(cell(model, tracks - 1, TrackListModel::Title), QString("Track 00999"));
        QVERIFY(model.headerData(TrackListModel::Title, Qt::Horizontal, Qt::DisplayRole).toString().endsWith("▲"));

        model.sortBy(TrackListModel::Title);
        QVERIFY(model.sortDescending());
        QCOMPARE(cell(model, 0, TrackListModel::Title), QString("Track 00999"));

        model.sortBy(TrackListModel::Title);
        QCOMPARE(model.sortColumn(), -1);
        QCOMPARE(model.trackId(0), firstInLibraryOrder);
        QCOMPARE(sorted.count(), 3);

        // Numbers sort as numbers, and ties keep the library order.
        model.setSort(TrackListModel::Year, true);
        int last = 9999;
        const QList<int> all = [] { QList<int> r; for (int i = 0; i < tracks; ++i) r << i; return r; }();
        for (const auto &r : model.rows(all)) {
            QVERIFY(r.year <= last);
            last = r.year;
        }
        model.setSort(TrackListModel::Rating, false);
        QCOMPARE(model.data(model.index(0, 0), TrackListModel::RatingRole).toInt(), -1); // unrated first
        QCOMPARE(model.data(model.index(tracks - 1, 0), TrackListModel::RatingRole).toInt(), 100);
    }

    void filtersWithTheQueryLanguage() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        model.setFilter("year:1995");
        QCOMPARE(model.count(), count(*m_library, "year = 1995"));
        QCOMPARE(model.count(), tracks / 20);
        QVERIFY(model.filterError().isEmpty());

        model.setFilter("genre:jazz rating:3..");
        QCOMPARE(model.count(), count(*m_library, "genre = 'Jazz' AND rating >= 60"));
        const int good = model.count();

        // A half-typed query keeps the last good list and says what is wrong.
        model.setFilter("year:abc");
        QVERIFY(!model.filterError().isEmpty());
        QCOMPARE(model.count(), good);

        model.setFilter("");
        QVERIFY(model.filterError().isEmpty());
        QCOMPARE(model.count(), tracks);

        model.setFilter("\"Track 00042\"");
        QCOMPARE(model.count(), 1);
        QCOMPARE(cell(model, 0, TrackListModel::Title), QString("Track 00042"));
    }

    void selectsWithShiftAndCtrlAndKeepsItThroughASort() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        QSignalSpy changed(&model, &TrackListModel::selectionChanged);
        model.select(2);
        model.select(5, Qt::ShiftModifier);
        QCOMPARE(model.selectedCount(), 4);
        QCOMPARE(model.selectedRows(), QList<int>({2, 3, 4, 5}));
        QCOMPARE(model.currentRow(), 5);
        model.select(3, Qt::ControlModifier);
        QCOMPARE(model.selectedRows(), QList<int>({2, 4, 5}));
        model.select(7, Qt::ShiftModifier | Qt::ControlModifier); // adds 3..7 to the selection
        QCOMPARE(model.selectedRows(), QList<int>({2, 3, 4, 5, 6, 7}));
        QVERIFY(model.data(model.index(4, 1), TrackListModel::SelectedRole).toBool());
        QVERIFY(!model.data(model.index(8, 1), TrackListModel::SelectedRole).toBool());

        // The keyboard: Shift+Down extends from the anchor, a plain move selects one.
        model.moveCurrent(1, Qt::ShiftModifier);
        QCOMPARE(model.currentRow(), 8);
        QCOMPARE(model.selectedCount(), 6); // anchor is 3: rows 3..8
        model.moveCurrent(1);
        QCOMPARE(model.selectedRows(), QList<int>({9}));
        model.moveCurrent(-100);
        QCOMPARE(model.currentRow(), 0);

        // Selection follows the tracks, not the rows, through a sort.
        model.select(10);
        model.select(12, Qt::ShiftModifier);
        QSet<qint64> ids;
        for (int row : model.selectedRows())
            ids << model.trackId(row);
        model.sortBy(TrackListModel::Title);
        QCOMPARE(model.selectedCount(), 3);
        QSet<qint64> after;
        for (int row : model.selectedRows())
            after << model.trackId(row);
        QCOMPARE(after, ids);

        model.selectAll();
        QCOMPARE(model.selectedCount(), tracks);
        model.clearSelection();
        QCOMPARE(model.selectedCount(), 0);
        // With nothing selected, the cursor row is what actions act on.
        QCOMPARE(model.selectedRows().size(), 1);
        QVERIFY(changed.count() > 5);

        // A new search starts a new selection.
        model.select(1);
        model.setFilter("year:1990");
        QCOMPARE(model.selectedCount(), 0);
    }

    void marksThePlayingTrack() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        const QString path = model.path(42);
        QVERIFY(path.startsWith(m_dir.path()));
        model.setPlayingPath(path);
        QVERIFY(model.data(model.index(42, 1), TrackListModel::PlayingRole).toBool());
        QVERIFY(!model.data(model.index(41, 1), TrackListModel::PlayingRole).toBool());
    }

    // ---- Tree --------------------------------------------------------------

    void treeNodesScopeTheList() {
        LibraryTree tree;
        tree.setDatabase(m_library->db());
        TrackListModel model;
        model.setDatabase(m_library->db());
        auto choose = [&](const QString &key) {
            const int row = tree.rowOfKey(key);
            if (row < 0)
                return -1;
            model.setScope(tree.node(row).where, tree.node(row).params);
            return model.count();
        };

        QCOMPARE(tree.node(0).kind, QString("entire"));
        QCOMPARE(tree.node(0).count, qint64(tracks));
        const int top = tree.count();
        QVERIFY(top >= 10);

        // Artist, then an artist's albums.
        tree.expand(tree.rowOfKey("artists"));
        QCOMPARE(tree.count(), top + 5);
        QCOMPARE(choose("artist:1"), tracks / 5);
        const int artistRow = tree.rowOfKey("artist:1");
        QCOMPARE(tree.node(artistRow).label, QString("Artist 0"));
        tree.expand(artistRow);
        QCOMPARE(tree.node(artistRow + 1).kind, QString("album"));
        QCOMPARE(tree.parentRow(artistRow + 1), artistRow);
        model.setScope(tree.node(artistRow + 1).where, tree.node(artistRow + 1).params);
        QCOMPARE(model.count(), 2); // ten tracks per album, five artists take turns
        tree.collapse(tree.rowOfKey("artists"));
        QCOMPARE(tree.count(), top);

        // Album artist (one value per track) and the Album letters.
        tree.expand(tree.rowOfKey("albumartists"));
        QCOMPARE(choose("albumartist:artist 3"), tracks / 5);
        tree.collapse(tree.rowOfKey("albumartists"));
        tree.expand(tree.rowOfKey("albums"));
        QCOMPARE(tree.node(tree.rowOfKey("albums") + 1).label, QString("A"));
        QCOMPARE(choose("letter:A"), tracks / 5);
        tree.expand(tree.rowOfKey("letter:B"));
        QCOMPARE(tree.node(tree.rowOfKey("letter:B") + 1).kind, QString("album"));
        tree.collapse(tree.rowOfKey("albums"));

        // Genre.
        tree.expand(tree.rowOfKey("genres"));
        QCOMPARE(choose("genre:2"), count(*m_library, "genre = 'Jazz'"));
        tree.collapse(tree.rowOfKey("genres"));

        // Year: decades, then years.
        tree.expand(tree.rowOfKey("years"));
        QCOMPARE(choose("decade:1990"), count(*m_library, "year BETWEEN 1990 AND 1999"));
        tree.expand(tree.rowOfKey("decade:2000"));
        QCOMPARE(choose("year:2003"), tracks / 20);
        tree.collapse(tree.rowOfKey("years"));

        // Rating: whole stars from five down, then unrated.
        tree.expand(tree.rowOfKey("ratings"));
        const int ratings = tree.rowOfKey("ratings");
        QCOMPARE(tree.node(ratings + 1).label, QString("★★★★★"));
        QCOMPARE(choose("rating:10"), count(*m_library, "rating = 100"));
        QCOMPARE(choose("rating:none"), count(*m_library, "rating IS NULL"));
        tree.collapse(ratings);

        // Location: volumes (an offline one dimmed), then folders.
        tree.expand(tree.rowOfKey("locations"));
        const int online = tree.rowOfKey("volume:1"), offline = tree.rowOfKey("volume:2");
        QVERIFY(online > 0 && offline > 0);
        QVERIFY(!tree.node(online).dim);
        QVERIFY(tree.node(offline).dim);
        QVERIFY(tree.node(offline).label.contains("offline"));
        tree.expand(online);
        // The library folder, unless it is the whole volume.
        const QString folder = "folder:1:" + m_prefix;
        if (!m_prefix.isEmpty()) {
            QCOMPARE(tree.node(online + 1).label, m_prefix.chopped(1));
            tree.expand(online + 1);
            QCOMPARE(tree.node(online + 2).label, QString("Artist 0"));
        } else {
            QCOMPARE(tree.node(online + 1).label, QString("Artist 0"));
        }
        QCOMPARE(choose(folder + "Artist 2"), tracks / 5);
        tree.expand(tree.rowOfKey(folder + "Artist 2"));
        QCOMPARE(choose(folder + "Artist 2/Album 2"), 2); // tracks 22 and 27
        QCOMPARE(tree.node(tree.rowOfKey(folder + "Artist 2/Album 2")).expandable, false);

        // Reloading keeps what was open.
        const int before = tree.count();
        tree.reload();
        QCOMPARE(tree.count(), before);
        QVERIFY(tree.isExpanded(tree.rowOfKey(folder + "Artist 2")));

        // Playlists: Now Playing first.
        tree.expand(tree.rowOfKey("playlists"));
        QCOMPARE(tree.node(tree.rowOfKey("playlists") + 1).kind, QString("nowplaying"));
        // Collections come from the database (Music, Classical, ...).
        tree.expand(tree.rowOfKey("collections"));
        QVERIFY(tree.rowOfKey("collection:1") > 0);
        QCOMPARE(choose("collection:1"), tracks); // type:music
    }

    // ---- Column browser ------------------------------------------------------

    void browserCountsValuesAndNarrowsTheList() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        ColumnBrowser browser;
        browser.setDatabase(m_library->db());
        browser.setTracks(&model);
        // Off: no counts, no filtering.
        QCOMPARE(browser.genres()->count(), 0);
        browser.setEnabled(true);

        FacetModel *genres = browser.genres(), *artists = browser.artists(), *albums = browser.albums();
        QCOMPARE(genres->count(), 1 + 3); // All, Jazz, Pop, Rock (by name)
        QCOMPARE(genres->label(0), QString("All (1,000)"));
        QCOMPARE(genres->label(1), QString("Jazz"));
        QCOMPARE(genres->data(genres->index(1), FacetModel::CountRole).toInt(), count(*m_library, "genre = 'Jazz'"));
        QVERIFY(genres->isSelected(0));
        QCOMPARE(artists->title(), QString("Album Artist"));
        QCOMPARE(artists->count(), 1 + 5);
        QCOMPARE(albums->count(), 1 + 100);

        // Choosing a genre narrows the panes to its right and the list.
        browser.select(ColumnBrowser::GenrePane, genres->rowOfKey("2")); // Jazz
        const int jazz = count(*m_library, "genre = 'Jazz'");
        QCOMPARE(model.count(), jazz);
        QCOMPARE(artists->label(0), QString("All (%1)").arg(jazz));
        qint64 sum = 0;
        for (int i = 1; i < artists->count(); ++i)
            sum += artists->data(artists->index(i), FacetModel::CountRole).toLongLong();
        QCOMPARE(sum, qint64(jazz));
        QVERIFY(browser.filtering());

        // Ctrl adds a second genre.
        browser.select(ColumnBrowser::GenrePane, genres->rowOfKey("3"), Qt::ControlModifier); // Pop
        QCOMPARE(model.count(), count(*m_library, "genre IN ('Jazz', 'Pop')"));
        QCOMPARE(genres->selectedCount(), 2);

        // An album artist, then an album: every pane's choice applies.
        browser.select(ColumnBrowser::ArtistPane, artists->rowOfKey("artist 1"));
        QCOMPARE(model.count(), count(*m_library, "genre IN ('Jazz', 'Pop') AND sort_artist = 'Artist 1'"));
        QSqlQuery distinct(m_library->db());
        QVERIFY(distinct.exec("SELECT count(DISTINCT album_id) FROM tracks WHERE genre IN ('Jazz', 'Pop') AND sort_artist = 'Artist 1'") && distinct.next());
        QCOMPARE(albums->count(), 1 + distinct.value(0).toInt());
        const int albumRow = 1;
        const QString albumKey = albums->key(albumRow);
        browser.select(ColumnBrowser::AlbumPane, albumRow);
        QCOMPARE(model.count(), count(*m_library, "genre IN ('Jazz', 'Pop') AND sort_artist = 'Artist 1' AND album_id = " + albumKey));

        // A search recounts within it and keeps the choices still there.
        model.setFilter("year:1991");
        QVERIFY(genres->label(0).startsWith(QString("All (%1)").arg(count(*m_library, "year = 1991"))));
        QCOMPARE(model.count(), count(*m_library, "year = 1991 AND genre IN ('Jazz', 'Pop') AND sort_artist = 'Artist 1' AND album_id = " + albumKey) );
        model.setFilter("");

        // All clears a pane; clear() clears every pane.
        browser.select(ColumnBrowser::AlbumPane, 0);
        QCOMPARE(albums->selectedCount(), 0);
        browser.clear();
        QVERIFY(!browser.filtering());
        QCOMPARE(model.count(), tracks);

        // Artists instead of album artists: the multi-value table.
        browser.setArtistMode("artist");
        QCOMPARE(artists->title(), QString("Artist"));
        browser.select(ColumnBrowser::ArtistPane, artists->rowOfKey("3")); // Artist 2
        QCOMPARE(model.count(), tracks / 5);

        // Turning the browser off shows the whole node again.
        browser.setEnabled(false);
        QCOMPARE(model.count(), tracks);
    }

    void browserFollowsTheTreeNode() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        ColumnBrowser browser;
        browser.setDatabase(m_library->db());
        browser.setTracks(&model);
        browser.setEnabled(true);
        browser.select(ColumnBrowser::GenrePane, browser.genres()->rowOfKey("1")); // Rock
        const int rock = model.count();
        QSignalSpy reloads(&model, &TrackListModel::reloaded);
        // A node: the panes count within it, the list reloads once, and the
        // genre stays chosen.
        model.setScope("t.year BETWEEN ? AND ?", {1990, 1994});
        QCOMPARE(reloads.count(), 1);
        QCOMPARE(browser.genres()->label(0), QString("All (%1)").arg(count(*m_library, "year BETWEEN 1990 AND 1994")));
        QCOMPARE(model.count(), count(*m_library, "year BETWEEN 1990 AND 1994 AND genre = 'Rock'"));
        QVERIFY(model.count() < rock);
        QCOMPARE(browser.genres()->selectedCount(), 1);
        // A node without that genre drops the choice rather than show nothing.
        model.setScope("t.id = 2", {}); // track 1 is Jazz
        QCOMPARE(browser.genres()->selectedCount(), 0);
        QCOMPARE(model.count(), 1);
    }

    void browserStaysQuickAndUsesNoTableScanForEverything() {
        // The "everything" fast path counts genres from track_genres alone.
        TrackListModel model;
        model.setDatabase(m_library->db());
        ColumnBrowser browser;
        browser.setDatabase(m_library->db());
        browser.setTracks(&model);
        browser.setEnabled(true);
        QVERIFY(browser.lastQueryMs() < 200);
        QCOMPARE(browser.genres()->count(), 4);
    }

    // ---- Album grid ----------------------------------------------------------

    void albumGridListsTheAlbumsOfTheList() {
        TrackListModel model;
        model.setDatabase(m_library->db());
        AlbumGridModel grid;
        grid.setDatabase(m_library->db());
        grid.setTracks(&model);
        QCOMPARE(grid.count(), 0); // nothing until it shows
        grid.setActive(true);
        QCOMPARE(grid.count(), 100);
        QCOMPARE(grid.data(grid.index(0), AlbumGridModel::ArtistRole).toString(), QString("Artist 0"));
        QCOMPARE(grid.data(grid.index(0), AlbumGridModel::TrackCountRole).toInt(), 10);
        // The album the list is narrowed to, and the chosen album kept through a refresh.
        model.setFilter("year:1995");
        QSqlQuery q(m_library->db());
        QVERIFY(q.exec("SELECT count(DISTINCT album_id) FROM tracks WHERE year = 1995") && q.next());
        QCOMPARE(grid.count(), q.value(0).toInt());
        grid.setCurrentRow(2);
        const qint64 album = grid.currentAlbum();
        QSignalSpy resets(&grid, &QAbstractItemModel::modelReset);
        model.sortBy(TrackListModel::Title); // a sort doesn't change the albums
        QCOMPARE(resets.count(), 0);
        grid.refresh();
        QCOMPARE(grid.currentAlbum(), album);
        model.setFilter("");
    }

    // ---- Controller ----------------------------------------------------------

    void controllerFollowsTheChosenNode() {
        LibraryController controller(m_dir.filePath("library.db"));
        QVERIFY(controller.activate());
        QCOMPARE(controller.tracks()->count(), tracks);
        LibraryTree *tree = controller.tree();
        const int artists = tree->rowOfKey("artists");
        tree->expand(artists);
        const int artist = tree->rowOfKey("artist:2");
        controller.selectNode(artist);
        QCOMPARE(controller.currentKind(), QString("artist"));
        QCOMPARE(controller.tracks()->count(), tracks / 5);
        // Opening a node above the chosen one moves the chosen row with it.
        tree->expand(tree->rowOfKey("collections"));
        QCOMPARE(controller.currentNode(), tree->rowOfKey("artist:2"));
        // Closing the node that holds it chooses that node.
        tree->collapse(tree->rowOfKey("artists"));
        QCOMPARE(controller.currentNode(), tree->rowOfKey("artists"));
        QCOMPARE(controller.tracks()->count(), tracks);
    }

    void infoGoingToThingsAndRefreshing() {
        LibraryController controller(m_dir.filePath("library.db"));
        controller.setWatching(false);
        QVERIFY(controller.activate());
        TrackListModel *list = controller.tracks();

        // What the info panel shows.
        const QVariantMap info = controller.trackInfo(43); // i = 42
        QCOMPARE(info.value("title").toString(), QString("Track 00042"));
        QCOMPARE(info.value("artist").toString(), QString("Artist 2"));
        QCOMPARE(info.value("album").toString(), QString("E Album 4"));
        QCOMPARE(info.value("year").toInt(), 1992);
        QCOMPARE(info.value("genre").toString(), QString("Rock"));
        QCOMPARE(info.value("format").toString(), QString("FLAC"));
        QCOMPARE(info.value("bitrate").toInt(), 900);
        QCOMPARE(info.value("rating").toInt(), -1); // 42 % 6 == 0: unrated
        QCOMPARE(info.value("plays").toInt(), 0);   // 42 % 7
        QCOMPARE(info.value("durationMs").toLongLong(), qint64(102000));
        QVERIFY(info.value("path").toString().endsWith("Artist 2/Album 4/Track 00042.flac"));
        QVERIFY(info.value("online").toBool());
        QVERIFY(controller.trackInfo(0).isEmpty());
        QCOMPARE(controller.firstTrackOfAlbum(5), qint64(41)); // album 4: i = 40..49

        // The playing track is found by its path.
        list->setPlayingPath(info.value("path").toString());
        QCOMPARE(controller.playingTrackId(), qint64(43));

        // Going to an artist, an album and a track.
        list->setFilter("year:1990");
        int row = controller.showArtist(3);
        QVERIFY(row > 0);
        QCOMPARE(controller.currentKind(), QString("artist"));
        QCOMPARE(controller.tree()->key(row), QString("artist:3"));
        QCOMPARE(list->filter(), QString()); // the search is cleared
        QCOMPARE(list->count(), tracks / 5);
        row = controller.showAlbum(5);
        QCOMPARE(controller.tree()->key(row), QString("letter:E/album:5"));
        QCOMPARE(list->count(), 10);
        QSignalSpy reveal(&controller, &LibraryController::revealRow);
        // Track 7 (i = 6) isn't in album 5: the list widens to Entire Library.
        row = controller.showTrack(7);
        QVERIFY(row >= 0);
        QCOMPARE(controller.currentNode(), 0);
        QCOMPARE(list->trackId(row), qint64(7));
        QCOMPARE(list->selectedRows(), QList<int>({row}));
        QCOMPARE(reveal.count(), 1);

        // A refresh after the library changed keeps the node, the selection
        // and the cursor, and picks up the new track.
        list->select(10);
        list->select(12, Qt::ShiftModifier);
        QSet<qint64> selected;
        for (int r : list->selectedRows())
            selected << list->trackId(r);
        const qint64 cursor = list->trackId(list->currentRow());
        QSqlQuery q(m_library->db());
        QVERIFY(q.exec("INSERT INTO tracks (id, volume_id, rel_path, file_name, size, mtime_ns, title, sort_artist, "
                       "album, added) VALUES (5000, 1, 'new.flac', 'new', 1, 1, 'New', 'Artist 0', 'A Album 0', 1)"));
        QSignalSpy refreshed(list, &TrackListModel::aboutToRefresh);
        const int revision = controller.revision();
        controller.refreshAll();
        QCOMPARE(refreshed.count(), 1);
        QCOMPARE(list->count(), tracks + 1);
        QCOMPARE(controller.currentNode(), 0);
        QSet<qint64> after;
        for (int r : list->selectedRows())
            after << list->trackId(r);
        QCOMPARE(after, selected);
        QCOMPARE(list->trackId(list->currentRow()), cursor);
        QVERIFY(controller.revision() > revision);
        QVERIFY(q.exec("DELETE FROM tracks WHERE id = 5000"));
        list->setPlayingPath({});
    }

    void playNowQueueAndAppendHandTracksToThePlayer() {
        LibraryController controller(m_dir.filePath("library.db"));
        Player player; // no state file; muted, and the files don't exist
        player.setMuted(true);
        controller.setPlayer(&player);
        QVERIFY(controller.activate());
        TrackListModel *list = controller.tracks();

        list->select(3);
        list->select(5, Qt::ShiftModifier);
        QCOMPARE(controller.appendSelection(), 3);
        QCOMPARE(player.playlist()->count(), 3);
        QCOMPARE(player.playlist()->at(0).title, list->rows({3})[0].title);
        QCOMPARE(player.playlist()->at(0).durationMs, list->rows({3})[0].durationMs);

        list->select(10);
        QCOMPARE(controller.queueSelection(), 1);
        QCOMPARE(player.playlist()->count(), 4);
        QCOMPARE(player.playlist()->at(1).title, list->rows({10})[0].title); // after the current track (0)

        // Play now: the whole list, starting at the chosen row.
        QCOMPARE(controller.playRow(7), tracks);
        player.stop();
        QCOMPARE(player.playlist()->count(), tracks);
        QCOMPARE(player.currentIndex(), 7);
        QCOMPARE(player.playlist()->at(7).path, list->path(7));
    }

    void playNowTakesAWindowOfAHugeList() {
        QTemporaryDir dir;
        Library big(dir.filePath("big.db"));
        QVERIFY(big.open());
        fill(big, dir.path(), LibraryController::maxPlayNow + 1000);
        big.close();

        LibraryController controller(dir.filePath("big.db"));
        Player player;
        player.setMuted(true);
        controller.setPlayer(&player);
        QVERIFY(controller.activate());
        QCOMPARE(controller.playRow(500), LibraryController::maxPlayNow);
        player.stop();
        QCOMPARE(player.currentIndex(), 0);
        QCOMPARE(player.playlist()->at(0).path, controller.tracks()->path(500));
        // Near the end, the window ends at the last row.
        QCOMPARE(controller.playRow(1500), LibraryController::maxPlayNow);
        player.stop();
        QCOMPARE(player.currentIndex(), 500);
        QCOMPARE(player.playlist()->at(500).path, controller.tracks()->path(1500));
    }

    void scansOnAWorkerThread() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("needs ffmpeg for the fixtures");
        QTemporaryDir dir;
        QProcess make;
        make.start(QString(OMAMP_TESTS_DIR) + "/make-fixtures", {dir.filePath("music")});
        QVERIFY(make.waitForFinished(120000));
        QCOMPARE(make.exitCode(), 0);

        LibraryController controller(dir.filePath("library.db"));
        QVERIFY(controller.activate());
        QCOMPARE(controller.tracks()->count(), 0);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QSignalSpy job(&controller, &LibraryController::jobChanged);
        controller.addFolders({dir.filePath("music")});
        QVERIFY(controller.busy());
        // The window stays responsive: this thread is free while the scan runs.
        QVERIFY(finished.wait(60000));
        QVERIFY(!controller.busy());
        QVERIFY(job.count() >= 2);
        const QJsonObject result = finished.first().first().toJsonObject();
        QVERIFY(result.value("ok").toBool());
        QVERIFY(result.value("added").toInt() >= 12);
        QCOMPARE(controller.tracks()->count(), result.value("added").toInt());
        QVERIFY(controller.message().contains("added"));
        QVERIFY(controller.tree()->node(0).count > 0);

        // A rescan finds nothing new.
        finished.clear();
        controller.rescan();
        QVERIFY(finished.wait(60000));
        QCOMPARE(finished.first().first().toJsonObject().value("added").toInt(), 0);
    }
};

// The real window, offscreen, driven from the keyboard as a person would:
// Tab into advanced mode, / to search, arrows and Enter in the list, Tab back.
static void typeText(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

class AdvancedWindowTests : public QObject {
    Q_OBJECT

private slots:
    void advancedModeWorksFromTheKeyboard() {
        QTemporaryDir dir;
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            fill(library, dir.path(), 300);
        }
        Translator translator(dir.filePath("language"));
        Theme theme(dir.filePath("theme"), nullptr, dir.filePath("no-hyprctl"));
        Player player;
        player.setMuted(true);
        LibraryController library(dir.filePath("library.db"));
        library.setPlayer(&player);
        library.setArtCacheDir(dir.filePath("art"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &player);
        engine.rootContext()->setContextProperty("library", &library);
        engine.rootContext()->setContextProperty("commandPalette", library.palette());
        engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(dir.filePath("window.conf")));
        installNowPlaying(engine, &theme, &player, dir.filePath("library.db"), dir.filePath("art"));
        engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider(dir.filePath("library.db"), dir.filePath("art")));
        engine.load(QUrl("qrc:/Main.qml"));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(window->property("advanced").toBool(), false);
        QVERIFY(!library.ready()); // the deck alone never opens the library

        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(window->property("advanced").toBool());
        QVERIFY(library.ready());
        QCOMPARE(library.tracks()->count(), 300);
        QTRY_VERIFY(window->findChild<QObject *>("advancedView"));

        // / focuses the search; typing filters the list.
        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_VERIFY(window->activeFocusItem() && window->activeFocusItem()->inherits("QQuickTextInput"));
        typeText(window, "year:1995");
        QTRY_COMPARE(library.tracks()->count(), 15);
        // A half-typed query says what is wrong and keeps the list.
        typeText(window, " rating:");
        QTRY_VERIFY(!library.tracks()->filterError().isEmpty());
        QCOMPARE(library.tracks()->count(), 15);
        for (int i = 0; i < 8; ++i)
            QTest::keyClick(window, Qt::Key_Backspace);
        QTRY_VERIFY(library.tracks()->filterError().isEmpty());

        // Enter goes to the list; arrows move and Shift extends; Enter plays from there.
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!window->activeFocusItem()->inherits("QQuickTextInput"));
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(library.tracks()->currentRow(), 1);
        QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(library.tracks()->selectedCount(), 2);
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(library.tracks()->selectedRows(), QList<int>({1}));
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(player.playlist()->count(), 15);
        QCOMPARE(player.currentIndex(), 1);
        player.stop();

        // Left goes to the tree; Down and Right open Collections; the list follows.
        QTest::keyClick(window, Qt::Key_Left);
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(library.currentNode(), 1);
        QTest::keyClick(window, Qt::Key_Right);
        QVERIFY(library.tree()->isExpanded(1));
        QTest::keyClick(window, Qt::Key_Left);
        QVERIFY(!library.tree()->isExpanded(1));

        // The header: a click sorts, a drag on a column's edge resizes it.
        auto *header = window->findChild<QQuickItem *>("trackHeader");
        auto *table = window->findChild<QQuickItem *>("trackTable");
        QVERIFY(header && table);
        auto columnWidth = [&](int column) {
            qreal width = 0;
            QMetaObject::invokeMethod(table, "columnWidth", Q_RETURN_ARG(qreal, width), Q_ARG(int, column));
            return width;
        };
        // The Art column comes first, then # and Title.
        const qreal art = columnWidth(0), number = columnWidth(1), title = columnWidth(2);
        QVERIFY(art > 20 && number > 20 && title > 100);
        const QPoint titleMiddle = header->mapToScene(QPointF(art + number + title / 2, header->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, {}, titleMiddle);
        QTRY_COMPARE(library.tracks()->sortColumn(), int(TrackListModel::Title));
        const QPoint edge = header->mapToScene(QPointF(art + number - 1, header->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, edge);
        for (int dx = 10; dx <= 60; dx += 10)
            QTest::mouseMove(window, edge + QPoint(dx, 0));
        QTest::mouseRelease(window, Qt::LeftButton, {}, edge + QPoint(60, 0));
        QTRY_VERIFY(columnWidth(1) > number + 40);
        QCOMPARE(library.tracks()->sortColumn(), int(TrackListModel::Title)); // the drag didn't sort

        // Escape in the search clears it; Tab goes back to the deck.
        QTest::keyClick(window, Qt::Key_Slash);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(library.tracks()->filter(), QString());
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(!window->property("advanced").toBool());

        // The mode and the window size are remembered.
        window->close();
        // Settings writes after a short delay.
        auto saved = [&](const QString &key) { return QSettings(dir.filePath("window.conf"), QSettings::IniFormat).value(key); };
        QTRY_COMPARE(saved("window/advanced").toBool(), false);
        QVERIFY(saved("window/advancedWidth").toInt() > 0);
        QVERIFY(saved("window/columns").toString().contains("\"0\":"));
    }
};

// The Phase 1D parts of the window: the palette, the column browser, the
// album grid, the info panel and refreshing after the library changes.
class AdvancedPartsTests : public QObject {
    Q_OBJECT

    struct Window {
        QTemporaryDir &dir;
        Translator translator;
        Theme theme;
        Player player;
        LibraryController library;
        QQmlApplicationEngine engine;
        QQuickWindow *window = nullptr;

        explicit Window(QTemporaryDir &d)
            : dir(d), translator(d.filePath("language")), theme(d.filePath("theme"), nullptr, d.filePath("no-hyprctl")),
              library(d.filePath("library.db")) {
            player.setMuted(true);
            library.setPlayer(&player);
            library.setArtCacheDir(d.filePath("art"));
            engine.rootContext()->setContextProperty("translator", &translator);
            engine.rootContext()->setContextProperty("theme", &theme);
            engine.rootContext()->setContextProperty("backend", &player);
            engine.rootContext()->setContextProperty("library", &library);
            engine.rootContext()->setContextProperty("commandPalette", library.palette());
            engine.rootContext()->setContextProperty("settingsUrl", QUrl::fromLocalFile(d.filePath("window.conf")));
            installNowPlaying(engine, &theme, &player, d.filePath("library.db"), d.filePath("art"));
            engine.addImageProvider(ArtImageProvider::name,
                                    new ArtImageProvider(d.filePath("library.db"), d.filePath("art")));
        }
        bool load() {
            engine.load(QUrl("qrc:/Main.qml"));
            if (engine.rootObjects().size() != 1)
                return false;
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            return window && QTest::qWaitForWindowExposed(window);
        }
        // Items made by a Repeater have no QObject parent, so this also
        // walks the visual tree.
        static QObject *findItem(QQuickItem *item, const QString &name) {
            if (item->objectName() == name)
                return item;
            for (QQuickItem *child : item->childItems())
                if (QObject *o = findItem(child, name))
                    return o;
            return nullptr;
        }
        QObject *find(const char *name) const {
            QObject *o = window->findChild<QObject *>(name);
            if (!o)
                o = findItem(window->contentItem(), QString::fromLatin1(name));
            if (!o)
                qWarning("No object named %s in the window", name);
            return o;
        }
        QVariant saved(const QString &key) const {
            return QSettings(dir.filePath("window.conf"), QSettings::IniFormat).value(key);
        }
    };

    static void startInAdvancedMode(QTemporaryDir &dir) {
        QSettings conf(dir.filePath("window.conf"), QSettings::IniFormat);
        conf.setValue("window/advanced", true);
    }

private slots:
    void paletteBrowserGridAndInfoFromTheKeyboard() {
        QTemporaryDir dir;
        {
            Library library(dir.filePath("library.db"));
            QVERIFY(library.open());
            fill(library, dir.path(), 300);
        }
        startInAdvancedMode(dir);
        Window w(dir);
        QVERIFY(w.load());
        QQuickWindow *window = w.window;
        LibraryController &library = w.library;
        QTRY_VERIFY(library.ready());
        QTRY_VERIFY(w.find("advancedView"));

        // Ctrl+K opens the palette; an artist's name and Enter go to that
        // artist in the tree.
        QObject *palette = w.find("commandPalette");
        QVERIFY(palette);
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(palette->property("opened").toBool());
        // The palette lists the window's actions before anything is typed.
        QVERIFY(library.palette()->actionIds().contains("browser"));
        typeText(window, "Artist 3");
        QTRY_VERIFY(library.palette()->count() > 0);
        QCOMPARE(library.palette()->get(0).value("kind").toString(), QString("artist"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!palette->property("opened").toBool());
        QCOMPARE(library.tree()->key(library.currentNode()), QString("artist:4"));
        QCOMPARE(library.tracks()->count(), 60);

        // An action from the palette: "> browser" shows the column browser.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(palette->property("opened").toBool());
        typeText(window, ">column browser");
        QTRY_COMPARE(library.palette()->get(0).value("itemId").toString(), QString("browser"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(library.browser()->enabled());
        QCOMPARE(library.browser()->genres()->label(0), QString("All (60)"));
        QVERIFY(w.find("columnBrowser")->property("visible").toBool());

        // In the browser: Down chooses the first genre, and the list narrows.
        QTRY_VERIFY(window->activeFocusItem() && w.find("browserPane0")->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(library.browser()->genres()->selectedCount(), 1);
        const QString genre = library.browser()->genres()->label(1); // Jazz
        int expected = 0;
        for (int i = 0; i < 300; ++i)
            expected += (i % 5 == 3 && genres[i % 3] == genre);
        QCOMPARE(library.tracks()->count(), expected);
        // Right moves to the next pane, Enter to the list; Down puts the cursor on the first track.
        QTest::keyClick(window, Qt::Key_Right);
        QVERIFY(w.find("browserPane1")->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(library.tracks()->currentRow(), 0);

        // I shows the info panel for the track under the cursor.
        QTest::keyClick(window, Qt::Key_I);
        QObject *info = w.find("infoPanel");
        QTRY_VERIFY(info && info->property("visible").toBool());
        const QString title = library.tracks()->data(library.tracks()->index(0, TrackListModel::Title), Qt::DisplayRole).toString();
        QTRY_COMPARE(w.find("infoTitle")->property("text").toString(), title);
        QVERIFY(w.find("infoPath")->property("text").toString().endsWith(".flac"));

        // G shows the albums of the list as a grid; Enter plays the chosen one.
        QTest::keyClick(window, Qt::Key_G);
        QTRY_VERIFY(library.albumGrid()->active());
        QVERIFY(w.find("albumGrid")->property("visible").toBool());
        QVERIFY(library.albumGrid()->count() > 0);
        QTest::keyClick(window, Qt::Key_Right); // the first album
        QCOMPARE(library.albumGrid()->currentRow(), 0);
        const qint64 album = library.albumGrid()->currentAlbum();
        // The info panel follows the grid.
        QTRY_COMPARE(w.find("infoTitle")->property("text").toString(),
                     library.trackInfo(library.firstTrackOfAlbum(album)).value("title").toString());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(w.player.playlist()->count(), 10); // the whole album, not just this genre
        w.player.stop();
        QTest::keyClick(window, Qt::Key_G);
        QTRY_VERIFY(!library.albumGrid()->active());

        // A refresh (as after the folder watcher wrote) keeps the scroll position.
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier); // browser off: all 60 tracks
        QTRY_VERIFY(!library.browser()->enabled());
        library.selectNode(0);
        QCOMPARE(library.tracks()->count(), 300);
        auto *table = qobject_cast<QQuickItem *>(w.find("trackTable"));
        QVERIFY(table);
        table->setProperty("contentY", 120 * 24.0 + 5); // rows are 24 pixels
        QTRY_COMPARE(table->property("topRow").toInt(), 120);
        library.refreshAll();
        QTRY_COMPARE(table->property("topRow").toInt(), 120);
        QCOMPARE(table->property("contentY").toDouble(), 120 * 24.0 + 5);

        // The parts shown are remembered.
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        typeText(window, ">info panel");
        QTRY_COMPARE(library.palette()->get(0).value("itemId").toString(), QString("info"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!info->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        w.window->close();
        QTRY_COMPARE(w.saved("window/browserVisible").toBool(), true);
        QCOMPARE(w.saved("window/infoVisible").toBool(), false);
        QCOMPARE(w.saved("window/gridVisible").toBool(), false);
    }

    void watcherChangesRefreshTheWindowAndKeepTheSelection() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("needs ffmpeg for the fixtures");
        QTemporaryDir dir;
        QProcess make;
        make.start(QString(OMAMP_TESTS_DIR) + "/make-fixtures", {dir.filePath("music")});
        QVERIFY(make.waitForFinished(120000));
        QCOMPARE(make.exitCode(), 0);
        startInAdvancedMode(dir);

        Window w(dir);
        LibraryController &library = w.library;
        QVERIFY(library.activate());
        QSignalSpy scanned(&library, &LibraryController::scanFinished);
        library.addFolders({dir.filePath("music")});
        QVERIFY(scanned.wait(60000));
        QVERIFY(w.load());
        QTRY_VERIFY(w.find("advancedView"));
        // The watcher watches the new folder (the scan reloads it).
        QTRY_VERIFY(library.watcher() && library.watcher()->watchedFolders() > 0);
        QObject *status = w.find("watchStatus");
        QVERIFY(status);
        QTRY_VERIFY(status->property("visible").toBool());

        TrackListModel *list = library.tracks();
        const int before = list->count();
        QVERIFY(before >= 12);
        list->select(1);
        list->select(3, Qt::ShiftModifier);
        QSet<qint64> selected;
        for (int r : list->selectedRows())
            selected << list->trackId(r);
        const qint64 cursor = list->trackId(list->currentRow());

        // A new file appears in a watched folder: the window refreshes on
        // its own, with the new track and the same selection.
        QSignalSpy changed(library.watcher(), &Watcher::libraryChanged);
        QVERIFY(QFile::copy(dir.filePath("music/Loose/untagged track.mp3"), dir.filePath("music/Loose/a copy.mp3")));
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 30000);
        QTRY_COMPARE(list->count(), before + 1);
        QSet<qint64> after;
        for (int r : list->selectedRows())
            after << list->trackId(r);
        QCOMPARE(after, selected);
        QCOMPARE(list->trackId(list->currentRow()), cursor);
        QCOMPARE(library.tree()->node(0).count, qint64(before + 1));
        w.window->close();
    }
};

int runLibraryWindowTests(int argc, char **argv) {
    LibraryWindowTests tests;
    AdvancedWindowTests window;
    AdvancedPartsTests parts;
    return QTest::qExec(&tests, argc, argv) + QTest::qExec(&window, argc, argv) + QTest::qExec(&parts, argc, argv);
}

#include "librarywindow_tests.moc"
