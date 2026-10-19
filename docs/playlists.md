# Playlists, collections, statistics and Auto-DJ

Omamp keeps four kinds of list in its library:

- A **playlist** is a list you build by hand. It keeps its order, can hold the
  same song twice, and keeps an entry even when its file goes missing.
- An **auto-playlist** is a saved search in the [query language](query-language.md).
  It updates itself: add a five-star song and it appears in `rating:5`.
- A **folder** holds playlists and other folders.
- A **collection** is a saved search with its own columns, tree and sort,
  such as Music, Classical, Audiobooks and Podcasts.

Everything here works from the command line, and every command takes `--json`
for scripts and agents.

## Playlists

```sh
omamp playlist create "Road trips" --folder
omamp playlist create Summer --parent "Road trips"
omamp playlist add "Road trips/Summer" 'genre:pop year:2000..2009'
omamp playlist add Summer --ids 12,40,12 --at 1
omamp playlist show Summer
omamp playlist move Summer 5 1        # entry 5 to the top
omamp playlist remove Summer 3 4      # entries 3 and 4
omamp playlist list
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

## Auto-playlists

```sh
omamp autoplaylist create "Loved and forgotten" 'rating:4.. played:>1y sort:random limit:2h'
omamp autoplaylist set "Loved and forgotten" 'rating:4.5.. played:>1y sort:random limit:2h'
omamp playlist show "Loved and forgotten"
omamp playlist reroll "Loved and forgotten"
```

The query may use `sort:` and `limit:`. An auto-playlist is read afresh every
time you open or play it, so it always matches the library.

A random auto-playlist keeps the same pick until you ask for a new one with
`playlist reroll`. Omamp stores the shuffle's seed with the playlist, so the
list doesn't change under you while you play it. `--seed n` picks a
particular shuffle.

You can't add, remove or reorder songs in an auto-playlist; change its query
instead.

## Playlist files

```sh
omamp playlist import ~/Playlists/Party.m3u8
omamp playlist import ~/Playlists               # a whole folder
omamp playlist export Summer ~/Summer.m3u8 --relative
omamp playlist export Summer ~/Summer.pls
```

Import reads M3U, M3U8 and PLS. For each entry, Omamp looks for the song:

1. by its path, first exactly and then ignoring case;
2. by the artist and title in its `#EXTINF` line (or PLS title), within three
   seconds of the length it gives.

Relative paths are read from the playlist's own folder, `file://` URLs become
paths, and backslashes in relative paths become slashes. A plain `.m3u` may
be UTF-8 or Latin-1; Omamp uses UTF-8 when the bytes are valid UTF-8. Entries
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
omamp collection list
omamp collection create Jazz 'genre:jazz'
omamp collection show Jazz
omamp collection remove Jazz
omamp collection restore-defaults
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
omamp stats --top artists
omamp stats --top albums --since 30d
omamp stats --top tracks --since 2026-01-01 --limit 50
omamp stats --top years
omamp stats --top genres --json
omamp stats --recent
omamp stats --never
```

Without `--since`, the totals come from each song's play count, which
includes counts read from tags or imported from MediaMonkey. With `--since`,
they come from the play history, so only plays Omamp recorded (or imported
with a date) count. `--since` takes an age (`12h`, `30d`, `2w`, `6m`, `1y`)
or a date (`2026`, `2026-06`, `2026-06-30`).

## Auto-DJ

Auto-DJ keeps the queue topped up with songs from a source. It avoids playing
the same artist twice in a row, and songs heard in the last week.

```sh
omamp autodj --count 20
omamp autodj --source collection:Music --count 20 --seed 7
omamp autodj --source 'playlist:Road trips/Summer'
omamp autodj --source 'query:rating:4.. genre:jazz' --separation 3 --days 14
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

## Party mode

Party mode locks the player so guests can't spoil the queue or the library.
It has three presets:

| Preset | Guests can |
|---|---|
| jukebox | browse the library, add songs to the queue, press play |
| dj | also skip, seek, change the volume, and remove or reorder queued songs |
| display | nothing; the window shows what is playing |

An optional PIN guards leaving party mode. Omamp stores only a salted hash of
it. The window will offer party mode; the settings live in the library under
`party_mode`.

## For developers

| File | What it holds |
|---|---|
| `src/core/playlists.h` | `Playlists::` (static, auto, folders, entries) and `Collections::` |
| `src/core/m3u.h` | `PlaylistFiles::` reading, matching (`Matcher`), import and export |
| `src/core/stats.h` | `Stats::recordPlay()`, the play rule, top lists, recent, never played |
| `src/core/autodj.h` | `AutoDj::Dj` and its settings; `PartyMode::` |

Schema step 7 added `playlists.position`, `seed`, `note` and `source`;
rebuilt `playlist_items` so `track_id` may be NULL and each entry keeps its
path, title, artist and length; added `play_history.source`; and added
`import_journal`. The tests are in `tests/playlist_tests.cpp`. How the
window and the player call these is in
[plan/progress.md](plan/progress.md#phase-3-core-playlists-statistics-auto-dj-and-mediamonkey-20-october-2026).
