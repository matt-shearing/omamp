# Omamp

**A music player and library manager for Arch Linux, built for Omarchy and Hyprland first.**

Omamp has two sides. The player is a Winamp 2–style deck: a green clock, a
bouncing analyser, a scrolling title and a playlist, all in your Omarchy
theme. The library manager is coming next. It brings back the big local
libraries of the 2000s, MediaMonkey-style, with modern tools on top: tagging
from MusicBrainz, smart playlists, file organising with a preview, and
libraries that live on a NAS.

> **Status:** early. The player works today. The library manager (advanced
> mode) is being planned in [docs/plan/](docs/plan/). Expect breaking changes.

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
| `?` | Show every key |
| `Q` | Quit |

Double-click a track to play it.

## Build and install

```sh
bin/build          # build/omamp
bin/test           # tests, offscreen
bin/install        # makepkg -fsi: the package, system-wide (needs sudo)
bin/install-user   # ~/.local only, no sudo
```

Built with C++ and Qt Quick (Qt Multimedia for playback), the same way
Omarchy's own apps are built.

## Coming next

The full plan is in [docs/plan/](docs/plan/). In short:

- **Advanced mode:** a library with a tree, column browser and fast search over 100,000+ tracks.
- **Tagging:** batch editing, MusicBrainz and AcoustID lookups, artwork and lyrics.
- **Organising:** rename and move files by a pattern, with a preview before anything moves.
- **NAS libraries:** SMB, NFS or a Navidrome/Subsonic server, with offline tracks kept in the library.
- **The 2000s comforts:** Auto-DJ, party mode, auto-playlists, ReplayGain, play counts and ratings.

## Known gaps today

- Translations. Every string goes through `qsTr()`, but only English ships.
- Tags for tracks that haven't played yet. Until a track plays, the
  playlist shows a tidied file name and no length.
- An equaliser window and playlist search.

## Licence

MIT. Theme following and the project layout come from Omacut and Monologue
by David Heinemeier Hansson, also MIT.

Omamp is inspired by Winamp and MediaMonkey. It is not affiliated with either.
