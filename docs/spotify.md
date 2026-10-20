# Spotify in Omamp

Omamp can show Spotify beside your own music. Switch it on in **Settings →
Spotify**. A **Spotify** node then appears at the bottom of the library tree
with your playlists, Liked Songs, saved albums and followed artists, and a
search. Spotify tracks can sit in Now Playing between your own files. When a
Spotify track's turn comes, Spotify plays it and Omamp follows: the deck, the
strip, the big view, the mini players, the media keys and the Omarchy bar show
the track, its cover and its position.

It is off until you switch it on. Playing a track needs Spotify Premium; a
Free account can browse and search.

![The Spotify node in the library](spotify-panel.png)

![Spotify results under the library search and in the command palette](spotify-search-blend.png)

![Settings → Spotify](settings-spotify.png)

## Signing in

Omamp never asks for your Spotify password and keeps no Spotify token. A
separate program talks to Spotify and keeps its own sign-in; Omamp runs that
program's command line.

With **spotify_player**, the recommended backend:

1. Install it if it is missing: `omarchy-pkg-add spotify-player`.
2. In Settings → Spotify, switch on **Show Spotify in Omamp** and press
   **Sign in…**. A terminal opens running `spotify_player authenticate`.
3. Your browser opens on Spotify's permission page. Allow it. spotify_player
   then asks a second time, for playback; allow that too.
4. Close the terminal. Omamp notices the sign-in within a few seconds, starts
   `spotify_player --daemon` in the background, and fills in the status.

spotify_player keeps the sign-in in `~/.cache/spotify-player`. Omamp only
checks that its files are there (`credentials.json` and a
`<client id>_token.json`) and never opens them. To sign out, delete those two
files or run `spotify_player authenticate` again.

With **Spotifast**, sign in inside Spotifast's own window. Omamp can use it
only while it is open.

## What you can do

In the Spotify node:

- **Your playlists, Liked Songs, Saved albums, Followed artists** on the left.
- **Search Spotify**: tracks, albums, artists and playlists, under headings.
- **Play**, **Play next**, **Add to Now Playing** and **Add to playlist** act
  on the chosen row. A track row plays the list from that track, as the
  library does. A playlist, album or artist row plays all of its tracks;
  Enter or a double-click opens it instead. Backspace or **‹ Back** returns.
- **Play a Spotify link**: paste an `open.spotify.com` link or a `spotify:`
  URI. A track plays next; an album, playlist or artist opens.
- The line along the top shows what Spotify is playing, whoever started it,
  with **Like** for Liked Songs.

Elsewhere:

- **The library search** shows Spotify's answers in a strip under the track
  list, with the library's tracks headed "Local". Double-click a track tile
  to play it next, or anything else to open it.
- **The command palette** (`Ctrl+K`) lists Spotify's answers after the
  library's, under "Local" and "Spotify". Choosing a track plays it next and
  starts it; choosing an album, playlist or artist opens it.
- **Spotify tracks in Now Playing** stay there between runs, like Navidrome
  tracks. Their entries read `spotify://track/<id>`.

Both kinds of blending can be switched off in Settings → Spotify.

## The command line

`omamp spotify` works without the window and takes `--json`, like every
command:

```text
omamp spotify status              installed, signed in, running, account, Premium, devices
omamp spotify search <text>       tracks, albums, artists and playlists
omamp spotify playlists|liked|albums|artists
omamp spotify tracks <uri>        a playlist's or album's tracks, an artist's top tracks
omamp spotify play <uri or link>  play it on the backend's Spotify Connect device
omamp spotify now                 what is playing
omamp spotify pause|resume
          [--backend spotify_player|spotifast]
```

`status` prints the next step when something is missing, for example
`sign in, in a terminal: spotify_player authenticate`.

## What crosses over and what doesn't

Spotify's sound never passes through Omamp. Spotify plays on its own Spotify
Connect device (spotify_player's, named `spotify-player` unless its
`app.toml` says otherwise) and Omamp pauses its own engine. So:

- **No gapless join, crossfade, ReplayGain or equaliser** between a local
  track and a Spotify track, or between two Spotify tracks. Starting each
  Spotify track takes about a second.
- **The visualiser rests** while Spotify plays; it has no sound to draw.
- **The volume slider and mute** act on Omamp's own sound. Spotify's volume
  is Spotify's.
- **Like** acts on the playing track only: spotify_player's command line can
  save or remove nothing else.
- **Position** comes from reading Spotify's state every second, and moves
  smoothly between reads.
- **A pause from another device** shows as paused in Omamp. If someone
  starts something else in Spotify, Omamp pauses its queue and says so.

## The two backends

| | spotify_player | Spotifast |
|---|---|---|
| Your playlists, Liked Songs, albums, artists | Yes | No |
| Search | Yes | No |
| Spotify tracks in Now Playing | Yes | Yes, from links you paste |
| Pause, resume, seek, Like | Yes | Yes |
| Runs | In the background, started by Omamp | Only while its window is open |

## Why it is built this way

**Omamp drives a program that already speaks to Spotify, and spotify_player
is the main one.** Spotify allows playback in third-party apps only through
librespot (a reverse-engineered Spotify Connect client) and only for Premium
accounts. Omamp could link librespot itself, but then it would have to hold a
Spotify sign-in and its tokens, keep up with every change Spotify makes, and
carry a Rust library in a C++ app. spotify_player already does all of that,
is packaged for Arch, runs as a daemon, and answers on its command line in
JSON: library, search, playback state, play, pause, seek, Like, add to a
playlist and choose a device. That gives the whole feature without Omamp ever
touching a credential.

**Spotifast is a second choice, for control only.** It is a good Spotify app
and Matt already uses it, but its command line plays a link, pauses, seeks and
reports what is playing; it has no search or library. Omamp uses it for
exactly that. The official Spotify app offers only MPRIS, so it isn't a
backend.

**The queue stays Omamp's.** Each Spotify track is a Now Playing entry, and
Omamp starts it on Spotify when its turn comes (`playback start track`), so
local and Spotify tracks mix in one list in your order. Handing whole
playlists to Spotify would have been simpler, but then Omamp's queue, shuffle
and repeat would stop meaning anything.

**One seam in the player.** `HandoffEngine` (`src/engine/handoffengine.h`)
sits in front of the real engine. It passes `spotify://` entries to
`SpotifyPlayback` and everything else to the engine. When Spotify plays next,
the engine is given nothing to queue, so it ends with "end of queue"; that is
the moment Spotify starts. When Spotify's state shows the track finished, the
engine loads and plays the next file. The Player itself is unchanged, so
everything that shows or controls the Player (the deck, the big view, the
mini players, MPRIS and the bar plugin) follows Spotify for free.

**Careful with spotify_player.** Three of its habits shape the code:

- When no instance is running, its command line starts a client of its own
  to answer, and that client may stop to ask for a sign-in. Omamp therefore
  runs a command only when spotify_player is signed in and running, and
  closes the command's input so nothing can wait for a reply.
- With an instance running, `playback start` returns at once and reports a
  failure (such as "Premium required") only in its log. Omamp counts a start
  as failed when Spotify's state hasn't shown the track playing after twelve
  seconds, and then says why it probably failed: a Free account, or no
  Spotify Connect device on this computer.
- It prints its JSON after turning every `\n` escape into a real line break.
  Omamp puts the escapes back before reading it.

**Pictures.** spotify_player's lists carry no pictures. The playing track's
cover comes from Spotify's playback state; covers for list rows come from
Spotify's public oEmbed page (`open.spotify.com/oembed`), which needs no
sign-in, as each row is shown. All of them are kept in
`~/.cache/omamp/spotify`. A row without a picture shows an
icon.

**Settings and files.** The switches live in `~/.config/omamp/spotify.conf`:
whether Spotify is on, the backend, the two kinds of blending and whether to
start spotify_player. Nothing about the account is stored.

## Testing

`tests/spotify_tests.cpp` runs against a fake `spotify_player` and a fake
`spotifast` (`tests/fake-spotify`) put first on `PATH`. They answer from
recorded JSON in `tests/fixtures/spotify`, made by `make-fixtures.py` from the
structs in spotify_player 0.25.1's source and rspotify-model 0.16.1, with
made-up artists and albums. The tests never reach Spotify, never start the
real programs and play no sound.

## Things to check by hand

None of this has met a real Spotify account. After signing in, check:

1. `omamp spotify status` shows your account, `Premium: yes` and the
   `spotify-player` device.
2. Play a playlist mixed with local files and listen to the change-overs.
3. Pause on your phone; Omamp should show paused within a second or two.
