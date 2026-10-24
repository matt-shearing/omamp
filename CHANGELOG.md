# Changelog

What changed in each release of Thrum, newest first. Thrum was called Omamp
until 0.4.0, so the entries before it use the old name.

## Unreleased

- **Thrum plays Spotify itself.** With the session helper installed, a
  Spotify track plays through Thrum's own engine instead of on
  spotify_player's Connect device, at 320 kbps. The visualisers move, and
  the volume, equaliser, ReplayGain (from Spotify's loudness figures),
  output device and gapless joins with local tracks all work. It needs
  Spotify Premium. Without the helper, Spotify tracks still play on
  spotify_player.
- **Quitting Thrum stops Spotify.** Thrum's own Spotify playback ends with
  it, and spotify_player is paused if it was playing Thrum's entry.
- **Like follows Thrum's own Spotify track.** While Thrum plays a Spotify
  track itself, Like saves that track (by its id, as Spotify's own app
  does), and the "playing now" line in the Spotify view shows it as
  "Playing in Thrum", with whether it is already liked.
- **Whole playlists, albums and artists play through Thrum.** They go into
  Now Playing as their tracks (an artist's ten best-known), shuffled if you
  asked, so Thrum's queue, shuffle and repeat hold for them. A whole-playlist
  entry already in Now Playing is read into its tracks when its turn comes.
  spotify_player plays them as one only when the session helper isn't
  installed or can't read them.
- **Thrum can be a Spotify Connect device.** Switch on **Let your phone and
  other Spotify apps play on Thrum** in Settings → Spotify, and Thrum
  appears in the device list of your Spotify apps. Music played on it from
  the phone plays through Thrum's engine (volume, equaliser, visualiser) as
  a live entry in Now Playing that shows the phone's track. Thrum's play,
  pause, next and previous reach the phone, and the phone's reach Thrum.
  It needs Spotify Premium; off by default.
- **One Spotify session.** The session helper now reads the catalogue,
  decodes tracks, saves Liked Songs and runs the Connect device in one
  process, where reading and playing were two processes signed in twice.

## 0.4.0

- **Omamp is now Thrum.** The program, its package, the command, the window,
  the bar widget and the website all carry the new name. Nothing else about
  the player changes.
- **Your library and settings come along by themselves.** On its first start
  Thrum moves `~/.config/omamp`, `~/.local/share/omamp` (the library database
  with its `-wal`, `-shm` and backup files) and `~/.cache/omamp` to the same
  places named `thrum`, following `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and
  `XDG_CACHE_HOME`. Each folder moves in one step, and a folder that already
  exists under the new name is kept as it is. What moved is noted in
  `~/.config/thrum/renamed-from-omamp.log`. NAS and Navidrome passwords saved
  in the keyring under Omamp are found and copied to Thrum's name the first
  time they are needed. Music shares and devices Omamp marked
  (`.omamp-volume-id`, `.omamp-sync.json`) are still recognised.
- **What you do by hand:**
  - Quit Omamp before starting Thrum for the first time. If Omamp is still
    running, Thrum says so and waits, so the library is never moved while it
    is open.
  - Install the `thrum` package; it replaces `omamp`. If you installed into
    `~/.local` with `bin/install-user`, remove `~/.local/bin/omamp`,
    `~/.local/bin/omamp-spotify-session`,
    `~/.local/share/applications/omamp.desktop` and
    `~/.local/share/icons/hicolor/scalable/apps/omamp.svg`.
  - Run `omarchy-plugin/install` again (or
    `/usr/share/thrum/omarchy-plugin/install`). The widget is now
    `contra.thrum`; it takes the Omamp widget's place on the bar and removes
    the old one. Its settings start from the defaults.
  - Hyprland window rules: match the class `^thrum$` and the titles
    `Thrum mini` and `Thrum micro`.
  - Scripts: the command is `thrum`, environment variables start with
    `THRUM_`, and the MPRIS name is `org.mpris.MediaPlayer2.thrum`
    (`playerctl -p thrum`).
- **The website** is at <https://matt-shearing.github.io/thrum/> and is
  published on every change to `site/`.

## 0.3.3

- **A visualiser picker.** A waveform button in the large bar, the big Now
  Playing view and the deck opens a gallery of live previews of every style,
  with where the large bar's visualiser sits. The mouse wheel over any
  visualiser flips through the styles.
- **Behind the whole bar.** A new placement runs the visualiser edge to edge
  behind the large now playing bar, fading out at both ends and softening under
  the words. Synthwave was redrawn for it. The big Now Playing view can run its
  visualiser edge to edge too.
- **A mini visualiser in the Omarchy bar.** The bar plugin draws a faint
  version of your visualiser behind the track, from levels Omamp shares over a
  local socket while music plays. Each side has an on/off switch.
- **The setup asks bold or subtle** for the large bar's visualiser.
- **Floating windows fixed.** On Hyprland, switching views and finishing the
  setup no longer leave the window too small or with a gap; Omamp now sizes
  floating windows through Hyprland.
- **The README** alternates dark and light pictures, and there is a one-page
  website in `site/`.

## 0.3.2

- **A short first-run setup.** On a new install, Omamp walks you through where
  it starts, the player's look and visualiser, the library's layout and your
  music, with each choice showing in the window as you pick it. **Help → Run
  setup again…** brings it back. Existing installs get a small banner instead.
- **The sidebar is on by default.**
- **Visualisers in the large now playing bar**, behind the seek bar, along its
  edge or beside the cover, and seven new styles made for a wide, short strip:
  Skyline, Waveform ribbon, Spectrum line, Pulse, LED matrix, Synthwave and
  Star field. A new **Reduce motion** setting calms them all.

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
