# Changelog

What changed in each release of Omamp, newest first.

## 0.3.1

Fixes and layout options from the first days of real use.

**Your layout**

- An Art column: a small album cover at the start of every track list, with
  compact, comfortable and large row heights. Columns can now be dragged into
  any order.
- A sidebar in the style of Spotify's: a big cover, lyrics, about the artist
  and the album (from Wikipedia through MusicBrainz and Wikidata, or from notes
  packaged with the album), credits, up next and track details. Each section
  can be folded, hidden or moved.
- A large now playing bar, at the top or the bottom, as an alternative to the
  compact strip. Settings → Appearance → Layout gathers all of this, with a
  Reset button.

**Fixes**

- The mini and micro players now float, shrink and pin themselves on Hyprland,
  and go back to how the window was when you leave them.
- Omamp follows the system's default sound output when a device disconnects
  and comes back, instead of staying on the device it fell back to.
- Albums from a Navidrome server get their covers from the server.
- The Omarchy bar plugin shows the cover in the bar and as the centrepiece of
  its panel.

## 0.3.0

This release makes Omamp a place to listen as well as a place to manage music:
it shows what is playing in several sizes, plays internet radio, and puts
Spotify beside your own files.

**Now playing**

- The deck, the library and the info panel show the playing track's cover,
  and Omamp sends it over MPRIS, so the Omarchy bar shows it too.
- A big Now Playing view (`N`) fills the window with the cover, the rating,
  the next tracks, timed lyrics and a visualiser, and goes fullscreen with
  `F11`.
- A mini player (`Ctrl+M`) and a micro player (`Ctrl+Shift+M`, Winamp's
  windowshade) for a corner of the screen, with a Hyprland rule to float and
  pin them.
- Five visualisers: bars, bars with peaks, a mirrored spectrum, an
  oscilloscope and fire.
- Three deck styles: Omarchy (follows the theme), Classic (Winamp 2's grey
  and green) and Silver (2000s MediaMonkey).

**Getting around**

- A menu bar with File, Edit, View, Play, Library, Radio, Spotify and Help.
  Every entry shows its key, and an entry that can't run yet is greyed out.
- Buttons to move between the deck, the library and the big view, and a
  first-run hint that says how to open the library.

**Devices and reports**

- Sync profiles copy playlists, collections or a query to a folder, a USB
  drive or a Syncthing folder, converting what you choose with `ffmpeg`.
  A preview lists every copy, conversion and deletion first.
- Any list can be saved as a CSV file or a plain web page.
- The AUR package files are in `aur/`, ready to publish once this release is
  tagged.

**Internet radio**

- Every Nightride FM and Rekt Network channel is built in, with what each one
  is playing now. `Shift+R` opens the picker; `[` and `]` change channel.
- Any other station can be found on radio-browser.info or added by its
  address, and station lists can be imported and exported.

**Spotify**

- With Spotify switched on, your playlists, Liked Songs, saved albums,
  followed artists and search sit in the library, and Spotify tracks can sit
  in Now Playing between your own files. Spotify plays them on its own device
  and Omamp follows along. Omamp never sees your Spotify password: it drives
  spotify_player, which keeps its own sign-in.
- You can use a Spotify app of your own (a client ID), which avoids most of
  the waiting that a shared one causes when Spotify limits how often it may
  ask. Omamp sets it up in spotify_player's settings and keeps a backup.
- A small optional helper, `omamp-spotify-session`, reads the playlists
  Spotify makes and artists' top tracks, which Spotify no longer gives to new
  apps.
- Omamp now waits its turn when Spotify is busy: it queues its requests,
  follows playback over MPRIS instead of asking, shows a countdown while
  Spotify asks it to wait, and lets you cancel a slow list.

**Choosing several tracks, and mixed playlists**

- In the Spotify lists, choose several rows with `Shift` and `Ctrl`, then
  play them, queue them, copy their links, or add them to a playlist.
- An Omamp playlist can hold Spotify tracks beside your own files. They play
  on Spotify when their turn comes, and are skipped while Spotify is off.

**Smaller things**

- Chosen rows are tinted with the theme's accent in light themes, so a
  selection always shows.

## 0.2.0

This release added advanced mode: press `Tab` and the deck becomes a
MediaMonkey-style library manager.

- A library that scans local folders, NAS shares and Navidrome servers, and
  stays quick at 200,000 tracks. Folders on local disks are watched for
  changes, and a moved file keeps its rating, play count and playlists.
- The library tree, a column browser (genre, album artist, album), an album
  grid with covers, an info panel, and the command palette (`Ctrl+K`).
- A query language for searches and auto-playlists, such as
  `genre:jazz rating:4.. year:1950..1969`.
- A tag editor for one track or many, tags from file names, organising files
  by a mask with a preview, and undo for all of them.
- Files to Edit (missing tags, missing covers and more) and a duplicate
  finder that can match by sound.
- Playlists, auto-playlists, collections, play counts, Auto-DJ and party
  mode, and an importer for a MediaMonkey database.
- Identify with MusicBrainz, covers from the Cover Art Archive, lyrics from
  LRCLIB, and scrobbling to ListenBrainz and Last.fm.
- A native playback engine with gapless playback, crossfade, ReplayGain and a
  10-band equaliser, behind a setting while it is tried out.
- A command line for all of it, with `--json` for scripts.

## 0.1.0

The first release: a Winamp 2–style player in the Omarchy theme.

- A deck with a green clock, a bouncing analyser, a scrolling title and a
  playlist, in the colours of the current Omarchy theme.
- Winamp's keys, MPRIS for the media keys and `playerctl`, and the playlist,
  volume, shuffle and repeat remembered between runs.
- A bar plugin for the Omarchy bar with the track, the play state and a
  panel of controls.
