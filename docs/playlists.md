# Playlists, collections, statistics and Auto-DJ

Thrum keeps four kinds of list in its library:

- A **playlist** is a list you build by hand. It keeps its order, can hold the
  same song twice, and keeps an entry even when its file goes missing.
- An **auto-playlist** is a saved search in the [query language](query-language.md).
  It updates itself: add a five-star song and it appears in `rating:5`.
- A **folder** holds playlists and other folders.
- A **collection** is a saved search with its own columns, tree and sort,
  such as Music, Classical, Audiobooks and Podcasts.

Everything here works in the window (below) and from the command line, and
every command takes `--json` for scripts and agents.

## In the window

![Playlists in the tree](playlists.png)

- **The tree.** Playlists holds Now Playing, then your folders, playlists
  and auto-playlists in their own order. A playlist shows its number of
  entries; an auto-playlist has the accent-coloured mark. Right-click (or
  press `Menu` on) Playlists or a folder to make a playlist, auto-playlist
  or folder in it, import M3U, M3U8 or PLS files through the desktop's file
  picker, or save Now Playing as a playlist. Right-click a playlist to play
  it, play it next, add it to Now Playing, rename it, move it into another
  folder, export it, or delete it (asked first; `Delete` in the tree does
  the same). Every command is also in the palette (`Ctrl+K`), working on the
  chosen node.
- **Adding tracks.** Drag tracks from the track list onto a playlist in the
  tree; the playlist under the pointer lights up. Or right-click tracks and
  choose **Add to playlist**, which lists every playlist and offers a new
  one.
- **The playlist view.** A playlist shows in its own order with its length,
  how many entries repeat and how many are not in the library. `Alt+↑` and
  `Alt+↓`, or a drag, move the chosen entries; `Delete` takes them out;
  `Enter` or a double-click plays from there. An entry in the list twice is
  marked **repeat**; one whose file is offline, missing or was never in the
  library is greyed and marked. **Export…** writes an M3U8 into a folder you
  pick, named after the playlist.
- **The auto-playlist editor** (New auto-playlist, or **Edit…** on one).
  The query is checked as you type: a mistake says where it is, and
  otherwise the editor counts the tracks and lists the first forty. The
  rules below build the same query from rows of field, operator and value,
  matched all or any, plus an order and a limit by tracks, playing time
  (`2h`) or size (`700mb`). Editing either side updates the other. The text
  is what is saved; a query the rows can't show (brackets, AND mixed with
  OR, `+word`) is marked **advanced** and the rows step aside. A random
  auto-playlist keeps its order until **New shuffle**.
- **Collections.** Right-click Collections for **New collection** or to put
  the four standard ones back; right-click one to edit or delete it. The
  editor takes a name, a query, when to show it in the tree, which nodes go
  under it (Artist, Album Artist, Album, Genre, Year, Rating, Composer) and
  which columns its list shows. Hiding or showing a column while the
  collection is open is remembered for that collection; other nodes keep
  the window's own columns.
- **Plays.** The player counts what you hear, by the rule below, whenever a
  track ends, is skipped or stopped, or another starts, and when Thrum
  quits. Plays and Last played update in the list straight away.

![The auto-playlist editor](autoplaylist-editor.png)

## Playlists

```sh
thrum playlist create "Road trips" --folder
thrum playlist create Summer --parent "Road trips"
thrum playlist add "Road trips/Summer" 'genre:pop year:2000..2009'
thrum playlist add Summer --ids 12,40,12 --at 1
thrum playlist show Summer
thrum playlist move Summer 5 1        # entry 5 to the top
thrum playlist remove Summer 3 4      # entries 3 and 4
thrum playlist list
```

You can name a playlist by its id, its name, or its path through folders
(`"Road trips/Summer"`). A name that two playlists share is refused; use the
id or the path instead.

`playlist add` takes a query or `--ids`, and adds the tracks in the query's
order at the end, or before entry `--at`. Entries count from 1.

Other commands:

| Command | What it does |
|---|---|
| `playlist rename <playlist> <name>` | renames it |
| `playlist move <playlist> --parent <folder>` | moves the playlist into a folder (`/` is the top level); `--at n` sets its place among its neighbours |
| `playlist clear <playlist>` | empties it |
| `playlist delete <playlist>` | deletes it, and everything inside a folder |

### Missing and offline songs

An entry remembers the path, title, artist and length it was added with. If
the drive holding the song is unplugged, the entry shows as offline. If the
file is deleted, it shows as missing. If the track leaves the library
altogether, the entry stays, marked unmatched, and still exports with its old
path. Nothing disappears from a playlist unless you remove it.

## Mixed playlists

With Spotify switched on ([spotify.md](spotify.md)), a playlist can hold
Spotify tracks beside your own files.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="mixed-playlist-dark.png">
  <img alt="A playlist of local files and Spotify tracks" src="mixed-playlist-light.png">
</picture>

- **Adding them.** In the Spotify view or the Spotify strip under the
  library search, choose tracks and use **Add to playlist ▸ Thrum
  playlists**, or drag them onto a playlist in the tree. **New Thrum
  playlist…** makes one and adds them. One album row adds the album's
  tracks.
- **What is kept.** Each Spotify entry stores the track's link
  (`spotify://track/<id>`), title, artists, album, length and album id, so
  the playlist shows it without asking Spotify, and the deck finds its
  cover.
- **In the playlist view** a Spotify entry has Spotify's mark before its
  title. Moving, removing and repeats work as for any entry; the summary
  line counts the Spotify entries. While Spotify is off or not signed in,
  they are greyed, and hovering one says why: "Spotify is off — turn it on
  in Settings → Spotify".
- **Playing.** The playlist goes into Now Playing in its order. Your files
  play in Thrum; when a Spotify track's turn comes, Spotify plays it on its
  device and Thrum follows, then goes back to the next file. Shuffle and
  Repeat work across both. While Spotify can't play, its entries are left
  out of Now Playing, and one notice says how many and why. A playlist of
  only Spotify tracks says why and plays nothing.
- **Files.** Exporting writes a Spotify entry as its `#EXTINF` line and its
  `https://open.spotify.com/track/<id>` link, which other players and a
  browser understand. Importing reads those links, and `spotify:track:<id>`
  URIs, back as Spotify entries; they are never matched to local files.
- **The command line.**

  ```sh
  thrum playlist add Summer spotify:track:<id> https://open.spotify.com/track/<id>
  thrum playlist show Summer
  ```

  `playlist add` takes Spotify links instead of a query. When Spotify is on
  in Thrum and spotify_player is running, it asks spotify_player for each
  track's name, artists, album and length; otherwise it adds the tracks by
  id alone and says so. `playlist show` lists a Spotify entry with its
  link and `[Spotify]`; with `--json` each entry has `"spotify": true` and
  its `url`.

Auto-playlists, Auto-DJ and device syncing stay local: they pick from the
library, so a playlist's Spotify entries are left out of them.

## Auto-playlists

```sh
thrum autoplaylist create "Loved and forgotten" 'rating:4.. played:>1y sort:random limit:2h'
thrum autoplaylist set "Loved and forgotten" 'rating:4.5.. played:>1y sort:random limit:2h'
thrum playlist show "Loved and forgotten"
thrum playlist reroll "Loved and forgotten"
```

The query may use `sort:` and `limit:`. An auto-playlist is read afresh every
time you open or play it, so it always matches the library.

A random auto-playlist keeps the same pick until you ask for a new one with
`playlist reroll`. Thrum stores the shuffle's seed with the playlist, so the
list doesn't change under you while you play it. `--seed n` picks a
particular shuffle.

You can't add, remove or reorder songs in an auto-playlist; change its query
instead.

## Playlist files

```sh
thrum playlist import ~/Playlists/Party.m3u8
thrum playlist import ~/Playlists               # a whole folder
thrum playlist export Summer ~/Summer.m3u8 --relative
thrum playlist export Summer ~/Summer.pls
```

Import reads M3U, M3U8 and PLS. For each entry, Thrum looks for the song:

1. by its path, first exactly and then ignoring case;
2. by the artist and title in its `#EXTINF` line (or PLS title), within three
   seconds of the length it gives.

Relative paths are read from the playlist's own folder, `file://` URLs become
paths, and backslashes in relative paths become slashes. A plain `.m3u` may
be UTF-8 or Latin-1; Thrum uses UTF-8 when the bytes are valid UTF-8. Entries
that match nothing, including streams, are kept as unmatched entries, and the
import reports them.

Importing a folder makes a playlist folder named after it, with one playlist
per file and a sub-folder for every sub-folder that holds playlists.

Export writes UTF-8 with an `#EXTINF` line (length, artist and title) before
each path, or a PLS file when the name ends in `.pls`. `--relative` writes
paths relative to the playlist file, which suits a playlist kept beside the
music on a USB stick or phone. An auto-playlist exports the songs it holds
now. The file is written beside the target and renamed into place.

## Collections

```sh
thrum collection list
thrum collection create Jazz 'genre:jazz'
thrum collection show Jazz
thrum collection remove Jazz
thrum collection restore-defaults
```

Each collection also stores the columns shown, the tree nodes, the sort and
the view (details or album grid). The window edits those; the command line
creates collections with the defaults. `restore-defaults` puts back Music,
Classical, Audiobooks or Podcasts if one was deleted.

## Play statistics

A play **counts** when you hear at least half the song, or at least four
minutes of it, the rule Last.fm uses. A play that ends before a tenth of the
song, or that you skip with Next, counts as a **skip**. A play stopped
somewhere between counts as neither.

A counted play adds one to the song's play count, sets when it was last
played and adds a row to the play history. A skip adds one to the skip count
and a history row marked as a skip.

```sh
thrum stats --top artists
thrum stats --top albums --since 30d
thrum stats --top tracks --since 2026-01-01 --limit 50
thrum stats --top years
thrum stats --top genres --json
thrum stats --recent
thrum stats --never
```

Without `--since`, the totals come from each song's play count, which
includes counts read from tags or imported from MediaMonkey. With `--since`,
they come from the play history, so only plays Thrum recorded (or imported
with a date) count. `--since` takes an age (`12h`, `30d`, `2w`, `6m`, `1y`)
or a date (`2026`, `2026-06`, `2026-06-30`).

## Auto-DJ

Auto-DJ keeps the queue topped up with songs from a source. It avoids playing
the same artist twice in a row, and songs heard in the last week.

```sh
thrum autodj --count 20
thrum autodj --source collection:Music --count 20 --seed 7
thrum autodj --source 'playlist:Road trips/Summer'
thrum autodj --source 'query:rating:4.. genre:jazz' --separation 3 --days 14
```

The source is `library`, `collection:<name>`, `playlist:<name>` or
`query:<text>`. The command prints the songs Auto-DJ would queue; the player
uses the same code to fill the real queue.

| Setting | Default | What it does |
|---|---|---|
| upcoming | 10 | songs kept queued after the current one |
| artist separation | 5 | no artist twice within this many songs |
| skip played within | 7 days | leaves out songs played this recently |
| weight by rating | on | a five-star song comes up three times as often as an unrated one, a no-star song a quarter as often |
| never-played boost | 2 | songs never played come up twice as often |
| online only | on | leaves out songs on unplugged drives and missing files |
| excluded types | audiobook, podcast | never picked |

When the rules leave nothing to pick, Auto-DJ relaxes them one at a time:
first the artist rule, then the recently-played rule, then repeats. A source
of one song still plays.

A seed makes the picks repeatable: the same seed and the same library give
the same songs.

In the window, `Ctrl+J` (or the **AUTO-DJ** lamp in the deck strip) turns
Auto-DJ on and off, and **Auto-DJ settings…** in the palette chooses the
source and the rules. While it is on, every track change tops Now Playing up
to the chosen number of queued tracks. Turning it on with an empty Now
Playing fills it without starting to play; `Space` starts it. Auto-DJ stays
on across restarts.

## Party mode

Party mode locks the player so guests can't spoil the queue or the library.
It has three presets:

| Preset | Guests can |
|---|---|
| jukebox | browse the library, add songs to the queue, press play |
| dj | also skip, seek, change the volume, and remove or reorder queued songs |
| display | nothing; the window shows what is playing |

An optional PIN guards leaving party mode. Thrum stores only a salted hash of
it. The settings live in the library under `party_mode`.

In the window, **Party mode…** in the palette chooses the preset, an
optional PIN and an optional big now-playing display that covers the
window. While it is on, the **PARTY** lamp in the deck strip is lit;
editing tags, changing playlists, organising, settings, the library folders
and switching Auto-DJ say "Locked in party mode", and `Q` or closing the
window asks for the PIN. Click the lamp (or the note on the big display) to
leave. Party mode holds across a restart.

## For developers

| File | What it holds |
|---|---|
| `src/core/playlists.h` | `Playlists::` (static, auto, folders, entries) and `Collections::` |
| `src/core/m3u.h` | `PlaylistFiles::` reading, matching (`Matcher`), import and export |
| `src/core/stats.h` | `Stats::recordPlay()`, the play rule, top lists, recent, never played |
| `src/core/autodj.h` | `AutoDj::Dj` and its settings; `PartyMode::` |
| `src/app/playlistscontroller.h` | the window's playlists, collections, import and export, query check |
| `src/app/playlistitemsmodel.h` | the playlist view's entries, selection, moves and removal |
| `src/app/rulebuilder.h` | the auto-playlist rules to query text and back |
| `src/app/djcontroller.h` | Auto-DJ refills and party mode for the window |
| `src/app/playrecorder.h` | play counts and the scrobble queue from the player |

Schema step 11 added `playlist_items.album` and `art` for Spotify entries
(a Spotify entry is an entry with no track whose path is
`spotify://track/<id>`). Schema step 7 added `playlists.position`, `seed`, `note` and `source`;
rebuilt `playlist_items` so `track_id` may be NULL and each entry keeps its
path, title, artist and length; added `play_history.source`; and added
`import_journal`. The tests are in `tests/playlist_tests.cpp`. How the
window and the player call these is in
[plan/progress.md](plan/progress.md#phase-3-core-playlists-statistics-auto-dj-and-mediamonkey-20-october-2026).
