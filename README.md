# Omamp

**A music player and library manager for Arch Linux, built for Omarchy and Hyprland first.**

Omamp has two sides. The player is a Winamp 2–style deck: a green clock, a
bouncing analyser, a scrolling title and a playlist, all in your Omarchy
theme. The library manager is coming next. It brings back the big local
libraries of the 2000s, MediaMonkey-style, with modern tools on top: tagging
from MusicBrainz, smart playlists, file organising with a preview, and
libraries that live on a NAS.

> **Status:** early. The player works today. The first part of the library
> manager (advanced mode: the tree, the track list, search and scanning)
> works too; the rest is planned in [docs/plan/](docs/plan/). Expect breaking
> changes.

## Why Omarchy first

Omamp follows the desktop instead of fighting it. Every colour comes from the
current Omarchy theme and changes when the theme does. Window corners match
Hyprland's rounding. Media keys, `playerctl` and the Omarchy bar control it
over MPRIS. Everything works from the keyboard. It builds and installs as an
Arch package. It runs on any Linux desktop, but Omarchy is where it looks right.

## The player

![Omamp in the Night Ride theme](docs/omamp-night-ride.png)

It plays local files: MP3, FLAC, Ogg, Opus, M4A/AAC, WAV and the rest of
what FFmpeg reads. Drop files or folders on the window, press `O` to pick a
folder, or pass them on the command line (`omamp ~/Music/Album`). The first
time it starts with nothing to play, it adds `~/Music`. The playlist, the
track you were on, the volume, shuffle and repeat come back the next time
you open it.

Omamp speaks MPRIS, so the media keys, `playerctl` and the Omarchy bar can
see and control it.

![Omamp in Omarchy's default theme](docs/omamp-default.png)

## Advanced mode: the library

Press `Tab` to switch between the deck and advanced mode. In advanced mode the
deck folds into a strip across the top, and the library fills the rest of the
window: a tree on the left, the tracks on the right, a search box above them,
and a status line below. Omamp remembers which mode you left it in and the
window size for each.

![Advanced mode with a small made-up library](docs/advanced-mode.png)

- **Add folder…** adds a music folder to the library and scans it. **Rescan**
  (or `F5`) checks every library folder again. Scans run in the background,
  with their progress and a Cancel button in the status line.
- **The tree** works like MediaMonkey's: Entire Library, Collections, Artist,
  Album Artist, Album (by first letter), Genre, Year (by decade), Rating,
  Composer, Location (disks and shares, then folders) and Playlists. Disks and
  shares that aren't connected show dimmed; their tracks stay in the library.
- **The track list** shows every track under the chosen node that matches the
  search. Click a column header to sort, again to reverse, and a third time
  for the library order. Drag a header's edge to resize a column. Right-click
  a header, or press **Columns**, to choose the columns.
- **Search** uses the same query language as `omamp query`: plain words,
  `artist:"neon coast"`, `year:1990..1999`, `rating:4..`, `-genre:christmas`.
  A query with a mistake in it says what is wrong and leaves the list as it was.
- Double-click a track or press `Enter` to play the list from that track.
  Right-click for Play next, Add to Now Playing and Show in folder.

It stays quick with big libraries. With 200,000 tracks, advanced mode opens
in about a third of a second and a search reaches the screen in under 100 ms.

![Advanced mode with 200,000 synthetic tracks](docs/advanced-mode-200k.png)

## Omarchy bar plugin

`omarchy-plugin/` adds Omamp to the Omarchy bar: the track and play state in the bar, and a panel with seek, transport, shuffle, repeat and volume. Install it with `omarchy-plugin/install`. See [its README](omarchy-plugin/README.md).

![The Omamp bar plugin](docs/omarchy-plugin.png)

## Keys

| Key | Does |
| --- | --- |
| `Space` | Play or pause |
| `Z` `X` `C` `V` `B` | Previous, play, pause, stop, next (Winamp's own keys) |
| `←` `→` | Back or forward 5 seconds |
| `↑` `↓` | Move through the playlist |
| `Enter` | Play the selected track |
| `Delete` | Remove the selected track |
| `-` `+` | Volume down or up |
| `S` | Shuffle on or off |
| `R` | Repeat on or off |
| `O` | Add a music folder |
| `T` | Show time left or time played (or click the clock) |
| `Tab` | Switch between the deck and advanced mode |
| `?` | Show every key |
| `Q` | Quit |

Double-click a track to play it.

In advanced mode, the arrow keys move through the tree and the track list
instead of the playlist, and these keys work too:

| Key | Does |
| --- | --- |
| `/` or `Ctrl+F` | Search. `Esc` clears the search; `Enter` or `↓` goes to the list |
| `↑` `↓` `Page Up` `Page Down` `Home` `End` | Move through the tree or the list |
| `→` `←` in the tree | Open or close a node, or step into it or out to its parent |
| `Enter` in the tree, `←` in the list | Go from the tree to the list, and back |
| `Shift+↑` `Shift+↓` | Select a range of tracks |
| `Ctrl+Space` | Select or unselect the track under the cursor |
| `Ctrl+A` | Select every track in the list |
| `Enter` | Play the list from the selected track (or play the selected tracks) |
| `Menu` | Play next, add to Now Playing or show in folder |
| `O` | Add a folder to the library |
| `F5` | Rescan the library |

## Build and install

```sh
bin/build          # build/omamp
bin/test           # tests, offscreen (needs ffmpeg for the library tests)
bin/bench          # times the library on 200,000 synthetic tracks
bin/install        # makepkg -fsi: the package, system-wide (needs sudo)
bin/install-user   # ~/.local only, no sudo
```

Built with C++ and Qt Quick (Qt Multimedia for playback), the same way
Omarchy's own apps are built.

## Coming next

The full plan is in [docs/plan/](docs/plan/). In short:

- **Advanced mode:** a column browser, an album-art grid, an info panel and a command palette.
- **Tagging:** batch editing, MusicBrainz and AcoustID lookups, artwork and lyrics.
- **Organising:** rename and move files by a pattern, with a preview before anything moves.
- **NAS libraries:** SMB, NFS or a Navidrome/Subsonic server, with offline tracks kept in the library.
- **The 2000s comforts:** Auto-DJ, party mode, auto-playlists, ReplayGain, play counts and ratings.

## The library from the command line

The library manager's core also works from the command line, and the
window and the command line share one library.

```sh
omamp scan ~/Music                       # add a folder to the library and scan it
omamp scan                               # rescan every library folder; unchanged files are only stat'ed
omamp query 'artist:"neon coast" year:1997..2001'
omamp query 'rating:4.. -genre:christmas' --json
omamp stats
omamp scan --quick                       # rescan, skipping folders whose modification time is unchanged
omamp watch                              # watch the library folders and rescan what changes (Ctrl+C stops)
omamp locate /mnt/usb/Music              # find missing tracks that were moved there
omamp art album 12 --size 256            # print the path of an album's picture, made if needed
```

A file moved or renamed inside the library keeps its rating, play count,
history and playlists: a scan recognises it by its size, length and tags, or
by its contents. Folders on network shares are never watched (a share's
changes made by other machines can't be seen); `omamp watch` rescans them
every 15 minutes instead, by folder modification time.

Every command takes `--json` and `--db <path>`, and exits 0 or 1. The library
is `~/.local/share/omamp/library.db`. Run `omamp help` for the query syntax.
Tracks on a disk or share that isn't mounted show as offline and are never
removed.

## Known gaps today

- Translations. Every string goes through `qsTr()`, but only English ships.
- Tags for tracks added from a folder or by dropping files (rather than
  from the library) that haven't played yet. Until such a track plays, the
  playlist shows a tidied file name and no length.
- An equaliser window and playlist search.

## Licence

MIT. Theme following and the project layout come from Omacut and Monologue
by David Heinemeier Hansson, also MIT.

Omamp is inspired by Winamp and MediaMonkey. It is not affiliated with either.
