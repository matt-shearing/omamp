# Omamp

**A music player and library manager for Arch Linux, built for Omarchy and Hyprland first.**

Omamp has two sides. The player is a Winamp 2–style deck: a green clock, a
bouncing analyser, a scrolling title and a playlist, all in your Omarchy
theme. Press `Tab` and it becomes a MediaMonkey-style library manager for big
local libraries: a library tree, a column browser, fast search over 200,000
tracks, a tag editor with undo, file organising with a preview, auto-playlists,
Auto-DJ, MusicBrainz tagging, synced lyrics, libraries that live on a NAS
or a Navidrome server, sync to a phone, USB drive or Syncthing folder, and
reports as CSV or HTML.

> **Status:** early (0.2.0). Everything below is built and tested (384
> automated tests), but only against made-up music, mock web services and
> simulated network shares. It has not yet been tried on a real large library
> or a real NAS. Expect rough edges and breaking changes. The plan and a
> detailed progress log are in [docs/plan/](docs/plan/).

## Getting around

Everything Omamp does is in the **menu bar** across the top of the window:
File, Edit, View, Play, Library, Radio, Spotify and Help. Each entry shows
its key, so the menus also teach the shortcuts. An entry is greyed out when
it can't run yet, for example Edit tags before you have chosen any tracks,
or anything party mode has locked.

![The deck with the menu bar and the Library button](docs/menu-deck.png)

Omamp has three views, and **View** switches between them:

- **Player** is the Winamp-style deck.
- **Library** is the MediaMonkey-style library manager.
- **Now Playing** is the big cover view.

You can also switch without the menu. On the deck, the **Library** and
**Now Playing** buttons sit in the deck's title bar. In the library, the
**Player** button at the right of the top strip goes back to the deck. `Tab`
switches between the deck and the library, and `N` opens Now Playing.

![The View menu open](docs/menu-open.png)

To search Spotify, choose **Spotify → Search Spotify…**. It opens the
library on Spotify with the search box ready. In the library, the
**Library**, **Spotify** and **Both** buttons beside the search box choose
where a search looks.

![The library with the menu bar and the Player button](docs/menu-library.png)

`Ctrl+K` opens the command palette, which finds any command, artist, album or
track by typing part of its name. `?` lists every key. To hide the menu bar,
untick **View → Show the menu bar**. `F10` or `Alt` brings it back for a
moment, and the same View entry turns it back on.

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

## Now playing: covers, the big view and the mini players

The deck shows the playing track's cover beside the green clock. So do the
strip in advanced mode, the top of Now Playing and the info panel. Covers
come from the library, from a Navidrome server, or, for a file outside the
library, from its tags or its folder, and work on the deck without opening
the library. Omamp sends the cover over MPRIS too, so the Omarchy bar and
`playerctl` show it.

![The deck with the cover, in the Night Ride theme](docs/deck-with-art.png)

- **The big Now Playing view** (`N`, or click the cover) fills the window
  with the cover, the title, artist, album and year, the rating as stars you
  can click, the position, the transport, the next five tracks, timed lyrics
  scrolling with the song, and a visualiser. The background is the cover,
  blurred. `F11` makes it fullscreen and `Esc` comes back. Party mode's
  display is the same view.
- **The mini player** (`Ctrl+M`) is a 420 by 64 strip: cover, scrolling
  title, seek, transport and volume. **The micro player** (`Ctrl+Shift+M`)
  is Winamp's windowshade, 275 by 28. Each remembers its size, and
  `omamp mini` starts in one. A Hyprland rule floats and pins them; it is in
  [docs/now-playing.md](docs/now-playing.md).
- **Visualisers**: bars, bars with peaks, a mirrored spectrum, an
  oscilloscope and fire. Click the visualiser or press `Shift+V` for the
  next one.
- **Deck styles** (Settings → Appearance): Omarchy follows the theme;
  Classic is Winamp 2's grey bevels and green readout; Silver is the
  brushed silver and blue of 2000s MediaMonkey. The library window keeps the
  Omarchy theme whatever the deck wears.

![The big Now Playing view](docs/now-playing-big.png)

![The big Now Playing view in Omarchy's default theme](docs/now-playing-big-default.png)

![The mini player](docs/mini-player.png)

![The micro player](docs/micro-player.png)

![The Classic deck style](docs/deck-classic.png) ![The Silver deck style](docs/deck-2000s.png)

![The five visualisers](docs/visualisers.png)

[docs/now-playing.md](docs/now-playing.md) has the details: where covers
come from, every key, the Hyprland rule, and the styles.

## Internet radio

Every Nightride FM and Rekt Network channel is built in: Nightride FM,
Chillsynth, Datawave, Spacesynth, Darksynth, Horrorsynth and EBSM, and Rekt,
Rektory, Rektify and D-Notive. The picker shows each channel's colour, its
genres and the song it is playing right now, from Nightride's live feed.
Press `Shift+R` on the deck, or choose **Radio** in the library tree, and
click a channel or press `Enter`. While a station plays, `[` and `]` change
channel.

![The Nightride FM channels](docs/radio-channels.png)

A station plays live: the deck shows **LIVE** and the time listened, seeking
is off, and the song's title and artist show everywhere, the bar's MPRIS
widget included. **Find stations…** searches radio-browser.info for any
other station; **Add address…** keeps a stream address of your own. Other
stations' song names come from the titles inside their streams.

![The deck playing Darksynth](docs/radio-deck-live.png)

Radio never counts as library plays, and radio songs are scrobbled only when
Settings say so. `omamp radio list|play|search|add` does the same from a
terminal. [docs/radio.md](docs/radio.md) has the details.

## Advanced mode: the library

Press `Tab` to switch between the deck and advanced mode. In advanced mode the
deck folds into a strip across the top, and the library fills the rest of the
window: a tree on the left, the tracks on the right, a search box above them,
and a status line below. Omamp remembers which mode you left it in and the
window size for each.

![Advanced mode with the column browser and the info panel, in the Night Ride theme, with a small made-up library](docs/advanced-mode.png)

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
- **The column browser** (`Ctrl+B`, or **Browser**) puts three lists above
  the tracks: Genre, Album Artist and Album, as in foobar2000 and
  MediaMonkey. Each lists its values with their number of tracks, within the
  tree node and the search. Choosing a genre narrows the artists, the albums
  and the tracks; `Ctrl`+click picks several, and **All** goes back. The
  middle list can show every artist instead of album artists.
- **The album grid** (`G`, or **Albums**) shows the albums of the list as
  covers. Double-click or `Enter` plays an album; right-click or
  `Ctrl+Enter` for **Show tracks**. Covers come from `cover.jpg`, `folder.jpg`
  or `front.jpg` beside the music, or from the files' own tags, and fill in
  while you look.
- **The info panel** (`I`, or **Info**) shows the cover and everything the
  library knows about the chosen track, or the one playing: tags, format,
  ReplayGain, rating, plays, when it last played, whether it has lyrics, and
  where the file is.
- **The command palette** (`Ctrl+K`) finds actions, artists, albums and
  tracks as you type. `Enter` runs the action or goes to the artist, album or
  track. Start with `>` for actions only.
- **Folders keep themselves up to date.** Omamp watches the library folders
  on local disks and rescans only the folders that change; the list keeps
  your selection and your place. Shares on the network are checked every 15
  minutes. The status line says what is watched.

### Tags, files and tidying up

- **Edit tags** (`Ctrl+E`, or right-click **Edit tags…**) opens the tag
  editor for the selected tracks: title, artists, album artist, album,
  genres, composers, year, track and disc numbers, BPM, comment, rating and
  five custom fields, with tabs for the artwork and the lyrics. With several
  tracks chosen, a field they disagree on shows **‹keep›**: each track keeps
  its own value unless you type a new one, and the **✕** empties it in all of
  them. Edited fields are marked. **Save** (`Ctrl+Enter`) writes only what you
  changed, safely (to a copy that replaces the file only once it checks
  out), and any file that fails is listed with the reason. The artwork tab
  shows the picture in the file, or the folder's `cover.jpg`, and takes a
  new one from a file, from the clipboard, or removes it.
- **`F2`** edits the title of the track under the cursor in place.
- **Organise files** (`Ctrl+Shift+O`) renames and moves the selected files by
  a mask such as `<Album Artist>/<Album> (<Year>)/<Track#:2> <Title>`, with a
  preview of every old and new place before anything moves. Type `<` or `$`
  for a list of fields and functions; a mistake in the mask says where it is.
  Double-click a new place to change it by hand. A name that is taken shows
  as a conflict and that file is left where it is; existing files are never
  replaced. Pictures and cue sheets can go with their folder, and folders
  left empty can be removed. [docs/masks.md](docs/masks.md) has the mask
  language.
- **Tag from file names** (right-click) reads tags out of the files' names
  and folders with a pattern such as `<Track#> - <Title>`, showing what each
  file would get before it writes anything.
- **Undo** (`Ctrl+Z`) takes back the last tag edit, tags from file names or
  organise, files and library together, and says what it undid; `Ctrl+Shift+Z`
  does it again. **Undo history** (from `Ctrl+K`) lists every change with
  what it changed, and can undo any of them.
- **Files to Edit**, at the bottom of the tree, lists the checks MediaMonkey
  has, with how many tracks each finds: unknown title, artist, album, genre
  or year, missing artwork, album artist, year or genre differing within an
  album, track-number gaps, not levelled, dead links, offline and more.
  Choose one to see its tracks.
- **Duplicates**, under Files to Edit, finds the same song more than once:
  by artist and title, by identical files, or by how it sounds (Chromaprint's
  `fpcalc`, so an MP3 and a FLAC of one recording match). Each group
  suggests the copy to keep and says why. Tick the others to take them out
  of the library, or move their files to the desktop's trash, after asking.
  Nothing is ever deleted outright.
- Dates read the same everywhere: "12 Jul 2026", not "7/12/26".

### Playlists, Auto-DJ and party mode

- **Playlists** live in the tree under Playlists: folders, playlists in your
  own order, and auto-playlists, nested as deep as you like. Right-click
  Playlists or a folder for **New playlist**, **New auto-playlist**, **New
  folder**, **Import playlists** (M3U, M3U8 and PLS, through the desktop's
  file picker) and **Save Now Playing as a playlist**; right-click a playlist
  to play it, rename it, move it into a folder, export it as M3U8 or delete
  it. Everything is in the palette (`Ctrl+K`) too.
- Drag tracks from the list onto a playlist in the tree, or use **Add to
  playlist** in a track's menu. A playlist shows in its own order:
  `Alt+↑` and `Alt+↓` (or a drag) move the chosen entries, `Delete` takes
  them out. A track that is in the list twice is marked **repeat**; an entry
  whose file isn't in the library stays, marked **not in library**, so an
  exported playlist is never shorter than the one you imported.
- **Auto-playlists** are a query that updates by itself
  ([docs/query-language.md](docs/query-language.md)). The editor checks the
  query as you type, says how many tracks it picks and shows them. Below
  the query, rules build the same text from rows of field, operator and
  value, matching all or any, with an order and a limit by tracks, playing
  time or size. The text is what is saved; a query the rows can't show (with
  brackets, say) is marked as advanced and edited as text. A random one
  keeps its order until **New shuffle**.
- **Collections** (right-click Collections) are a saved query with their
  own tree nodes (Artist, Album, Genre…) and their own columns; columns
  hidden or shown while a collection is open are remembered for it.
- **Play counts** go up as you listen: a track counts once you have heard
  half of it or four minutes, and pressing Next before a tenth of it counts
  as a skip. Seeking forward is not listening. Plays and Last played update
  in the list as they happen, and listens queue for ListenBrainz or Last.fm
  when one is set up.
- **Auto-DJ** (`Ctrl+J`, or the **AUTO-DJ** lamp in the deck strip) keeps a
  number of tracks queued after the one playing, from the whole library, a
  collection, a playlist or a query, without repeating an artist too soon or
  a track heard lately, favouring higher ratings. **Auto-DJ settings…** in
  the palette sets all that.
- **Party mode** (palette) lets guests use the player without touching your
  library: tag editing, playlists, organising, settings and quitting are
  locked, with an optional PIN to leave, and an optional big now-playing
  display. The **PARTY** lamp shows it is on; click it to leave.
- **Settings** (`Ctrl+,`): the playback engine, crossfade, ReplayGain with
  preamp and clip prevention, a 10-band equaliser with presets; library
  folders, watching and the network rescan interval, ratings in files;
  ListenBrainz (paste a token and check it) and Last.fm; the AcoustID key,
  MusicBrainz preferences and cover size.

### MusicBrainz, covers, lyrics and the NAS

- **Identify with MusicBrainz** (`Ctrl+I`, a track's or an album's menu, or
  the palette) works as Picard does. The chosen tracks are grouped into
  albums. Each album gets MusicBrainz releases, best first, with a score, a
  confidence (high, medium or low) and the reasons. Below that you see which
  file went to which track, how well each fits, and every tag that would
  change, old and new. Pick another release from the list, or paste a
  release's ID or link. **Save tags** writes each ticked album, with its
  front cover if you want it; `Ctrl+Z` undoes it. Without an AcoustID key
  (Settings → Web), tracks are matched by their tags and any MusicBrainz IDs
  already in them, and the sheet says so. When MusicBrainz can't be reached,
  it says that too.
- **Covers**: **Find missing covers** (the palette, the album grid's menu,
  or Files to Edit → Missing artwork) fetches a front cover from the Cover
  Art Archive for every album without one, as a job you can cancel. **Find
  a cover…** on one album shows the candidates side by side when there are
  several.
- **Lyrics**: the info panel's Lyrics tab (`I`, then Lyrics, or **Show
  lyrics** in the palette) shows the words from a `.lrc` file beside the
  track, from its tags, or from LRCLIB. Timed lyrics follow the playing
  track, with the line being sung lit; click a line to jump there. **Fetch
  from LRCLIB**, **Save as .lrc** and **Save into the file** are buttons;
  saving can be undone.
- **Scrobbling** (Settings → Scrobbling): ListenBrainz with a pasted token;
  Last.fm by logging in through its web page, with your own API key; the
  queue's waiting, sent and refused counts; **Send now**.
- **Volumes** (the palette, Settings → Library, or Location's menu in the
  tree) lists every disk, NAS share and server the library uses, whether it
  is connected, and its tracks. Rescan or quick-rescan one, point it at a new
  folder when the music moved, forget one that is gone, or set how many
  folders it reads at once. Shares are checked on a separate thread, which
  gives up after 1.5 seconds, so a NAS that is asleep never freezes the
  window. Its tracks stay in the library, greyed, with a tooltip saying which
  share isn't connected; when it comes back, Omamp rescans it and the jobs
  bar says so.
- **Connect a NAS…** turns a share's name (`//nas/music` or
  `nas:/volume1/music`) into the two systemd units and the commands that
  mount it on demand, each with a Copy button. Omamp never runs `sudo` or
  mounts anything itself. The full guide is [docs/nas.md](docs/nas.md).
- **Navidrome and other Subsonic servers** (**Add Navidrome / Subsonic
  server…**): the address, a user and password or an API key, **Test the
  connection**, and a streaming quality. The password is kept in the desktop
  keyring, never in the library. The server's tracks join the library with a
  small cloud mark and its covers; sync again or remove the server from the
  Volumes page.

![Identify with MusicBrainz: releases, scores and the tag changes](docs/identify.png)

![Synced lyrics in the info panel](docs/lyrics.png)

![Volumes: a local disk, a NAS share that isn't connected, and a Navidrome server](docs/volumes.png)

![Adding a Navidrome server](docs/remote-source.png)

![Playlists in the tree, with a repeat and an entry not in the library](docs/playlists.png)

![The auto-playlist editor: query, rules and preview](docs/autoplaylist-editor.png)

![Settings: playback and the equaliser](docs/settings-playback.png)

![The tag editor, three tracks at once](docs/tag-editor.png)

![Organise files: the preview](docs/organise.png)

![Files to Edit, with the info panel](docs/files-to-edit.png)

![The album grid with the info panel](docs/album-grid.png)

![The command palette](docs/command-palette-live.png)

It stays quick with big libraries. With 200,000 tracks, advanced mode opens
in about a third of a second. A search, a sort, a choice in the column
browser and the album grid all reach the screen in under 150 ms, most in
under 100 ms.

![Advanced mode with 200,000 synthetic tracks](docs/advanced-mode-200k.png)

### Devices and reports

A sync profile copies playlists, auto-playlists, collections or a query to a
folder, a USB drive or a Syncthing folder. Format rules convert what you
choose with `ffmpeg` (for example lossless files to Opus at 128 kbit/s) and
copy the rest. **Preview** lists what a sync would copy, convert and delete
before anything is written; **Sync now** runs as a job. A second sync of an
unchanged list copies nothing, files the list no longer holds are removed,
and other files stay unless the profile says to delete them. Each playlist is
also written as an `.m3u8` beside the music. Profiles sit under **Devices** in
the tree.

Any list can be saved as a CSV file for a spreadsheet or as a plain web page.
The guide is [docs/sync.md](docs/sync.md).

![The Devices page with a preview of the next sync](docs/devices.png)

## Spotify (optional)

Switch on **Settings → Spotify** to put Spotify beside your own music: a
Spotify node in the library tree with your playlists, Liked Songs, saved
albums, followed artists and search; Spotify results under the library search
and in the command palette; and Spotify tracks in Now Playing between your
own files. When a Spotify track's turn comes, Spotify plays it on its Spotify
Connect device and Omamp follows it on the deck, the big view, the mini
players, MPRIS and the bar.

Omamp never sees your Spotify password. It drives
[spotify_player](https://github.com/aome510/spotify-player), which keeps its
own sign-in: **Sign in…** runs `spotify_player authenticate` in a terminal.
Spotifast works too, for playing links and control only. Playing needs
Spotify Premium; a Free account can browse and search. Gapless joins,
crossfade, the equaliser and the visualiser stay with your own files.
[docs/spotify.md](docs/spotify.md) has the details and the reasons.

![The Spotify node](docs/spotify-panel.png)

## Omarchy bar plugin

`omarchy-plugin/` adds Omamp to the Omarchy bar: the track and play state in the bar, and a panel with the cover, seek, transport, shuffle, repeat and volume. Install it with `omarchy-plugin/install`. See [its README](omarchy-plugin/README.md).

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
| `N` | The big Now Playing view, and back |
| `F11` | Fullscreen Now Playing; `Esc` leaves it, then the big view |
| `Shift+V` | Next visualiser style (or click the visualiser) |
| `Shift+R` | Radio: the channel picker |
| `[` `]` | Previous or next radio channel, while a station plays |
| `Shift+L` | Lyrics in the big view on or off |
| `Ctrl+M` | The mini player, and back |
| `Ctrl+Shift+M` | The micro player, and back |
| `Tab` | Switch between the deck and the library (advanced mode) |
| `/` or `Ctrl+F` | Search the library (opens it from the deck) |
| `Ctrl+B` `G` `I` | The column browser, the album grid, the info panel (opens the library from the deck) |
| `F10` | Open the menu bar's File menu; shows the menu bar if it is hidden |
| `Alt` | Show or hide a hidden menu bar |
| `Ctrl+K` | The command palette |
| `Ctrl+,` | Settings |
| `?` | Show every key |
| `Q` | Quit |

Double-click a track to play it.

In advanced mode, the arrow keys move through the tree and the track list
instead of the playlist, and these keys work too:

| Key | Does |
| --- | --- |
| `Ctrl+K` | The command palette (on the deck too) |
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
| `Ctrl+B` | Show or hide the column browser (`B` stays Winamp's next track) |
| `↑` `↓` in the browser | Choose a value; `Shift` for a range, `Ctrl+Space` for one more |
| `←` `→` in the browser | Move between its lists and to the tree; `Enter` goes to the tracks |
| `G` | Album covers instead of the track list, and back |
| `Enter`, `Ctrl+Enter` in the grid | Play the album; show its tracks |
| `I` | Show or hide the info panel |
| `Ctrl+E` | Edit the tags of the selected tracks (in the grid, the chosen album's) |
| `F2` | Edit the title of the track under the cursor; `Enter` saves, `Esc` leaves it |
| `Ctrl+Shift+O` | Organise files: rename and move the selected tracks by a mask |
| `Ctrl+I` | Identify the selected tracks (in the grid, the chosen album) with MusicBrainz |
| `Ctrl+Z` | Undo the last tag edit or organise |
| `Ctrl+Shift+Z` or `Ctrl+Y` | Redo what was undone |
| `Ctrl+Enter` or `Ctrl+S` in an editor | Save, organise or tag |
| `Esc` in an editor | Close it (asking first if there are unsaved changes) |
| `Menu` in the tree | New playlist, auto-playlist or folder; rename, move, export or delete |
| Drag from the list | Add the selected tracks to a playlist in the tree |
| `Alt+↑` `Alt+↓` in a playlist | Move the chosen entries up or down |
| `Delete` in a playlist | Remove the chosen entries from the playlist |
| `Delete` in the tree | Delete the chosen playlist, folder or collection (asks first) |
| `Ctrl+J` | Auto-DJ on or off (on the deck too) |
| `Ctrl+,` | Settings (on the deck too) |

## Build and install

```sh
bin/build          # build/omamp
bin/test           # tests, offscreen (needs ffmpeg for the library tests)
bin/bench          # times the library on 200,000 synthetic tracks
bin/install        # makepkg -fsi: the package, system-wide (needs sudo)
bin/install-user   # ~/.local only, no sudo
```

An AUR package (`PKGBUILD` and `.SRCINFO`) is in [aur/](aur/), ready but not
yet published.

Built with C++ and Qt Quick (Qt Multimedia for playback), the same way
Omarchy's own apps are built.

## Coming next

The full plan is in [docs/plan/](docs/plan/). Next up:

- **Real-world testing:** a real large library, a real NAS share, a real
  Navidrome server, and a day of listening on the native (gapless) engine
  before it becomes the default.
- **Real-world sync:** a real phone over USB and a real Syncthing folder.
- **Release:** the AUR package is ready in [aur/](aur/) but not published;
  then a proposal to Omarchy's package list.

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
omamp sync create Phone --to /run/media/$USER/PHONE/Music --kind usb \
    --source 'playlist:Road trip' --rule 'lossless->opus:128'
omamp sync run Phone --dry-run           # what a sync would copy, convert and delete
omamp sync run Phone
omamp report 'rating:5' --out five-stars.csv
omamp spotify status                     # is spotify_player signed in and running? Premium?
omamp spotify search 'neon coast' --json # Spotify's tracks, albums, artists and playlists
omamp radio list --group nightride       # the Nightride FM channels
omamp radio play darksynth               # tune the running Omamp in
omamp radio search 'jazz'                # find stations on radio-browser.info
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
- Searching inside a playlist, and dragging tracks from the album grid.
- Last.fm needs your own API account: Omamp doesn't ship a key.
- A change of playback engine takes effect at the next start.

## Licence

MIT. Theme following and the project layout come from Omacut and Monologue
by David Heinemeier Hansson, also MIT.

Omamp is inspired by Winamp and MediaMonkey. It is not affiliated with either.
