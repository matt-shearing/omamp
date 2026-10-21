# Web metadata

Thrum can fill in tags, covers and lyrics from free web services, and send
what you play to ListenBrainz or Last.fm. Nothing goes online until you run
one of the commands below or press the matching button in the window
(Identify with MusicBrainz, Find missing covers, Find a cover, Fetch from
LRCLIB, the scrobbling log-ins and Send now). This page lists each service, what it needs, how fast Thrum may ask
it, and exactly what leaves your machine.

## Services

| Service | What Thrum uses it for | Key needed | Pace |
|---|---|---|---|
| MusicBrainz | Release and recording look-ups and searches for `thrum identify`; release searches for `thrum fetch-art` | No | One request a second |
| AcoustID | Turning an audio fingerprint into MusicBrainz recordings | An application key | Three requests a second |
| Cover Art Archive | Front covers by release or release group | No | Two requests a second |
| LRCLIB | Plain and synced (LRC) lyrics | No | Two requests a second |
| ListenBrainz | Scrobbling | Your user token | One a second, or slower when its `X-RateLimit` headers say so |
| Last.fm | Scrobbling | An API key and secret, then a session | Five a second |

Every request carries the User-Agent
`Thrum/<version> ( https://github.com/matt-shearing/thrum )`, which
MusicBrainz and ListenBrainz require. The pace is shared by every thread in
Thrum, so two jobs running at once still keep to it. When a service answers
503, 429, 502 or 504, Thrum waits (as long as a `Retry-After` header says, or
one, two, then four seconds) and tries up to three more times.

## Keys and log-ins

### AcoustID

AcoustID gives an application key free for non-commercial use. Thrum does not
have one registered yet. Without a key, `thrum identify` says so and matches
by the tags and any MusicBrainz IDs already in the files.

1. Register an application at https://acoustid.org/new-application.
2. Run `thrum net set acoustid.key <key>`, or set `THRUM_ACOUSTID_KEY` in the
   environment. The environment wins.

Fingerprints need `fpcalc`, which comes with the `chromaprint` package.
Thrum only looks fingerprints up; it never submits them.

### ListenBrainz

1. Copy your user token from https://listenbrainz.org/settings/.
2. Run `thrum scrobble login listenbrainz --token <token>`. Thrum checks the
   token with ListenBrainz before it saves it.

`thrum scrobble logout listenbrainz` forgets the token. Listens still waiting
stay in the queue.

### Last.fm

Last.fm needs an API account of your own.

1. Create one at https://www.last.fm/api/account/create.
2. Run `thrum net set lastfm.api_key <key>` and
   `thrum net set lastfm.api_secret <secret>`.
3. Run `thrum scrobble login lastfm`. It prints a Last.fm page; open it and
   allow Thrum.
4. Run `thrum scrobble login lastfm --complete`. Thrum swaps the approved
   token for a session key and saves it.

## Commands

All of them take `--json` and `--db <path>`.

```text
thrum identify <query> [--dry-run | --apply] [--release <mbid>] [--min-score 0.8]
               [--no-acoustid] [--no-search] [--no-art] [--cover-file] [--genres] [--size 500]
thrum fetch-art <query> | --missing [--embed] [--save-cover] [--size 500] [--no-search] [--dry-run]
thrum lyrics <query> [--fetch] [--refresh] [--show] [--write lrc|tag] [--synced-in-tag] [--dry-run]
thrum scrobble status | flush | login listenbrainz --token <t> | login lastfm [--complete]
               | logout listenbrainz|lastfm
thrum net status | set <key> <value> | unset <key> | clear-cache
```

### Identify

![Identify with MusicBrainz: releases, scores and the tag changes](identify.png)

`thrum identify` works the way Picard does.

1. **Cluster.** The tracks the query finds are grouped into would-be albums
   by their album and album artist tags and their folder. A `CD1` or `Disc 2`
   folder joins its siblings.
2. **Look up.** For each group Thrum collects candidate releases from the
   MusicBrainz release ID most of its files already carry, from AcoustID
   (when there is a key and `fpcalc`), and from a MusicBrainz search by album,
   artist and track count. It looks up to five candidates up in full.
3. **Score.** Each candidate gets a score from 0 to 1. The tracks count most:
   every file is paired with the release track whose title, length, artist
   and position fit it best, and a matching fingerprint or recording ID beats
   any tag. Then come the track count, the album title, the artist, the date,
   and your preferred countries and formats.
4. **Review.** Without `--apply`, Thrum prints the three best candidates and
   every tag the best one would change. Nothing is written.
5. **Save.** With `--apply`, each group whose best candidate scores at least
   `--min-score` (0.80 unless you say otherwise) is written through the tag
   writer, so it is journalled and `thrum undo <batch>` reverses it. A group
   below the line is left alone and named. To choose a release yourself, run
   `--release <mbid> --apply` on a query that finds one album.

Identify writes the title, artists, album, album artist, year, original year,
track and disc numbers with their totals, the compilation flag for Various
Artists releases, the ISRC, the label and catalogue number, the release
country, and the MusicBrainz recording, release track, release, artist, album
artist and release group IDs. It writes MusicBrainz's genres only when asked.
It then adds the release's front cover from the Cover Art Archive to the
files, and to the album in Thrum's artwork cache.

### Covers

`thrum fetch-art --missing` works through every album the "Missing artwork"
audit lists. For each album Thrum uses the release ID in its tracks, then the
release group ID, then a MusicBrainz search by album and artist that must
match the title closely. The original picture is saved under
`~/.cache/thrum/art/fetched/` and becomes the album's picture. `--embed`
writes it into every track (journalled), and `--save-cover` saves `cover.jpg`
beside the tracks when the folder has no picture file.

### Lyrics

![Timed lyrics in the info panel](lyrics.png)

`thrum lyrics <query>` shows the lyrics Thrum already has for each track, in
this order:

1. a `.lrc` file beside the track, with the same name;
2. the tags: an ID3v2 `SYLT` frame (synced), then `USLT` or `LYRICS`, which
   counts as synced when it holds LRC text;
3. an earlier LRCLIB answer, kept in the library's `lyrics` table.

`--fetch` asks LRCLIB when none of those has any. Thrum asks by artist, title,
album and length first, then searches and keeps the closest match within five
seconds. When LRCLIB has nothing, Thrum remembers that for 30 days.

`--write lrc` writes the synced lyrics to a `.lrc` file beside the track.
`--write tag` writes the plain lyrics into the file (`--synced-in-tag` writes
the LRC text instead). Both go through the tag writer, so `thrum undo`
reverses them, removing a `.lrc` it created.

### Scrobbling

A play counts once you have heard half the track or four minutes of it,
whichever comes first, and only for tracks of 30 seconds or more. Counted
plays go into the library's `scrobble_queue` table at once, one row per
service, so plays made offline are never lost. `thrum scrobble flush` sends
them in batches (100 to ListenBrainz, 50 to Last.fm). A listen a service
rejects as invalid is set aside as failed; anything else stays queued for the
next flush. `thrum scrobble status` shows what is waiting, sent and failed.

## Settings

Set these with `thrum net set <key> <value>`; `thrum net unset <key>` clears
one. Secrets are never printed back.

| Setting | Default | Meaning |
|---|---|---|
| `acoustid.key` | none | The AcoustID application key |
| `identify.countries` | none | Preferred release countries, best first: `GB,XW,US` |
| `identify.formats` | none | Preferred formats, best first: `CD,Digital Media` |
| `identify.write_genres` | `0` | Write MusicBrainz's three most-voted genres |
| `identify.write_label` | `1` | Write the label and catalogue number |
| `identify.cover_embed` | `1` | Put the front cover into the files |
| `identify.cover_file` | `0` | Also save `cover.jpg` when the folder has no picture |
| `identify.cover_size` | `500` | Cover size: 250, 500, 1200, or 0 for the original |
| `coverart.size` | `500` | The same, for `thrum fetch-art` |
| `scrobble.enabled` | `1` | `0` stops all scrobbling |
| `scrobble.listenbrainz`, `scrobble.lastfm` | `1` | `0` stops one service |
| `net.offline` | `0` | `1` keeps Thrum off the network; cached answers still work |
| `net.url.<service>` | the public address | Another server, such as a MusicBrainz mirror |

The environment overrides some of these: `THRUM_ACOUSTID_KEY`,
`THRUM_LISTENBRAINZ_TOKEN`, `THRUM_LASTFM_API_KEY`, `THRUM_LASTFM_API_SECRET`,
`THRUM_OFFLINE=1`, and `THRUM_<SERVICE>_URL` (for example
`THRUM_MUSICBRAINZ_URL`). `THRUM_RATE_<SERVICE>_MS`, `THRUM_NET_RETRIES` and
`THRUM_NET_BACKOFF_MS` change the pace and retries; leave them alone unless
you run your own mirror.

## Privacy

This is everything that leaves your machine, and only when you run the
command that sends it.

- **MusicBrainz** receives the album, artist and title text being searched,
  the number of tracks, and the IDs of the releases and recordings being
  looked up.
- **AcoustID** receives each file's Chromaprint fingerprint and length in
  seconds, with the application key. Not the audio, not the file name.
- **Cover Art Archive** receives release and release group IDs.
- **LRCLIB** receives the artist, title, album and length of the track.
- **ListenBrainz** receives your token and, for each counted play, the
  artist, title, album, length, track number, MusicBrainz IDs and the time
  you started it. "Now playing" sends the same without the time.
- **Last.fm** receives your API key, session key, a signature, and the same
  details per play.

Every request also carries Thrum's User-Agent and, as with any web request,
your IP address. File paths, ratings, play counts and the rest of your
library never leave the machine.

Tokens and keys are stored in the library's `settings` table in plain text,
so anyone who can read `~/.local/share/thrum/library.db` (or its backups) can
read them. Answers are cached under `~/.cache/thrum/http`;
`thrum net clear-cache` empties it.

## Terms

AcoustID is free for non-commercial use only. MusicBrainz data is CC0.
Last.fm restricts caching of its data; Thrum caches none of it. Each service
can change its terms; check them before relying on Thrum in a commercial
setting.
