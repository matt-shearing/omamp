# Plan: Thrum advanced mode

Thrum's advanced mode turns the Winamp-style player into a MediaMonkey-style
library manager. It handles big local libraries, from a few hundred albums to
more than 100,000 tracks, on local disks and on a NAS. It keeps the 2000s
features that made MediaMonkey worth using and adds what modern tools do
better.

This plan comes from two research reports in this repo:
[MediaMonkey's feature set](../research/mediamonkey.md) and
[the modern landscape: tools, services, libraries and NAS](../research/modern-landscape.md).

Written 19 October 2026. Nothing in it is built yet except the player.

## The pitch

**Arch, Omarchy and Hyprland first.** Thrum follows the Omarchy theme,
Hyprland's rounding and Omarchy's keyboard habits. It installs as an Arch
package and has an Omarchy bar plugin. Other desktops work, but they are not
the target.

**Powerful, modern library management.** Thrum tags from MusicBrainz and
AcoustID. One query language drives search, smart playlists and collections.
Files are organised by a pattern, with a preview and undo. Background jobs
never block the window, and a command-line interface lets scripts and agents
drive it.

**The 2000s, back.** A library tree with Files to Edit nodes,
auto-playlists, Auto-DJ, party mode, ratings and play counts, volume
levelling, and a deck that looks like Winamp. Big local libraries, managed
properly.

## What we keep from MediaMonkey, and what we fix

| MediaMonkey | In Thrum |
|---|---|
| SQLite library with ratings, play counts, custom fields | Same, plus an undo journal for every tag change |
| Drive IDs for removable and network drives, which break on a new PC | Volume IDs from filesystem UUIDs or a marker file at the share root; paths stored relative to the volume |
| "Disconnected" vs "Dead Links" | Kept: offline tracks stay in the library and never get purged while their volume is missing |
| Folder monitoring that misses changes made on a NAS by other machines | Local disks are watched with inotify. Network volumes get fast periodic rescans by size and modification time, or are read through Navidrome on the NAS |
| Collections (saved criteria with their own columns and tree) | Kept |
| Quick search and Advanced Search | One query language for search, auto-playlists, collections, Auto-DJ sources and sync lists |
| Auto-Organize masks (`<Album Artist>\<Album>\<Track#:2> <Title>`, `$If`, `$MovePrefix`) | Same mask language, a preview with per-row overrides, and undo (MediaMonkey had none) |
| Auto-Tag from Web (Amazon, later a MusicBrainz lookup users disliked) | Picard's workflow: cluster, look up by AcoustID fingerprint and MusicBrainz, review the matches by confidence, then write |
| Files to Edit nodes (unknown artist, missing art, duplicates, unlevelled) | Kept, as built-in saved queries |
| Duplicate finder, manual delete only | Kept: by title and by audio fingerprint, and never deleted automatically |
| Volume levelling | ReplayGain 2 (EBU R128) analysis; reads existing tags first |
| AutoPlaylists, Auto-DJ, Party Mode, sleep timer, equaliser | Kept |
| Device sync with auto-conversion | Sync profiles to folders, USB drives and Syncthing folders, transcoding with `ffmpeg` |
| UPnP/DLNA server | Replaced by Navidrome (OpenSubsonic) on the NAS, which also serves phones |
| VBScript and JS addons that broke between versions | A stable command-line interface with JSON output first; scripting later, with a versioned API |
| Slow drill-down in MM5 at 150,000 tracks | Performance targets with a 200,000-track test library (below) |

New, from modern tools: synced lyrics (LRCLIB), ListenBrainz and Last.fm
scrobbling, a waveform seek bar, gapless playback and crossfade, a
command palette (Ctrl+K), a background job panel, and importing an existing
MediaMonkey database (ratings, play counts, playlists).

## Architecture

### Code layout

The code splits in three, so the window, the command line and the tests share
one core:

```text
src/core/     library database, scanner, volumes, query and mask languages,
              tagging, organise, jobs, metadata providers, sync
src/engine/   audio playback (QMediaPlayer now, our own engine later)
src/app/      Qt Quick window: deck, advanced mode, dialogs
src/cli/      `thrum scan`, `thrum query`, `thrum tag`, `thrum organise`
omarchy-plugin/   the Omarchy bar widget and panel
```

The build stays qmake. `src/core/core.pri` is included by the app, the CLI and
the tests.

### Database

- SQLite in `~/.local/share/thrum/library.db` with WAL mode, through Qt SQL.
  It always lives on a local disk. SQLite on a network mount risks locking
  failures and corruption.
- Main tables: `volumes`, `folders`, `tracks`, `artists` with
  `track_artists` (multi-value artist, album artist, composer and genre, as in
  MediaMonkey), `albums`, `playlists`, `playlist_items`, `collections`,
  `play_history`, `tag_journal`, `artwork`, `jobs`, `settings`.
- `tracks` holds the volume ID, the path relative to the volume, size,
  modification time, every common tag, rating, play count, last played,
  date added, ReplayGain values, content type (music, audiobook, podcast,
  classical), five custom fields, and availability (online, offline, missing).
- An FTS5 table with the trigram tokeniser serves search and the command
  palette.
- Versioned migrations from day one, and a backup before each migration.

### Volumes

Each library root belongs to a volume. A local disk is identified by its
filesystem UUID. A network share is identified by a marker file,
`.thrum-volume-id`, written once at its root, with the server and share name
as a fallback. When a volume disappears, its tracks show as offline. They
stay in playlists and keep their ratings and history. Remounting the share
somewhere else needs no rescan.

### Scanning

1. Walk the folders and compare each file's size and modification time with
   the database. Unchanged files are skipped without opening them.
2. Read new and changed files with TagLib 2 on a pool of 4 to 8 workers.
3. Write in batches inside one transaction.
4. Watch local folders with inotify. Rescan network volumes on a schedule and
   when they reappear, walking only folders whose modification time changed.

The window shows the existing library at once and refreshes in the
background. A scan never blocks the window.

### Query language

One language, used everywhere: the search box, auto-playlists, collections,
Auto-DJ sources, sync lists and the CLI. It starts from MediaMonkey's quick
search and adds fields, ranges and regular expressions:

```text
neon coast                         words anywhere (AND)
artist:"neon coast" year:1997..2001
rating:4.. -genre:christmas        ranges and NOT
genre:rock OR genre:metal
title:/^live/                      regular expression
added:<30d played:0                relative dates and counts
```

It compiles to parameterised SQL. Saved queries become auto-playlists,
collections and Files to Edit nodes.

### Mask language

MediaMonkey's mask syntax, for organising files, tagging from file names and
custom columns: `<Album Artist>/<Album> (<Year>)/<Disc#>-<Track#:2> <Title>`,
with `$If`, `$Replace`, `$MovePrefix`, `$Left`, `<Artist@1>` grouping and the
rest. Every rename and move shows a preview first, applies as one job with a
rollback log, and can be undone.

### Tag writing

- TagLib 2, one file object per worker.
- Write to a temporary copy, then rename it over the original, so a dropped
  NAS connection never leaves a half-written file.
- Every change goes into `tag_journal` with the old and new values, so any
  edit, batch or web lookup can be undone.
- A setting decides whether ratings and play counts go into the files or stay
  in the database. The default is the database, so playing music doesn't
  rewrite files on the NAS.

### Playback engine

Qt 6's `QMediaPlayer` has no gapless playback or crossfade. Phase 3 replaces
it with our own engine: FFmpeg decoding (two decoders, so the next track is
ready before the current one ends), ReplayGain, a 10-band equaliser and
crossfade, into one `QAudioSink`. It also reads encoder delay and padding, so
gapless albums really are gapless.

Omarchy's own apps call the `ffmpeg` command instead of linking FFmpeg's
libraries. Playback can't work that way, so this is a deliberate exception.
Strawberry's GStreamer engine is the fallback design if ours gets stuck.

### Background jobs

Scans, lookups, ReplayGain analysis, organising, syncing and transcoding are
all jobs. They run in a queue with progress, pause and cancel. They show in
one panel and as Omarchy notifications, never as modal dialogs.

### Command line

`thrum scan`, `thrum query 'rating:5 genre:jazz'`,
`thrum tag --set genre=Jazz <query>`, `thrum organise --dry-run`, and
`thrum import mediamonkey <MM5.DB>`. Each command takes `--json` and exits 0
or 1. `thrum skill install` installs an agent skill, as Hype does.

## The window in advanced mode

`Tab` switches between the deck and advanced mode. Advanced mode keeps the
deck as a strip across the top and opens the library below it:

```text
┌─ deck strip: clock · analyser · title · transport · volume ────────────────┐
├─ tree ───────────┬─ column browser: Genre │ Artist │ Album ──────┬─ info ──┤
│ Collections      ├───────────────────────────────────────────────┤ artwork │
│  Music           │ #  Title          Artist     Album    Time  ★ │ lyrics  │
│  Audiobooks      │ 1  …                                          │ details │
│ Artist / Album … │                                               │         │
│ Playlists        │                                               │         │
│ Auto-playlists   │                                               │         │
│ Files to Edit    │                                               │         │
│ Volumes / NAS    │                                               │         │
│ Devices          │                                               │         │
├──────────────────┴─────────────── jobs: scanning NAS 41 % ────────┴─────────┤
└────────────────────────────────────────────────────────────────────────────┘
```

- The tree, the column browser and the track list behave like MediaMonkey's.
  Columns can be chosen and reordered per collection.
- `Ctrl+K` opens a command palette over tracks, albums, artists and actions.
- `/` focuses search. Multi-select, then `Ctrl+E` opens the tag editor.
- Album-art grid as a second view of any list.
- Everything stays in the Omarchy theme and works from the keyboard.

The look stays native and plain. Winamp flavour belongs in the deck.

## Phases

Each phase ends with something Matt can use, tests that pass, screenshots,
and a push. Within a phase, parts that touch different files run as parallel
subagents: Opus for building and review, Sonnet for searching and QA sweeps.
Each subagent works in its own worktree and is reviewed before merging.

### Phase 0: the core (one agent, in order)

Split the code into core, engine, app and CLI. Add the database, migrations,
volumes, scanner and `thrum scan` and `thrum query`. Build a test fixture
generator that makes tagged audio files with `ffmpeg`, and a 200,000-row
synthetic library for performance tests.

*Done when:* `thrum scan ~/Music` fills the library, a rescan of an unchanged
library only checks file sizes and times, and the player still works.

### Phase 1: the library window (three agents in parallel)

1. The advanced-mode layout, the tree and the track list.
2. The query language, search and the command palette.
3. The column browser, the album-art grid and the info panel.

*Done when:* the 200,000-track library opens in under a second, and search
and filtering answer in under 100 ms.

### Phase 2: tagging and tidying (three agents)

1. The tag editor: several tracks at once, artwork, lyrics, undo.
2. The mask language: organise files and tag from file names, with a preview.
3. Files to Edit nodes, the duplicate finder, and missing-artwork reports.

### Phase 3: playlists and playback (three agents)

1. Static playlists and auto-playlists (with a rule builder over the query
   language), M3U import and export, collections.
2. The new playback engine: gapless, crossfade, ReplayGain, equaliser.
3. Auto-DJ, party mode, sleep timer, play statistics, ratings, and importing a
   MediaMonkey database.

### Phase 4: the web (two agents)

1. Identify: Chromaprint fingerprints, AcoustID, MusicBrainz and Cover Art
   Archive, in Picard's cluster, look up, review and save workflow.
2. LRCLIB synced lyrics with a scrolling lyrics view, plus ListenBrainz and
   Last.fm scrobbling with an offline queue.

### Phase 5: the NAS (two agents)

1. Network volumes over SMB and NFS: offline handling, scheduled rescans,
   local artwork and waveform caches, and a guide to mounting shares with
   systemd automount.
2. An OpenSubsonic client, so Thrum can browse and play a Navidrome library
   on the NAS without scanning across the network.

### Phase 6: sync and release

Sync profiles (folder, USB, Syncthing) with transcoding; reports; the AUR
package; the full README (see [README-outline.md](../README-outline.md));
and proposing Thrum to `omacom/omarchy-pkgs`.

### Later, if wanted

CD ripping (through `whipper` or `cdparanoia`), podcasts and internet radio,
sound-alike radio from local audio analysis, Snapcast multi-room, a JS
scripting API, and translations.

## Performance targets

| What | Target |
|---|---|
| Open a 200,000-track library | under 1 second to a usable window |
| Search or filter | under 100 ms |
| Rescan an unchanged local library of 100,000 tracks | under 30 seconds |
| Rescan an unchanged NAS library of 100,000 tracks | stat only, no file reads; minutes, not hours |
| Memory with 200,000 tracks | under 500 MB |

## Risks

- **The playback engine is the biggest single job.** Phase 3 keeps
  `QMediaPlayer` behind an interface, so the swap can wait or fall back to
  GStreamer.
- **Huge lists in Qt Quick.** `TableView` with a SQL-backed model that loads
  rows on demand; tested against 200,000 rows from Phase 1.
- **Writing tags over the network.** Write and rename, journal everything,
  and retry with verification.
- **Service terms.** AcoustID is free for non-commercial use only. Discogs and
  Last.fm restrict caching. MusicBrainz and ListenBrainz allow one request a
  second and need a real User-Agent.
- **Wayland.** No global key grabs and no self-positioning windows. MPRIS,
  Hyprland bindings and window rules cover both.

## Questions for Matt

1. Which NAS do you have, and does it share over SMB, NFS or both? Could it run
   Navidrome in Docker?
2. Roughly how big is the library you want to manage: albums, tracks,
   gigabytes?
3. Do you still have a MediaMonkey database (`MM.DB` or `MM5.DB`)? Importing
   ratings and play counts would move up the list.
4. Should ratings and play counts be written into the music files, or kept in
   Thrum's database only? The plan assumes the database.
5. Do you use ListenBrainz or Last.fm, and do you have a Discogs account?
