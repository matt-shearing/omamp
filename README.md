# Omamp

A music player for Omarchy, laid out like Winamp 2: a green clock, a
bouncing analyser, a scrolling title, chunky transport buttons and a
playlist underneath. It takes every colour from the current Omarchy theme
and changes with it, so Night Ride gives it pink, cyan and mint, and the
default theme gives it yellow and blue.

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

## Not done yet

- Translations. Every string goes through `qsTr()`, but only English ships.
- Tags for tracks that haven't played yet. Until a track plays, the
  playlist shows a tidied file name and no length.
- An equaliser window and playlist search.

## Licence

MIT. Theme following and the project layout come from Omacut and Monologue
by David Heinemeier Hansson, also MIT.
