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

## Phase 1B: the query language and the command palette (20 October 2026)

The query language is complete, the command palette has a backend and a
popup, and `omamp query` takes the whole language. Nothing is wired into
`Main.qml` (agent 1A owns it); the steps are below. The schema is unchanged.

### What was built

- **`src/core/query.{h,cpp}`**: dates and ages (`added:<30d`, `added:2024`,
  `added:2024-01..2024-06`, `played:>1y`, `played:never`, `lastplayed:<7d`,
  `today`, `yesterday`); lengths and sizes with units (`length:>10m`,
  `length:3:00..5:00`, `size:>20mb`); `none` and `any` on every field;
  `+word` word starts; `;` and `|` as OR; `sort:` with several keys,
  descending keys, `random`, `random:<seed>` and `randomalbum`; `limit:` by
  count, playing time or size; short aliases (`aa`, `ar`, `al`, `ti`...);
  "did you mean" for misspelt fields; every error with its character
  position. New API: `Query::Context` (pinned clock and seed),
  `Compiled::orderBy`, `order()` and `limit`, `Query::ids()`,
  `Parsed::toJson()`, `Options::countOnly`, and `added`, `lastPlayed` and
  `size` on `Track`.
- **`src/core/palette.{h,cpp}`**: `Palette`, a `QAbstractListModel` that
  ranks actions, artists, albums and tracks together.
- **`src/app/CommandPalette.qml`**: the popup, keyboard-only, in the theme
  colours. Added to `resources.qrc`.
- **CLI**: `omamp query` takes `--count`, `--offset`, `--seed` and
  `--explain`, prints the total length and size, and puts `duration_ms` and
  `seed` in its JSON.
- **Docs**: [docs/query-language.md](../query-language.md), the full syntax
  with examples. Screenshots: `docs/command-palette.png` and
  `docs/command-palette-actions.png`, from the offscreen test with made-up
  data.
- **Tests**: `tests/query_tests.cpp` (parse, compile and run against a
  12-track library built in SQL with a pinned clock, plus the CLI),
  `tests/palette_tests.cpp` (ranking, actions, activation, library changes,
  and the QML popup driven by keys in an offscreen window that has Omamp's
  one-key shortcuts), and `tests/fixture_library.h`. `tests/main.cpp` now
  takes `OMAMP_TESTS=library,query,palette` to run some classes only, and
  `OMAMP_SCREENSHOTS=<folder>` makes the palette test save its screenshots.

### Test results

```text
Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 442ms   (library)
Totals: 24 passed, 0 failed, 0 skipped, 0 blacklisted, 21ms    (query)
Totals: 8 passed, 0 failed, 0 skipped, 0 blacklisted, 82ms     (palette)
```

The player tests were not run in this phase: they open audio devices
(muted), and this machine had to stay silent. Nothing in the player changed.

### Benchmark

`omamp_bench run` on the 200,000-track synthetic library, generated in a
temporary folder and deleted afterwards. Median of 7 runs, warm cache, with
another heavy job on the machine. Each query time includes a page of 100
rows and the total count. The Phase 0 rows were measured again and match.

| Query | ms | Rows |
|---|---:|---:|
| `"midnight highway"` (phrase) | 5.4 | 357 |
| `+love` (word start: index, then pattern) | 27.4 | 17,211 |
| `+go` (word start too short for the index) | 93.5 | 43,858 |
| `aa:"neon coast"` | 10.2 | 10,126 |
| `added:<30d` | 0.9 | 1,404 |
| `added:2024` | 24.9 | 21,250 |
| `added:2024-01..2024-06` | 4.4 | 10,510 |
| `played:>1y` | 24.8 | 68,839 |
| `played:never` | 24.6 | 100,288 |
| `lastplayed:<7d` | 54.4 | 188 |
| `plays:0` | 26.6 | 100,288 |
| `plays:10..` | 26.5 | 95,179 |
| `length:>6m` | 24.8 | 39,421 |
| `length:3:00..5:00` | 26.9 | 80,765 |
| `album:none` | 11.8 | 0 |
| `genre:any` | 25.6 | 200,000 |
| `-genre:none` | 26.7 | 200,000 |
| `sort:-rating,artist limit:50` | 66.1 | 50 |
| `sort:random limit:50` | 79.7 | 50 |
| `sort:random` (every row shuffled, first page) | 82.2 | 200,000 |
| `sort:randomalbum limit:100` | 73.8 | 100 |
| `limit:2h` (library order) | 0.1 | 24 |
| `limit:700mb` (library order) | 0.1 | 35 |
| `rating:4.. sort:random limit:2h` | 38.5 | 28 |
| `added:<1y plays:0 sort:-added limit:2h` | 0.2 | 25 |
| `genre:rock played:>1y sort:-plays limit:50` | 5.6 | 50 |

| Palette search | ms | Rows |
|---|---:|---:|
| First search, loading artist and album names | 63.5 | 17 |
| `coast` | 6.0 | 17 |
| `neon night` | 21.2 | 13 |
| `lov` | 7.6 | 13 |
| `re` | 1.7 | 10 |
| `midnight highway` | 11.3 | 14 |
| `artist:neon sort:-plays` | 14.8 | 9 |

Every query is under the 100 ms target. `+go` is the closest at 93.5 ms, for
the same reason the plain short word `go` takes 75 ms: no index can answer
two letters. Every palette search after the first is under the 50 ms target;
the first loads 1,600 artist and 18,278 album names once.

**Indexes for the coordinator.** I tried these two indexes on a copy of the
benchmark library. They are not in a migration, because 1C owns the schema.
They cut the play-history clauses by an order of magnitude:

```sql
CREATE INDEX tracks_last_played ON tracks(last_played);
CREATE INDEX tracks_play_count ON tracks(play_count);
```

| Query | Without (ms) | With (ms) |
|---|---:|---:|
| `played:>1y` | 24.8 | 1.3 |
| `played:never` | 24.6 | 1.6 |
| `lastplayed:<7d` | 54.4 | 0.3 |
| `plays:0` | 26.6 | 1.7 |
| `plays:10..` | 26.5 | 0.8 |

An index on `rating` would not speed up `sort:-rating,artist`: `NULLS LAST`
and the tie-breaking keys need a sort anyway.

### Decisions

- **Ages and dates read the way people say them.** With an age, `<` means
  more recent (`added:<30d`, "less than 30 days ago"), and `added:30d` means
  the same. With a calendar date, `<` means before. `m` means months in a
  date and minutes in a length; `min` always means minutes.
- **A calendar date covers its whole span** in local time: `added:2024` runs
  from 1 January 2024 00:00 up to 1 January 2025 00:00. A range runs from
  the earlier moment to the later, whichever order it is written in.
- **`played:>1y` leaves out never-played tracks.** A missing date is not a
  date long ago. The docs show `played:>1y OR played:never`.
- **NOT counts unknown as false.** `-rating:4..` used to drop unrated tracks,
  because SQL's NOT of NULL is NULL. Every NOT is now `NOT coalesce(x, 0)`.
  This deliberately changes Phase 0 behaviour.
- **`none`, `never` and `any` are keywords** only when unquoted;
  `album:"none"` searches for the word. `rating:none` means unrated, and a
  rating of 0 stars is a rating. For numbers, `none` means 0 or unset.
- **A value's precision is its last unit.** `length:4m` matches 4:00 to
  4:59, `length:3:00..5:00` includes 5:00.999, and `size:20mb` matches a
  whole megabyte. Sizes count in 1,024s, as MediaMonkey and file managers do.
- **`limit:` by time or size stops at the first track that does not fit**,
  rather than skipping ahead for a shorter one, so the result is a prefix of
  the sorted list and paging stays simple. When the order is the library
  index, it reads only the rows it keeps (0.1 ms).
- **The query's `limit:` applies first, then the page** (`Options::limit`,
  `--limit`). `total` is the count after `limit:`.
- **`sort:` and `limit:` live outside the tree** (`Parsed::sort`,
  `Parsed::limit`). Inside brackets, after `-` or next to `OR` they are an
  error with a position. Several `sort:` clauses add their keys; a second
  `limit:` is an error.
- **Empty values sort last both ways round.** Ties fall back to the library
  order and then the id, so a page never changes between runs.
- **The shuffle is SQL arithmetic**: a multiply, an xor-shift (written
  `(a | b) - (a & b)`, since SQLite has no XOR) and a multiply, all modulo
  2^32, on the track id plus the seed. Any connection can run a compiled
  query without registering a C function. `randomalbum` hashes the album id
  and keeps disc and track order. The core never picks a seed:
  `Context::seed` defaults to 0, and the CLI picks a random one per run and
  prints it.
- **`+word` narrows with the index, then checks the word start** with the
  pattern `(?<![\p{L}\p{N}])word`. When the word is too short for the
  index, `LIKE` narrows first. The pattern does not ignore accents.
- **Field names start with a letter**, so `3:20` and `12:00` are words.
  `-(` now starts a NOT; a lone `-` or `+` is a word.
- **The palette keeps artist and album names in memory**, folded for case
  and accents. It reloads them when a cheap count-and-max check on
  `artists`, `albums` and `tracks` changes (checked at most every 2 seconds)
  or on `refresh()`. Tracks come from the search index. Text with no word
  of three letters lists no tracks, because a full scan would take 75 ms
  per keystroke.
- **Palette ranking.** A name equal to the text scores 100; one that starts
  with it, 92; one where every word starts a word, 80; one that contains the
  text, 68; one that contains every word, 55; long names lose a little.
  Actions also match fuzzily from a word start ("pp" finds Play/Pause, "tam"
  finds Toggle advanced mode) and through keywords. Artists get 4 extra,
  albums 2, actions 3 (and 2 more once used), and tracks a little for their
  play count. Text with fields (`artist:neon sort:-plays`) lists the tracks
  it selects, in its order. `>` lists actions only. The last row is always
  "Search the library for ...", which emits `searchRequested`.
- **The popup takes every key while open** (`Keys.onShortcutOverride`), so
  Main.qml's one-letter shortcuts (Space, Q, Z, Up, Return...) do not fire
  while typing. Ctrl+K still reaches the window, so it toggles the popup.
  The test proves this with a window that carries those shortcuts.

### Integrating the palette (for 1A)

1. In `main.cpp`, create the model next to the library and hand it to QML:

   ```cpp
   #include "palette.h"
   Palette palette;                      // after the window's Library is open
   palette.setLibrary(&library);         // or leave unset for actions only
   engine.rootContext()->setContextProperty("commandPalette", &palette);
   ```

   The `Library` must outlive the palette (or call `setLibrary(nullptr)`
   first) and belong to the GUI thread. After a scan finishes, call
   `palette.refresh()`.

2. In `Main.qml`, add the popup and its key inside the `ApplicationWindow`:

   ```qml
   CommandPalette { id: palette; model: commandPalette }
   Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; onActivated: palette.toggle() }
   ```

3. Register the actions once, with translated titles, for example in
   `Component.onCompleted`:

   ```qml
   commandPalette.addAction("play-pause", qsTr("Play/Pause"), ["pause", "resume"], "Space")
   commandPalette.addAction("next", qsTr("Next track"), ["skip"], "B")
   commandPalette.addAction("previous", qsTr("Previous track"), ["back"], "Z")
   commandPalette.addAction("add-folder", qsTr("Add folder…"), ["open", "import"], "O")
   commandPalette.addAction("rescan", qsTr("Rescan library"), ["scan", "refresh"], "")
   commandPalette.addAction("advanced", qsTr("Toggle advanced mode"), ["library"], "")
   commandPalette.addAction("shortcuts", qsTr("Show keyboard shortcuts"), ["help"], "?")
   commandPalette.addAction("quit", qsTr("Quit"), ["exit"], "Q")
   ```

   `setActionEnabled(id, false)` greys one out (for example Rescan while a
   scan runs); `removeAction(id)` drops it.

4. Connect what the user chooses:

   ```qml
   Connections {
       target: commandPalette
       function onActionTriggered(id) {
           if (id === "play-pause") backend.toggle()
           else if (id === "next") backend.next()
           // ...
       }
       function onTrackActivated(trackId) { /* play or show the track */ }
       function onAlbumActivated(albumId, title, albumArtist) { /* show the album */ }
       function onArtistActivated(artistId, name) { /* filter by artist:="name" */ }
       function onSearchRequested(text) { /* put the text in the search box */ }
   }
   ```

   The popup closes itself after a choice and also emits `chosen(item)` with
   the row's roles (`kind`, `itemId`, `title`, `subtitle`, `detail`,
   `shortcut`, `kindLabel`, `actionEnabled`).

5. Optionally, disable Main.qml's one-key shortcuts while `palette.opened`.
   The popup already takes the keys, so this is only a second guard.

The new strings to translate are in `CommandPalette.qml` (the placeholder,
the hint line, "Nothing matches") and in `palette.cpp` (`Action`, `Artist`,
`Album`, `Track`, `Search`, `%n tracks`, `Search the library for “%1”`).
Check that the app's `Translator` serves `tr()` in C++ as well as `qsTr()`.

### Known gaps

- **`+word` and words under three letters match accents exactly**, and the
  short-word `LIKE` ignores case only for A to Z.
- **Short word starts such as `+go` take 94 ms** on 200,000 tracks: under
  target, but the slowest clause.
- **`limit:` by time or size does not skip ahead** to fill the space. An
  Auto-DJ or sync list that wants the closest fit needs its own packing.
- **`played:` reads `tracks.last_played`**, not `play_history`, so "played
  more than three times this month" cannot be written yet.
- **No `present` or `now` in number ranges** (MediaMonkey's
  `year:1978..Present`). For dates, `added:2020..today` works.
- **The palette does not highlight the matched letters.**
- **The palette's fuzzy matching is greedy**, with a preference for word
  starts, so it can miss the best alignment in a long title. Only actions
  use it.
- **The first palette search costs about 60 ms** on 200,000 tracks while it
  loads the names.

## Phase 1C: watching, moved files, tag extras and artwork (20 October 2026)

Phase 1C is built, backend only. Local library folders are watched with
inotify and only the folders that change are rescanned. Network folders are
rescanned on a schedule, skipping folders whose modification time hasn't
changed. A moved or renamed file keeps its track. Scans read ratings, play
counts, ReplayGain, lyrics and artwork flags, MusicBrainz IDs, BPM and the
compilation flag. Album and track pictures are cached as small JPEGs, and
QML can show them through an image provider. All tests pass (11 player, 16
library, 16 Phase 1C). Nothing is pushed.

### What was built

**Schema 3** (`src/core/migrations.cpp`). Step 3 adds, to `tracks`:
`tag_version`, `has_embedded_art`, `has_lyrics`, `mb_release_group_id`,
`mb_album_artist_id`, `tag_rating`, `tag_play_count` and `quick_hash`, with
indexes on `size` and `quick_hash`; to `albums`: `mb_release_group_id` and
`art_checked`; to `artwork`: `added`. Phase 0 databases (schema 2) upgrade
with the usual `VACUUM INTO` backup first. Rows keep their ratings and play
counts; `tag_version` 0 marks them as read by the Phase 0 reader, so the next
full scan reads each file once more to fill the new fields, and rescans are
stat-only again after that.

**Tag extras** (`src/core/tagreader.*`). Ratings come from `FMPS_RATING`
(0.0 to 1.0), a plain `RATING` (0 to 5 stars, 0 to 100, or a POPM byte), and
ID3v2 `POPM` frames (Windows Media Player's and MediaMonkey's steps,
including MediaMonkey's half stars). Play counts come from `FMPS_PLAYCOUNT`,
`PLAYCOUNT`, the POPM counter and `PCNT`. Lyrics are present when any
`LYRICS*` property has text (USLT, Vorbis `LYRICS`/`UNSYNCEDLYRICS`, MP4
`©lyr`). Embedded artwork is present when TagLib lists a `PICTURE`. Tags are
now read through a read-only `FileStream`: TagLib's `FileRef` opened files
for writing when it could, so every scan "wrote" every file it read, which
the watcher saw as a change and a NAS saw as a write.

**Library stats win over tags.** A file's rating fills the library's only
while the library has none and the file's rating is new (different from the
stored `tag_rating`), so a rating the user cleared in Omamp stays cleared.
The play count fills only while the library's count is 0. Both file values
are kept in `tag_rating` and `tag_play_count`.

**Scanner** (`src/core/scanner.*`):

- It walks every root (or folder) first and marks vanished files missing,
  then reads and writes. A file moved between two folders of one scan is
  therefore already missing at its old place when its new place is read.
- **Moved files keep their tracks.** A new file that matches a missing track
  takes over that track's row (its path, volume and folder change), so its
  rating, play count, history and playlist entries follow it. A match is the
  same content hash and length, or the same size, length (within a second),
  title, artist and album, plus the same file name when the file has no
  tags. When several missing tracks match, the one with the same file name
  wins; if that still leaves several, the file is added as new rather than
  given the wrong history. `ScanResult` reports `moved` and the first 100
  `old -> new` paths.
- `quick_hash` is SHA-1 over the size and 64 KiB from the middle of the
  file, 16 hex digits. The middle avoids the tags at either end.
- **Locate** (`Scanner::locate`, `omamp locate <folder>`) is MediaMonkey's
  "Locate Moved/Missing Files": it searches any folder, inside the library
  or not, opens only files whose size matches a missing track, and repoints
  the tracks it finds. Nothing else is added.
- **Quick rescans** (`Scanner::Mode::ChangedFolders`, `omamp scan --quick`)
  skip listing a folder whose modification time matches the database.
  Subfolders are still visited (one `readdir` each, using `d_type`), so a
  quick rescan costs one stat per folder rather than per file. Folder times
  are stored only after a scan's files are written, so a cancelled scan
  never makes a later quick rescan skip a folder.
- **Incremental rescans** (`Scanner::rescanFolders`) take folders inside
  library roots, each recursive or not. A folder that no longer exists has
  its tracks marked missing, but only while its library root is still there.
- The walk now filters by name and `d_type` before any stat, so non-audio
  files cost nothing.
- **The tag pool is fed at most 2,000 files ahead of the writer**
  (`setInFlightLimit`), so neither the pool's queue nor the waiting results
  grow with the library.

**Watcher** (`src/core/watcher.*`). Raw inotify, one watch per folder, on
library roots whose volume is local. Measured on a 10,000-folder temporary
tree: adding the watches takes 15 ms and 0.6 MB with inotify, 240 ms and
2.4 MB with `QFileSystemWatcher`. Both would scale; inotify was chosen
because it also says which kind of change happened (close after writing,
moves, overflow) and reports the watch limit with `ENOSPC`.

- Events: a file counts once closed after writing, moved, deleted or
  touched, and only if it has an audio extension. A new or moved-in folder
  gets watches and a recursive rescan; a deleted or moved-out folder loses
  its watches and gets a recursive rescan (its tracks go missing, and are
  found again if it reappears inside the library). A queue overflow rescans
  every watched root in full.
- Changes are collected for 1.5 s after the last event, and never held back
  more than 10 s. Rescans run one at a time on a low-priority worker thread
  with its own `Library`; changes arriving meanwhile merge into the next one.
- Network roots (volume kind `network`, or a network mount now) are never
  watched. They are rescanned every 15 minutes in quick mode; every fourth
  scheduled run lists every folder, to catch files changed in place (which
  leave the folder's time alone).
- The watcher uses at most half of `fs.inotify.max_user_watches` (524,288 on
  this machine), leaving the rest for other programs. If that, or the
  kernel, runs out, the roots not fully watched join the scheduled rescans,
  `degraded` becomes true and `watchLimitReached` fires.
- `omamp watch` runs it in the foreground and prints each change and rescan
  (`--json` prints one JSON object per line, `--dry-run` only reports,
  `--interval <s>` sets the schedule). Ctrl+C or SIGTERM stops it cleanly.

**Artwork** (`src/core/artwork.*`, `src/app/artprovider.*`). A picture comes
from a file beside the album's tracks named `cover`, `folder` or `front` (in
that order, any case, as `.jpg`, `.jpeg`, `.png`, `.webp`, `.bmp` or `.gif`),
also one folder up from a `CD1`/`Disc 2` folder; failing that, from the first
track's embedded front cover. It is decoded once (JPEGs are decoded already
scaled down), flattened onto white if it has transparency, and saved on the
CPU at 96, 256 and 600 pixels as `~/.cache/omamp/art/<hash>-<size>.jpg`,
where the hash is of the original bytes. An album whose every track embeds
the same cover shares one set of files. `artwork` records each picture and
its source, `albums.artwork_id` and `tracks.artwork_id` point at it, and
`albums.art_checked` records the search, so an album without a picture isn't
searched again until the scanner changes one of its tracks. A cleared cache
is rebuilt from the recorded source. Nothing is made until asked for;
`ArtworkPrefetch` makes the rest on a lowest-priority thread, and
`omamp art all` does the same in the foreground.

### Test results

```text
Totals: 11 passed, 0 failed, 0 skipped, 0 blacklisted, 112ms    (player)
Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 472ms    (library)
Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 2566ms   (Phase 1C)
```

The Phase 1C tests (`tests/watcher_tests.cpp`) make their own files with
`ffmpeg` and TagLib: FLACs with FMPS, ReplayGain, MusicBrainz, lyrics, BPM
and compilation tags; an MP3 with a POPM rating and counter and USLT lyrics
written by TagLib; an MP3 with an embedded 500×300 PNG cover; and 800×800
and 500×300 images. They cover: the schema 2 to 3 upgrade with its backup;
rows from the old reader read once more, then stat-only; rating conventions;
every tag extra; library stats winning over tags, including a cleared rating
staying cleared; moved and renamed files (tagged, and untagged by hash)
keeping rating, play count, history and playlist entries with the pool fed
one file at a time; identical copies not guessed; `locate` outside the
library; quick rescans skipping unchanged folders; the watcher seeing
create, modify, move, a folder moved in from outside, a file in that new
folder, file and folder deletion, ignoring a JPEG, and not waking itself by
reading a file; the watch limit falling back to the schedule; a CIFS root
(fake mountinfo) never watched; artwork from an upper-case `Cover.JPG`, from
an embedded cover, none, track pictures, a cleared cache, and the image
provider; and the background prefetch plus the scanner clearing
`art_checked`.

### Benchmark

`omamp_bench files` on tetelestai with another heavy job running: 100,000
hard links of one small tagged MP3 in 8,334 folders, on btrfs, warm cache,
8 reader threads. Phase 0 was rebuilt from `afddb5b` and run alternately
with Phase 1C.

| What | Phase 0 | Phase 1C |
|---|---:|---:|
| First scan | 5,666–6,147 ms | 7,142–7,745 ms |
| Rescan, nothing changed (stat only) | 396–473 ms | 395–449 ms |
| Quick rescan by folder time | — | 195–233 ms |
| Peak memory, first scan | 235–264 MB | 104–108 MB |
| Peak memory, whole run | 235–264 MB | 135–140 MB |

Phase 1C at other sizes: 10,000 files take 791 ms with a 43 MB peak, and
30,000 files take 1,985 ms with a 77 MB peak. Memory now grows far slower
than the library; most of what remains is SQLite's 32 MB page cache and the
walk's file list.

The first scan is about 1.3 s slower. Each file now also gets a content
hash (one more open and a 64 KiB read) and more tag work, and the writer,
which is the bottleneck, now shares the CPU with the readers for the whole
scan. With a 20,000-file limit instead of 2,000 the first scan took
6,397–6,462 ms but peaked at 189–194 MB; the smaller limit was kept.

### Decisions

- **Raw inotify, not QFileSystemWatcher**, for the reasons and numbers above.
- **Watch at most half the system's inotify limit**, so Omamp can't starve
  editors, Syncthing or file managers.
- **Debounce 1.5 s, at most 10 s.** A long copy keeps triggering
  close-after-write per file; the cap stops a busy folder holding a rescan
  back for ever.
- **Picture files before embedded pictures.** Folder images are usually the
  larger scan and cost no tag parsing. This matches foobar2000's default.
- **Every standard size is made at the first request** for a picture, since
  decoding the original is most of the cost.
- **A rescan rereads Phase 0 rows once** to fill the new fields. On a
  100,000-track NAS library this is one slow scan; the alternative was
  leaving every Phase 0 row without ratings, art flags or IDs until edited.
- **A file's rating fills an empty library rating only when the file's
  rating is new.** Without that, clearing a rating in Omamp would be undone
  by the next rescan.
- **Ambiguous moves are not guessed.** Handing one track's history to
  another is worse than adding a new row; `omamp locate` and later a
  "Locate Moved/Missing Files" window can resolve the rest.
- **Core now links Qt GUI** for `QImage` (CPU only). The benchmark and CLI
  still need no display.
- **TagLib reads are read-only** (see above).

### Known gaps

- **A move from outside the scanned folders.** If a file's old place isn't
  rescanned in the same run as its new place (for example, the old folder is
  on a different root that the watcher didn't see change), the new file is
  added as a new track and the old one goes missing later. `omamp locate`
  fixes it by hand only before that rescan adds the new file; merging two
  existing rows is not built.
- **Quick rescans miss files changed in place** on a share, because the
  folder's time doesn't change. Every fourth scheduled rescan is a full one
  to catch them. They also don't reread Phase 0 rows.
- **A share that comes back** isn't rescanned at once; the next scheduled
  run does it, or `Watcher::rescanScheduledNow()`.
- **Picture files changing** (a new `cover.jpg`) don't wake the watcher, and
  the artwork cache has no eviction or orphan cleanup.
- **Cancel:** `Watcher::stop()` cancels a running rescan; nothing else does.
- **`omamp watch` was tested by hand only.** The `Watcher` class has tests.
- **The image provider is not registered** in the window yet; that is 1A's
  `main.cpp` (one line, below).

### What the window needs to know

**Watcher.**

```cpp
#include "watcher.h"
Watcher watcher;                        // default library; Watcher(dbPath, resolver) in tests
watcher.start();                        // after the library opens; reload() after adding a root
QObject::connect(&watcher, &Watcher::libraryChanged, ...);   // reload the visible page
QObject::connect(&watcher, &Watcher::rescanStarted, ...);    // (kind, folders) for the jobs strip
QObject::connect(&watcher, &Watcher::rescanFinished, ...);   // ScanResult::toJson() + "kind"
QObject::connect(&watcher, &Watcher::watchLimitReached, ...); // (watched, limit): say so in the window
engine.rootContext()->setContextProperty("watcher", &watcher);
```

QML properties: `running`, `busy`, `degraded`, `watchedFolders`,
`localRoots`, `networkRoots`, `scheduledRoots`. Invokables: `reload()`,
`rescanScheduledNow()`, `rescanFolders(folders, recursive)`. Signals also
include `foldersChanged(folders)` (a debounced batch, before its rescan) and
`busyChanged()`. `rescanStarted`'s kind is `changes`, `scheduled` or
`overflow`. Settings: `setDebounce(ms)`, `setMaxDelay(ms)`,
`setNetworkInterval(seconds)` (0 is off), `setMaxWatches(n)`,
`setAutoRescan(bool)`.

**Pictures in QML.**

```cpp
#include "artprovider.h"
engine.addImageProvider(ArtImageProvider::name, new ArtImageProvider);   // "omamp-art"
```

```qml
Image { source: "image://omamp-art/album/" + albumId + "/256"; asynchronous: true }
Image { source: "image://omamp-art/track/" + trackId + "/96" }
Image { source: "image://omamp-art/album/" + albumId; sourceSize.width: 600 }
```

Sizes snap to 96, 256 or 600. A missing picture sets `status` to
`Image.Error`; show a placeholder. Text after `?` is ignored, so
`…/256?v=2` reloads a picture that changed. Pictures are made on two
low-priority threads, each with its own library connection.

From C++: `ArtworkCache(library).albumArtwork(albumId, 256)` or
`.trackArtwork(trackId, 96)` gives a file path (empty when there is none).
`ArtworkPrefetch prefetch; prefetch.start(256);` makes the rest in the
background and emits `finished(made)`.

**Scanner additions.** `Scanner::scan(roots, Scanner::Mode::ChangedFolders)`,
`rescanAll(mode)`, `rescanFolders({{path, recursive}})`, `locate(folder)`,
`libraryRoots(library)` (path, kind, fs type, volume, online). `ScanResult`
adds `moved`, `movedPaths`, `foldersListed`, `foldersSkipped` and
`changedLibrary()`.

**New columns for the query language** (1B): `tracks.has_embedded_art`,
`has_lyrics`, `tag_rating`, `tag_play_count`, `mb_release_group_id`,
`mb_album_artist_id`; `bpm` and `compilation` are now filled. Fields such
as `art:none` or `lyrics:yes` for Files to Edit can read these.
