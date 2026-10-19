# Progress

## Phase 0: the core (20 October 2026)

Phase 0 is built. `omamp scan` fills a library, a rescan of an unchanged
library only checks sizes and modification times, `omamp query` searches it,
and the player works as before. All tests pass (11 player tests, 16 library
tests). Nothing is pushed; the work is on a branch for review.

### What was built

**Code layout.** The source is split as the plan says:

```text
src/core/    library.{h,cpp}     the database: open, WAL, migrations, backups, settings, stats
             migrations.{h,cpp}  the schema, as numbered steps
             volumes.{h,cpp}     mountinfo parsing, volume identity, where a volume is now
             tagreader.{h,cpp}   TagLib 2 reading, multi-value splitting, content type
             scanner.{h,cpp}     walk, compare, read on a thread pool, write in batches
             query.{h,cpp}       the query language: parser, field table, SQL compiler, runner
             job.{h,cpp}         progress and cancel for long work
             core.pri            Qt Core + Qt SQL + taglib (pkg-config)
src/engine/  player, playlist, spectrum (QMediaPlayer), engine.pri
src/app/     main.cpp, mpris, portal, theme, translator, the QML and resources
src/cli/     cli.{h,cpp}, cli.pri
tests/       omamp_tests.cpp (player), library_tests.cpp (core), main.cpp (runs both),
             make-fixtures, bench/
bin/bench    the performance run
```

`omamp.pro` includes `core.pri`, `engine.pri` and `cli.pri`; the tests
include the same files. The core needs no GUI module, so the benchmark links
only Qt Core and Qt SQL.

The player no longer includes the portal code. `Player::openFolder()` emits
`folderRequested()`, and `main.cpp` connects that to `FolderPicker` and the
picker's results to `Player::addPicked()`. This keeps D-Bus out of the engine.

**Database.** `~/.local/share/omamp/library.db` (it follows `XDG_DATA_HOME`;
every command takes `--db`). WAL mode, `synchronous=NORMAL`, foreign keys on,
a 5-second busy timeout, and the `REGEXP` function from Qt's SQLite driver.
Tables: `volumes`, `folders`, `tracks`, `artists`, `track_artists`, `genres`,
`track_genres`, `albums`, `playlists`, `playlist_items`, `collections` (with
Music, Classical, Audiobooks and Podcasts made at creation), `play_history`,
`tag_journal`, `artwork`, `jobs`, `settings`, and `tracks_fts`.

**Migrations.** `PRAGMA user_version` holds the schema version. Step 1 makes
the tables; step 2 adds the search index and its triggers and indexes any
existing rows. Before migrating a database that already has a version, Omamp
copies it with `VACUUM INTO` to `library.db.v<old>-<UTC time>.bak`. A
database from a newer Omamp is refused, not opened.

**Volumes.** See "Decisions" below for the rules. A local filesystem's key is
`uuid:<filesystem UUID>` (plus `:<subvolume root>` for a btrfs subvolume or a
bind mount), a network share's key is `marker:<id>` or `net:<source>`, and a
filesystem with no UUID (tmpfs, overlay) gets `mount:<type>:<source>:<root>@<mount point>`.
On this machine `/home` comes out as
`uuid:bccdb82b-…:/@home`. Track paths are stored relative to the volume
root. `Library::refreshVolumes()` checks every volume and marks its tracks
offline or back online; missing tracks stay missing until a scan finds them.

**Scanner.** POSIX `opendir`/`fstatat` walk (nanosecond modification times),
hidden entries skipped, linked files followed, linked folders not followed,
other filesystems mounted inside a root not crossed. Unchanged files (same
size and `mtime_ns`) are never opened. New and changed files are read with
TagLib on a `QThreadPool` of 4 to 8 threads and written in batches of about
500, one transaction each. Each scan is a row in `jobs`.

**Command line.** `omamp scan [folder...]` (no folder means rescan every
library folder), `omamp query <text>`, `omamp stats`, `omamp help`,
`omamp --version`. Each takes `--json`, prints results on stdout and errors
on stderr, and exits 0 or 1. Commands run under `QCoreApplication`, so they
need no display. `omamp`, `omamp <files>` and `omamp open <files>` open the
window as before.

**Fixtures.** `tests/make-fixtures <folder>` uses `ffmpeg` to make 14 small
files: a two-disc FLAC album with a guest artist and a composer; a
compilation across MP3, Ogg Vorbis and Opus with multi-value genres and a
Christmas track; Unicode folders, file names and tags (M4A, WAV); files with
no tags and with a title only; an M4B audiobook; a broken MP3; a JPEG; and a
hidden folder. The tests build them once per run in a temporary folder.

**Tests.** `bin/test` builds one binary that runs both test classes
offscreen. The library tests cover: a new database's schema and WAL mode; a
version 1 database upgraded with a working backup and the old rows indexed;
a newer schema refused; mountinfo parsing; a disk known by UUID wherever it
mounts, and btrfs subvolumes; a share known by its marker, a read-only share
falling back to its server name, and a share found after a remount
elsewhere; tag value parsing; a full scan of the fixtures; a rescan that
skips unchanged files (proved by changing a title in the database and
seeing it survive) and rereads a file whose time changed (the rating
survives); missing versus offline, including a rescan refused while the disk
is away and paths following a remount; multi-value artists, album artists,
composers and genres; search; the parser; and the CLI's JSON.

The volume tests use a fake `mountinfo` file and a fake `by-uuid` folder, so
they can plug, unplug and move disks.

### Test results

```text
Totals: 11 passed, 0 failed, 0 skipped, 0 blacklisted, 64ms     (player)
Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 493ms    (library)
```

### Benchmark

`bin/bench` on tetelestai (Ryzen 9 7950X3D, btrfs on LUKS), with
another heavy job running. Database and files on disk under `build/bench`.
Times are the median of 7 runs with a warm page cache.

Generating 200,000 tracks (18,278 albums, 1,600 artists): 13.8 s, peak
memory 49 MB. The database is 191 MB.

| What | ms | Rows |
|---|---:|---:|
| Open the library | 0.6 | |
| First page: count + 100 sorted rows | 1.7 | 200,000 |
| `coast` (search index) | 11.7 | 14,591 |
| `midnight highway` (two words) | 22.3 | 1,144 |
| `go` (too short for the index; LIKE) | 77.7 | 43,858 |
| `artist:"neon coast" year:1997..2001` | 11.2 | 751 |
| `rating:4.. -genre:christmas` | 43.9 | 30,918 |
| `genre:rock OR genre:metal` | 9.7 | 17,443 |
| `artist:="Neon Coast"` (multi-value table) | 3.8 | 10,136 |
| `title:/^love/` (regular expression) | 49.3 | 3,542 |
| Every row, sorted, loaded into memory | 432.1 | 200,000 |
| `stats` | 40.6 | |

Each query time includes a page of 100 rows and the total count. Peak memory
for the whole run, including all 200,000 rows loaded at once: 216 MB.

Scanning 100,000 files (hard links of one small tagged MP3, on btrfs):

| What | ms |
|---|---:|
| First scan, TagLib on 8 threads | 5,842 |
| Rescan, nothing changed (stat only) | 401 |

Peak memory during the scan: 242 MB.

Against the plan's targets: opening is far under a second, every search and
filter is under 100 ms, and an unchanged 100,000-file rescan takes 0.4 s
against a 30-second target. Caveats: the page cache was warm, and the first
scan read the same few file contents over and over, so a real first scan of
a real library will be slower (disk reads, larger tags, artwork). A cold or
NAS rescan was not measured.

### Decisions

- **Volume identity follows the plan, with three additions.** A btrfs
  subvolume or bind mount adds its root to the UUID key, because one UUID
  can be mounted several times with different contents. A share's marker is
  looked for from the added folder up to the mount point, so adding a
  subfolder of a known share reuses that share's volume. A filesystem with no
  UUID gets a key from its mount, which is the best available.
- **A marker is written only by `omamp scan <folder>`** on a network
  filesystem, and only if the folder is writable. Looking up a path never
  writes. The marker is written atomically and names itself in a comment.
- **A marker counts at its last place only while a share is mounted there.**
  An unmounted mount point can still hold a stale copy of the folder.
  Otherwise Omamp tries the same subfolder under every network mount, which
  finds a share remounted somewhere else.
- **A scan never crosses into another filesystem** mounted inside a root (as
  `find -xdev`). It reports how many it skipped; add them as roots of their
  own. This keeps every track on the volume that really holds it.
- **What "offline" protects.** A scan fails outright if a root is missing or
  cannot be listed. Files under a subfolder that cannot be listed are not
  marked missing. Because tracks belong to a volume, an empty mount point
  (share not mounted) resolves to the local disk's volume and cannot mark the
  share's tracks missing.
- **Unreadable files are still added**, by file name, and counted as failed.
  Otherwise every rescan would try them again, and a rescan would no longer
  cost one stat per file.
- **Multi-value tags.** Separate values in the file (ID3v2.4, repeated Vorbis
  comments) are kept, and a single value is also split on `;`, MediaMonkey's
  separator. `&` and "feat." are not split, because they belong to names like
  "Simon & Garfunkel". The track row keeps the joined text (`A; B`) for
  display and search; `track_artists` holds each value with its role.
- **Genres get their own table** (`genres`, `track_genres`) instead of a role
  in `track_artists`, because a genre is not a person and the artist tables
  should list only people.
- **Albums** are keyed by album artist and title. A compilation with no album
  artist goes under "Various Artists". With no album artist at all, the
  folder stands in for it, so an album whose tracks each name a different
  guest stays one album.
- **Ratings** are stored 0 to 100 (20 per star; NULL means unrated), as
  MediaMonkey and the POPM and FMPS tags do. The query language works in
  stars: `rating:4` means 4 stars, give or take half a star.
- **The search index** covers title, artist, album, album artist, composer,
  genre and the file name (without its folder). The file name lets files
  with no tags be found. The folder path is left out, so a word like "music"
  doesn't match every track; `path:` searches it with LIKE.
- **Words shorter than three characters** fall back to LIKE over the same
  columns, since the trigram index cannot answer them. The LIKE fallback is
  case-insensitive for ASCII only and does not ignore accents; the index
  does both.
- **OR binds tighter than AND**, as in MediaMonkey: `rating:4..
  genre:rock OR genre:metal` means rating and (rock or metal). Regular
  expressions are case-insensitive unless they say otherwise.
- **An unknown field is an error** that lists the known fields, rather than
  being searched as a plain word. Scripts and agents get told instead of
  getting silently wrong results.
- **CLI errors** go to stderr, and with `--json` also to stdout as
  `{"ok": false, "error": "..."}`, so an agent reading stdout sees them.
- **Command words win over paths.** `omamp scan` is always the command. To
  open a file named `scan`, use `omamp ./scan` or `omamp open scan`. One
  change from before: a first argument of exactly `open` is now taken as the
  word, not a file.
- **`bin/build` and `bin/test` honour `JOBS`** (`JOBS=6 bin/build`); the
  default is still every core.
- **The PKGBUILD** now depends on `taglib` and `sqlite` and builds with
  `pkgconf`.

### Known gaps

- **No folder watching yet.** inotify is not wired up. A rescan is cheap
  (`omamp scan` with no folder rescans every library folder), so this can
  wait for the window, which will run it.
- **No scan in the window.** The window does not use the library yet. Phase 1
  runs the scanner on a worker thread with its own `Library` connection.
- **Folder modification times are stored but not yet used** to skip walking
  unchanged folders on a NAS (plan step 4 of scanning; Phase 5).
- **Artwork, lyrics, ratings and play counts in tags** are not read. The
  `artwork` table exists and is empty.
- **Query language gaps:** no dates (`added:<30d`, `played:`), no sort or
  limit clauses, no `field:none` for empty fields, no relative ranges. The
  parser and field table are ready for them (below).
- **A scan reads, then writes.** Each tag-reading task holds a copy of its
  file's details until it runs, which is why 100,000 files peak at 242 MB.
  Feeding the pool in chunks would flatten this.
- **Cancel** stops reading and skips the missing pass, but the job row and
  the `Job` only report it; nothing in the window can cancel yet.
- **A share with no marker that changes server name** gets a new volume.
  Re-pointing a volume by hand (MediaMonkey's "re-assign drive") is not built.
- **Files moved between folders** come back as one missing track and one new
  one; ratings and history don't follow. Matching by size and tags is a
  later job.
- **Not measured:** cold-cache scans, a real NAS, and a real library on this
  machine. `~/Music` here holds only two video clips.
- **The GUI was checked offscreen only** (it loads and draws with a muted
  player); it was not opened on screen.

### What Phase 1 needs to know

**Opening the library.**

```cpp
Library library;                       // or Library(path) in tests
QString error;
if (!library.open(&error)) { ... }     // creates, migrates, backs up
library.db();                          // the QSqlDatabase for this thread
library.refreshVolumes(VolumeResolver());
LibraryStats s = library.stats();
QString path = library.absolutePath(trackId);   // empty when offline
```

A `Library` is one connection and belongs to the thread that opened it. For
a scan in the window, open a second `Library` on the worker thread. WAL lets
the window keep reading while the scan writes.

**Scanning.**

```cpp
Job job("scan");                       // progressChanged(phase, done, total), finished()
Scanner scanner(library, VolumeResolver(), &job);
ScanResult r = scanner.scan({"/home/matt/Music"});   // or scanner.rescanAll()
job.cancel();                          // from any thread
Scanner::roots(library);               // the library folders, "" when offline
```

`scan()` blocks the calling thread. `ScanResult::toJson()` is what
`omamp scan --json` prints.

**Querying.**

```cpp
Query::Parsed p = Query::parse(text);  // a tree of Query::Node, or an error and position
Query::Compiled c = Query::compile(p.root);   // SQL condition over "tracks t" + parameters
Query::Options o;
o.limit = 100;                         // 0 = no limit; o.offset for later pages
Query::Result r = Query::run(library, text, o);
// r.tracks (Query::Track), r.total (count without the limit), r.error, r.elapsedUs
```

For the track list, use `Query::compile()` and build a SQL model that
selects only the visible columns, a page at a time, ordered by
`t.sort_artist COLLATE NOCASE, t.album COLLATE NOCASE, t.disc_no, t.track_no`
(the `tracks_order` index). Loading all 200,000 rows takes 430 ms; one page
takes under 2 ms.

To add a field, add a line to `Query::fields()`. To add an operator or a
value kind (dates, `none`), extend `Parser::parseValue()` and
`Compiler::term()`. To add a column to the search index, write a new
migration that drops and rebuilds `tracks_fts` and its triggers. Never edit
a released migration.

**Schema notes.** `tracks.availability` is `online`, `offline` or `missing`.
`tracks.sort_artist` is the album artist, or the artist, or "Various
Artists". `tracks.file_name` is the name without its folder or extension.
Times are Unix seconds except `mtime_ns`. `track_artists.role` is `artist`,
`album_artist` or `composer`.

**Running the benchmark.** `JOBS=6 bin/bench` (about two minutes; `--tracks`
and `--files` change the sizes). Everything goes in `build/bench`.

## Phase 1A: the advanced-mode window, tree and track list (20 October 2026)

`Tab` now switches between the deck and advanced mode. Advanced mode shows
the deck as a strip across the top and the library below it: the tree on the
left, the track list on the right, a search box above them, and a status
line with the running job below. The deck itself is unchanged. All tests
pass, including one that drives the real window offscreen from the keyboard.
Nothing is pushed.

![Advanced mode, made-up library](../advanced-mode.png)

### What was built

```text
src/app/librarycontroller.{h,cpp}  `library` in QML: opens the library on first use, owns the
                                   tree and the list, runs scans on a worker thread, hands
                                   tracks to the player
src/app/librarytree.{h,cpp}        the tree as a flat list model, children read on expand
src/app/tracklistmodel.{h,cpp}     the track list: ordered ids, rows read a page at a time
src/app/devhooks.{h,cpp}           OMAMP_PROFILE, OMAMP_SCREENSHOT, OMAMP_DEMO (below)
src/app/AdvancedView.qml           the layout: strip, actions and search, tree | list, status
src/app/DeckStrip.qml, Analyser.qml  the deck as one strip
src/app/LibraryTreePane.qml        the tree view and its keys
src/app/TrackTable.qml             TableView + HorizontalHeaderView, menus, keys
src/app/NowPlayingList.qml         Now Playing, chosen under Playlists
src/app/JobsBar.qml                track count, selection, job progress and Cancel, messages
src/engine/playlist, player        insertTracks/replaceTracks; playTracks, queueNext, appendTracks
tests/librarywindow_tests.cpp      the models, the controller, and the window from the keyboard
```

**The track list.** A change of node, search or sort runs one query that
returns only the matching track ids, in order (`SELECT t.id FROM tracks t
WHERE <node> AND <search> ORDER BY …`). The model keeps that id list (8 bytes
a track) and reads rows in pages of 128 with `WHERE t.id IN (…)` when the
view asks for them, keeping the 48 most recently used pages. So the view can
jump anywhere at once, and no query loads every row. Every sort ends with
the library order and the id, so ties keep albums together.

Selection is a set of track ids, so it survives a re-sort. Changes to the
selection and the playing mark are signalled for the rows on screen only:
TableView handles a `dataChanged` over every row one cell at a time, which
took 4 seconds at 200,000 rows. A reset first scrolls the view to the top,
then back to the cursor row. TableView left at the bottom of a list it no
longer had rebuilt itself row by row and hung for tens of seconds.

**The tree.** A flat list of the visible rows, each with a depth, drawn by a
plain ListView. Each node carries its scope as a SQL condition over
`tracks t` with parameters. Children are read when a node opens and dropped
when it closes. After a scan the tree reloads and reopens what was open.

| Node | Children | Scope |
|---|---|---|
| Entire Library | | everything (with the track count) |
| Collections | rows of `collections` | the collection's query, compiled |
| Artist | `track_artists` role `artist`, then that artist's albums | `track_artists` |
| Album Artist | distinct `sort_artist`, then albums | `t.sort_artist = ?` |
| Album | first letters (`#` for the rest), then albums | `t.album_id = ?` |
| Genre | `genres` | `track_genres` |
| Year | decades, then years; Unknown year | `t.year` ranges |
| Rating | 5 stars down to half a star; Unrated | `t.rating` ranges |
| Composer | `track_artists` role `composer` | `track_artists` |
| Location | volumes (offline ones dimmed), library folders, subfolders | `t.volume_id` and a `rel_path` range |
| Playlists | Now Playing, then rows of `playlists` | `playlist_items`, or the auto-playlist's query |

**Scanning.** `library.addFolder()` opens the portal folder picker. It is a
second `FolderPicker`, so its folders go to the library, not to Now Playing.
`addFolders()` and `rescan()` run `Scanner` on a `QThread` with its own
`Library` connection. The `Job`'s progress reaches the status line through
queued signals, and Cancel calls `Job::cancel()`. When the scan ends, the
tree and the list reload and the status line says what changed ("Scan
finished: 34 added").

**Playing.** Enter or a double-click is MediaMonkey's "play now": Now Playing
becomes the visible list and plays from that track. The tags come from the
library, so nothing is read from disk. Offline and missing tracks are left
out. A list of more than 5,000 tracks hands over 5,000, starting at that
track (or the last 5,000 when it is near the end), so Now Playing stays quick
to save in `state.json`. Play next inserts after the current track; Add to
Now Playing appends. Show in folder runs `xdg-open` on the track's folder.

**Window state.** `~/.config/omamp/window.conf` (QtCore `Settings`) keeps
the mode, each mode's window size, the tree width and the columns (which are
hidden, and widths set by hand). The library opens the first time advanced
mode shows, so the deck alone never touches the database.

**Keys.** Advanced mode turns off the deck's arrow, Enter and Delete
shortcuts, so the tree and the list get those keys. `O` adds a folder to the
library, `/` and `Ctrl+F` search, `F5` rescans. The `?` overlay shows the
player's keys and the library's side by side, and the README lists them all.

### Test results

```text
Totals: 11 passed, 0 failed, 0 skipped, 0 blacklisted, 95ms      (player)
Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 465ms     (library)
Totals: 12 passed, 0 failed, 0 skipped, 0 blacklisted, 783ms     (library window models)
Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 1438ms     (the window, offscreen)
```

The model tests cover paging (each of 1,000 rows matches its id), sorting
(ascending, descending, back to library order; ties in library order;
unrated first), filtering (counts checked against SQL; a half-typed query
keeps the last list), selection (Shift, Ctrl, Ctrl+Shift, the keyboard,
through a sort), the playing mark, every tree node's scope against a SQL
count, the chosen node following rows that open above it, play now, play
next, add to Now Playing, the 5,000-track window, and a scan on the worker
thread. The window test loads the real `Main.qml` offscreen and presses
keys: `Tab` in, `/`, typing a search and then a broken one, `Enter`, arrows,
`Shift+↓`, `Enter` to play, `←` to the tree, `→` and `←` to open and close a
node, a header click to sort, a header-edge drag to resize, `Esc`, `Tab` out,
and the saved settings.

### Performance

The Phase 0 generator made the 200,000-track library (`omamp_bench
generate`, 191 MB). The window ran offscreen with the software renderer, a
throwaway `HOME`, no session bus and no audio, on tetelestai while another
heavy job ran. Times run from the change to the next frame on screen
(`OMAMP_PROFILE=1`), over three runs.

| What | ms to the screen | Query alone (ms) |
|---|---:|---:|
| Start straight into advanced mode, to the first frame | 270–420 | 14 (200,000 ids) |
| Start on the deck, to the first frame | 105–111 | |
| `Tab` into advanced mode (opens the library) | 127–135 | |
| Jump to the last row | 61–67 | |
| Sort by Title | 196–261 | 180–210 |
| Sort by Title, descending | 223–298 | 187 |
| Search `coast` (search index) | 71–79 | 18 |
| Search `go` (two letters: LIKE, no index) | 169–191 | about 150 |
| Search `artist:"neon coast" year:1997..2001` | 65–72 | 7 |
| Clear the search (200,000 rows, still sorted by Title) | 225–269 | 200–230 |

Peak memory for the whole run: 255 MB.

Against the plan's targets, the 200,000-track library opens well under a
second, and indexed searches and tree nodes reach the screen in under
100 ms. About 50 ms of each figure is the software renderer drawing a
1,400 × 860 window. Two things miss 100 ms:

- **Sorting by a text column** (Title, Artist, Album, Genre) costs about
  200 ms at 200,000 rows, all of it SQLite sorting. Number columns are fast.
  Indexes on `title`, `artist`, `album` and `genre` (each `COLLATE NOCASE`)
  would bring text sorts to about 20 ms; that is a schema change, so it is
  left for the coordinator and 1C (below). The other fix is to run the id
  query on a second connection on a worker thread so the window never waits,
  a contained change in `TrackListModel::reload()`.
- **One- and two-letter searches** fall back to LIKE (a Phase 0 decision) and
  take about 150 ms. That belongs to the query language (1B).

To rerun it, make the database with `bin/bench`, then:

```sh
mkdir -p /tmp/o/data/omamp /tmp/o/config/omamp
cp build/bench/library.db /tmp/o/data/omamp/
printf '[window]\nadvanced=true\n' > /tmp/o/config/omamp/window.conf
HOME=/tmp/o XDG_DATA_HOME=/tmp/o/data XDG_CONFIG_HOME=/tmp/o/config \
  QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software OMAMP_PROFILE=1 build/omamp
```

The benchmark's volume (`/bench`) doesn't exist, so its tracks show as
offline (dimmed). For the screenshot below, its tracks were moved onto a
volume that exists; the timings are the same either way.

![Advanced mode, 200,000 synthetic tracks](../advanced-mode-200k.png)

### Decisions

- **The tree is a flat list model, not a QML TreeView.** Lazy loading, the
  keys and reopening after a reload all live in C++ and are tested, and the
  view is a plain ListView.
- **Album Artist uses `tracks.sort_artist`** (the album artist, else the
  artist, else Various Artists): one value per track, read off the
  `tracks_order` index. It is fast and matches how albums group, but a track
  with two album artists shows under the first only. Artist and Composer use
  the multi-value `track_artists` table.
- **Album groups albums by first letter** (`#` for the rest), because a flat
  list of 18,000 albums is no use. Artist stays flat (1,600 in the benchmark).
- **Location opens on each volume's library folders**, not its root, so
  `/home` doesn't start with a user name. A library folder that is the whole
  volume opens straight onto its subfolders. Folders show no track counts:
  each count would cost a query.
- **The search box is a plain `TextInput`**, not a Material `TextField`,
  whose floating label covers the hint. It applies 60 ms after typing stops,
  or at once on `Enter`.
- **A broken query keeps the last good list** and shows the error in red
  beside the search box, so typing `artist:` doesn't blank the list.
- **A new node or search clears the selection; a sort keeps it.**
- **Play now hands over at most 5,000 tracks** (above). MediaMonkey hands
  over the whole list; at 200,000 tracks `state.json` would be 20 MB.
- **Left and Right don't seek in advanced mode**, because the tree uses them.
  The strip's position bar seeks there.
- **`O` adds a folder to what is on screen:** the library in advanced mode,
  Now Playing on the deck.
- **Columns:** `#`, Title, Artist, Album, Year, Genre, Length, Rating (stars,
  with a half star dimmer), Plays, Added, and Format with bit rate. Added
  starts hidden so the rest fit 1,360 pixels. Title takes the width the
  others leave until it is resized by hand.
- **`Tab` belongs to the mode switch**, so it never moves focus between the
  tree, the list and the search. `/`, `Enter` and `←`, and `Esc` do that.
- **Dropping files in advanced mode** adds them to Now Playing, as on the deck.
- **Developer hooks in the binary**, off unless set: `OMAMP_PROFILE=1` (the
  timings above), `OMAMP_SCREENSHOT=<png>` (grab the window and quit), and
  `OMAMP_DEMO="expand:artists;node:genre:3;select:2;queue:0;filter:…;sort:7;help"`
  (steps run before the screenshot). The README screenshots come from these.
- **`playlist.*` and `player.*` gained small additions** (insert and replace
  with known tags; play, queue next, append) rather than the library writing
  through `addPaths`, which reads every file from disk.

### Known gaps

- **Text-column sorts take about 200 ms at 200,000 tracks** (above).
- **Queries run on the window's thread.** All but the text sorts finish in
  under 30 ms here, but a slow query would pause the window.
- **No column reordering** and no column sets per collection yet (the plan
  wants both; `collections.columns` exists for them).
- **Static playlists** show in the tree, but nothing creates them until
  Phase 3. Now Playing is the only playlist so far.
- **The track list refreshes when a scan ends**, not while it runs.
- **Files to Edit**, the column browser, the album-art grid, the info panel
  and the command palette belong to other parts of Phase 1 and to Phase 2.
- **Checked offscreen only.** Not opened on screen, not tried on Matt's
  library, and not looked at in themes other than the default palette.

### Integration points for 1B and 1C

**1B (command palette: `src/app/CommandPalette.qml`, `src/core/palette.*`).**
The query language needs nothing from me: the search box sets
`library.tracks.filter`, which calls `Query::compile(text)`. To put the
palette in the window:

- Add `CommandPalette.qml` to `src/app/resources.qrc`, and the palette
  sources to `omamp.pro` and `tests/omamp_tests.pro`.
- In `Main.qml`, add `Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; … }`
  beside the advanced-mode shortcuts, and place the palette after the
  `advancedLoader` Loader and before the `?` overlay, so it draws over the
  library.
- What it can call: `win.setAdvanced(true)`, `advancedLoader.item.focusSearch()`,
  `library.tracks.filter = "<query>"`, `library.selectNode(row)` with
  `row = library.tree.rowOfKey(key)`, `library.playRow(row)`,
  `library.playSelection()`, `library.queueSelection()`,
  `library.appendSelection()`, `library.addFolder()`, `library.rescan()`,
  `library.tracks.selectAll()`. Node keys are `artist:<id>`,
  `albumartist:<lower-case name>`, `genre:<id>`, `decade:<year>`,
  `year:<year>`, `collection:<id>` and `playlist:<id>`. A node must be
  showing, so open its group first, for example
  `library.tree.expand(library.tree.rowOfKey("artists"))`.
- The palette's database reads should wait for `library.activate()`. If
  `palette.*` needs the connection, add a method on `LibraryController` that
  returns its `Library` (main thread only); I left none, to keep my files
  stable.

**1C (artwork, watcher, schema).**

- **Artwork in the list:** add a role to `TrackListModel` (read `t.artwork_id`
  or the album's in `fetch()`) and register an image provider in `main.cpp`
  (`engine.addImageProvider("artwork", …)`). The info panel and the
  album-art grid (Phase 1 part 3) would sit in `AdvancedView.qml`, in the
  `SplitView` beside the list pane.
- **Folder watching:** after the watcher writes a batch, call
  `library.tracks()->refresh()` and `library.tree()->reload()` on the main
  thread, or reuse `LibraryController::onScanFinished`. A watcher job can
  report in the same status line: `LibraryController` shows one job at a time
  (`busy`, `jobText`, `jobProgress`, `cancelJob()`); a queue would turn
  `m_job` into a list.
- **Schema request (I changed no schema):**
  `CREATE INDEX tracks_title ON tracks(title COLLATE NOCASE)`, and the same
  for `artist`, `album` and `genre`, for fast text sorts. With those, change
  the Title key in `TrackListModel::orderBy()` from
  `coalesce(nullif(t.title, ''), t.file_name) COLLATE NOCASE` to
  `t.title COLLATE NOCASE` so SQLite can use the index.
