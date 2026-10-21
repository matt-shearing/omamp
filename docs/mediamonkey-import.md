# Importing a MediaMonkey library

`thrum import mediamonkey` brings across what MediaMonkey knows and your music
files don't: ratings, play counts, skip counts, when each song was last
played and added, the custom fields, the play history, playlists and
AutoPlaylists. It never changes MediaMonkey's database or your music files,
and you can undo the whole import with one command.

## Before you start

1. Scan your music into Thrum first (`thrum scan /mnt/nas/music`). The import
   only fills in songs Thrum already has.
2. Copy MediaMonkey's database to this computer. MediaMonkey 4 keeps it as
   `MM.DB` and MediaMonkey 5 as `MM5.DB`, usually in
   `C:\Users\<you>\AppData\Roaming\MediaMonkey\` (MediaMonkey 5:
   `...\MediaMonkey5\`). Close MediaMonkey before copying, or copy its
   `-wal` or `-journal` file too.
3. Note where your music lived on Windows (`D:\Music`, `\\nas\music`) and
   where the same folders are now (`/mnt/nas/music`).

## Look first

```sh
thrum import mediamonkey ~/MM.DB --map 'D:\Music=/mnt/nas/music' --dry-run
```

A dry run is the default; it changes nothing. It reports:

- how many MediaMonkey songs it found, and how each was matched to a Thrum
  track: by path, by folder and file name, or by artist, title and length;
- the songs it couldn't match, with a sample of their paths;
- each change it would make, counted by field;
- how many play-history rows it would add, and how many are already there;
- each playlist, and whether an AutoPlaylist could be translated;
- suggested `--map` rules, guessed from songs that matched by file name.

Add `--json` for the full report, with a sample of the changes song by song.

### Path maps

MediaMonkey stored Windows paths. Each `--map` swaps a Windows prefix for a
Linux folder. Repeat it for each drive or share:

```sh
thrum import mediamonkey ~/MM.DB \
    --map 'D:\Music=/mnt/nas/music' \
    --map '\\nas\audiobooks=/mnt/nas/audiobooks' \
    --dry-run
```

Case and slash direction don't matter, and the longest matching prefix wins,
so `D:\Music\Classical=/mnt/classical` can sit beside `D:\Music=...`. A song
that no map fits is still matched by its last two or three folders and file
name, when only one Thrum track has them, and failing that by artist, title
and a length within three seconds. Good maps make the import faster and
surer, so add the ones the dry run suggests.

## Import

```sh
thrum import mediamonkey ~/MM.DB --map 'D:\Music=/mnt/nas/music' --apply
```

Everything is written in one transaction and recorded as one batch, whose id
the command prints (`mmimport-20261020-231500-a1b2`).

### What changes

| What | Rule |
|---|---|
| Rating | Filled in where Thrum has none. MediaMonkey's "unknown" (-1) is skipped. |
| Play count, skip count | The larger of the two. |
| Last played | The later of the two. |
| Date added | The earlier of the two. |
| Custom 1 to 5 | Filled in where Thrum's is empty. |
| Play history | One row per MediaMonkey play, unless Thrum already has a play of that song at that second. |
| Playlists | Under a new folder called MediaMonkey, with MediaMonkey's folders kept. |

`--overwrite` takes MediaMonkey's value for every field it has, instead of
merging.

Because the rules merge, importing the same database twice changes nothing
the second time. Playlists already imported from that database are not
added again.

### Playlists and AutoPlaylists

A static playlist keeps its order and its duplicates. An entry whose song
didn't match keeps its mapped path, title and artist, so nothing is lost and
the playlist still exports.

An AutoPlaylist becomes a Thrum auto-playlist when its criteria translate to
the [query language](query-language.md). Ratings become stars (80 is four
stars), lengths become seconds, `Date() - 30` becomes "in the last 30 days",
and `ORDER BY` and `LIMIT` become `sort:` and `limit:`. When something
doesn't translate, such as a field Thrum lacks, the AutoPlaylist is imported
as a plain playlist of the songs MediaMonkey listed for it (often none), and
its note records MediaMonkey's criteria and the reason. The report lists
each one, so you can rebuild it by hand with `thrum autoplaylist set`.

## Undo

```sh
thrum import undo                   # the newest import
thrum import undo mmimport-20261020-231500-a1b2
thrum undo mmimport-20261020-231500-a1b2   # the same
```

Undo puts back every value the import changed, deletes the playlists it made
and removes the history rows it added. Plays you made since the import are
kept: a play count goes back down only by what the import added. A rating or
date you changed yourself since the import stays as you set it.

## What Thrum assumes about MediaMonkey's database

Thrum has not yet been tested on a real MediaMonkey database. The importer
follows MediaMonkey 4's published schema ("Database Structure (MM4)" on the
MediaMonkey wiki) and reads only the columns that exist, so a missing column
is reported, not fatal. The tests build a small database in that shape.

The importer copies the database to a temporary folder and reads the copy
with the SQLite C library. MediaMonkey declares its text columns with two
custom collations, `IUNICODE` (case- and accent-insensitive) and
`NUMERICSTRING` (numbers in text sorted as numbers). SQLite refuses to sort
or search those columns without them, so Thrum registers stand-ins on the
copy; any other unknown collation gets a case-insensitive stand-in.

| Table and column | Used for | Confidence |
|---|---|---|
| `Songs.ID`, `Songs.SongPath` | identity and path | documented |
| `Songs.SongTitle`, `Artist`, `Album`, `AlbumArtist`, `SongLength` (ms) | matching by tags | documented |
| `Songs.Rating` (0 to 100, -1 unknown) | rating | documented |
| `Songs.PlayCounter`, `Songs.LastTimePlayed`, `Songs.DateAdded` | counts and dates | documented |
| `Songs.SkipCount` | skips | unverified |
| `Songs.Custom1` to `Custom5` | custom fields | unverified for MediaMonkey 4 |
| `Songs.IDMedia`, `Medias.IDMedia`, `Medias.DriveLetter` | the drive letter of a local path | unverified |
| `Played.IDSong`, `Played.PlayDate` | play history | unverified |
| `Playlists.IDPlaylist`, `PlaylistName`, `ParentPlaylist`, `isAutoPlaylist`, `QueryData` | playlists | names documented; `QueryData`'s format unverified |
| `PlaylistSongs.IDPlaylist`, `IDSong`, `SongOrder`, `IDPlaylistSong` | playlist entries | documented |

More assumptions, each unverified:

- **Dates** are OLE Automation dates (days since 30 December 1899, a double),
  read as UTC. If MediaMonkey wrote local time, imported times are off by
  your time zone's offset, which matters only for history by the hour.
  Dates before 1980 or after 2200 are treated as empty.
- **Local paths** in MediaMonkey 4 start with `:\` and get their drive letter
  from `Medias.DriveLetter`, read as 0 for A, or as a character code (65 for
  A). Network paths are stored whole (`\\server\share\...`).
- **MediaMonkey 5** is recognised by a `Collections` table, a `Custom6`
  column or the file name `MM5.DB`, and read the same way.
- **AutoPlaylist criteria** (`QueryData`): the translator reads a SQL-style
  condition (`Rating >= 80 AND Genre LIKE '%Jazz%' ORDER BY Rating DESC
  LIMIT 50`, with `Date() - n`), or INI-style lines (`Condition0=`,
  `MatchAny=1`, `MaxTracks=`, `Order=`). MediaMonkey's real format may be
  neither; such AutoPlaylists come in as plain playlists with their criteria
  in the note.
- **Year** may be stored as `YYYYMMDD`; the translator divides numbers above
  9999 by 10,000.
- **Bitrate** in criteria may be bits per second; numbers of 10,000 and over
  are divided by 1,000.

When you run the import on your real database, check the dry run's
`missing_columns` and `notes` first. If a column has another name there,
`src/core/mmimport.cpp` reads each one in one place.

## Options

| Option | What it does |
|---|---|
| `--map 'D:\Music=/mnt/music'` | a Windows prefix and the Linux folder it is now; repeat for each |
| `--dry-run` | report only (the default) |
| `--apply` | write the changes |
| `--overwrite` | take MediaMonkey's value wherever it has one, instead of merging |
| `--json` | the full report as JSON |
| `--db <path>` | another Thrum library |
