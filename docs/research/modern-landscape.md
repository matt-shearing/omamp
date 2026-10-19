# Omamp advanced mode: research report (2026-10-19)

**How this was sourced.** The search tool returned thin results. I checked these against the pages: the MusicBrainz and AcoustID limits, ListenBrainz auth and rate limits, the Qt 6 `QMediaPlayer` API, the OpenSubsonic API surface, and the NFS/inotify behaviour. Everything else is my own knowledge, marked **(unverified)**. Check those items against the live docs before building on them.

## 1. Features worth stealing

| Tool | Feature to copy | Why it matters for Omamp |
|---|---|---|
| **beets** | Plugin pipeline (`fetchart`, `lyrics`, `replaygain`, `chroma`, `duplicates`, `missing`, `smartplaylist`, `mbsync`). Import is a staged, interruptible flow with per-album match confidence. | Model "import" as a candidate-match-then-confirm step. `duplicates` and `missing` should be built-in reports. `smartplaylist` writes M3Us, which is useful for NAS clients. |
| **Picard** | Cluster, then lookup, then save. AcoustID matching. A file-naming script language. | The cluster/lookup workflow with per-file confidence colours is the best UX in the field. Steal the "unmatched / clusters / albums" three-pane idea. |
| **foobar2000** | Title-formatting language (`%artist% - $if(...)`), facet/album-list views, Masstagger scripts. | One small expression language can drive columns, file renaming, tag edits and grouping. This is the closest match to MediaMonkey's "Auto-Organize" and Library Filters. |
| **Quod Libet / Ex Falso** | Regex-capable search with a typed query syntax (`artist=/foo/ & #(rating > 3)`), plus a tag editor that works without the player. | A typed query language doubles as smart-playlist definitions. |
| **Strawberry** | Smart playlists, a collection-organising dialog, a transcoder, scrobbling, Subsonic and Tidal sources, and a GStreamer engine with gapless and crossfade. | It is the nearest existing codebase to what you describe, so read its source. |
| **Elisa** | Clean Qt Quick UI, Baloo/KFileMetaData indexing, MPRIS. | A reference for a Qt Quick library UI. |
| **Navidrome** | Single Go binary, SQLite, multi-user, smart playlists in JSON, transcoding, Subsonic/OpenSubsonic API, periodic and watch-based scans. | The natural NAS backend (see section 4). |
| **Jellyfin** | Server-side library with metadata providers; a client-per-device model. | Mostly useful as an architecture reference. |
| **Plex / Plexamp** | **Sonic analysis** (audio-feature similarity), "Sonic Sage"-style and radio modes, sweet-fade crossfade, loudness levelling. | Local sonic analysis is the standout. Offline equivalents are Essentia or similar descriptors with nearest-neighbour search. This is hard but a real differentiator **(unverified for current Plexamp internals)**. |
| **Roon** | Rich metadata: credits, composers, work/movement hierarchy, linked artist pages. | MusicBrainz relationships (work, performer, label) can fill the same fields. Treat as a stretch goal. |
| **Swinsian** | Fast tag editing across huge libraries, duplicate finder, folder watching. | The UX bar for a native-feeling library. |
| **MusicBee** | Auto-tagger, auto-DJ, a rules engine for auto-playlists, scrobbling, a very customisable column and tag model. | The closest to MediaMonkey in spirit. |

**Core design takeaways:**
- A single expression language shared across columns, queries, renaming and tag rules.
- An import/identify workflow with confidence scores and a preview before writing.
- Reports for duplicates, missing tracks and suspect tags.
- A stable plugin or provider boundary for metadata sources.

## 2. Metadata services and terms

| Service | Key needed? | Limits and terms |
|---|---|---|
| **MusicBrainz** | No key. | Stay at one call per second or less, and set a proper `User-Agent` (app name, version, contact). IP blocking is the penalty. [Rate limiting](https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting) and [API](https://wiki.musicbrainz.org/ws/2). Third-party sources mention looser per-User-Agent allowances, but design for 1 req/s. Core data is CC0. For bulk work, use the database dumps or a local mirror instead of the API. |
| **AcoustID + Chromaprint** | Yes, an application key (the `client` parameter). Submissions also need a per-user key. | Max 3 requests/s. **Free for non-commercial use only**; commercial use needs a separate sign-up via acoustid.biz. Lookup `meta` values include `recordings`, `releasegroups`, `releases`, `tracks`. [Web service](https://acoustid.org/webservice). Omamp should ship an app key if it is open source and non-commercial. |
| **Cover Art Archive** | No key. | Served by MusicBrainz and Internet Archive. Look up by release MBID or release-group MBID, e.g. `coverartarchive.org/release/<mbid>/front-500`. Keep a polite User-Agent and cache locally **(unverified: no hard published limit)**. |
| **Discogs** | A personal token or OAuth for most calls. | A third-party client doc reports roughly 60 requests/min authenticated and 25 unauthenticated, with 429 on excess **(secondhand)**. Requires a User-Agent. Images need authentication, and terms restrict bulk storage, so check the [Discogs developer terms](https://www.discogs.com/developers) before caching. |
| **LRCLIB** | No key, no account. | Open synced-lyrics database. Endpoints `/api/get` (by artist, title, album, duration), `/api/search` and `/api/publish`. Returns both `plainLyrics` and `syncedLyrics` (LRC). Asks for a User-Agent. [Docs](https://lrclib.net/docs) (page text did not load for me, so the details are **unverified**). |
| **ListenBrainz** | Per-user token in the `Authorization: Token ...` header. No app key. | Keep to one call per second; responses carry `X-RateLimit-*` headers; 429 on excess; HTTPS only; User-Agent should be `App/<version> (contact)` or requests may be blocked. [API docs](https://listenbrainz.readthedocs.io/en/latest/users/api/index.html). Free and open. It also offers recommendations, stats and a Last.fm importer. |
| **Last.fm** | Yes: an API key and shared secret, with an md5-signed session for scrobbling. | Free for non-commercial use. Terms are restrictive about caching. I would implement ListenBrainz first, and Last.fm second as an optional provider **(unverified current terms)**. |
| **fanart.tv** | Yes. A project key is required, and a personal client key gives faster access to new images. | Old forum posts say images approved in the last 7 days are withheld without a personal key, and 48 hours with one **(dated, 2014–2018)**. Use it only for artist backgrounds and logos, not as a primary source. Check its API page for current terms. |

## 3. Libraries and Arch packages

| Need | Library | Arch package | Notes |
|---|---|---|---|
| Tags | **TagLib 2.x** | `taglib` | Reads and writes MP3 (ID3v2), FLAC, Ogg Vorbis/Opus, MP4/M4A, APE, WavPack, ASF, WAV/AIFF, plus DSF/DFF (DSD) and Matroska in 2.x. Cover art: `ID3v2::AttachedPictureFrame`, `FLAC::Picture`, MP4 `covr`. 2.0 moved to C++17, changed some APIs and added a unified `PropertyMap`/complex-property interface. Not thread-safe per file object, so use one object per file per worker. |
| Fingerprinting | **Chromaprint** | `chromaprint` | `fpcalc` CLI or `libchromaprint`. Needs decoded PCM, so decode the first ~2 minutes with FFmpeg. |
| Loudness | **libebur128** | `libebur128` | Implements EBU R128 and ReplayGain 2 (−18 LUFS reference), including true peak. |
| Database | **SQLite with FTS5** | `sqlite` | FTS5 is compiled into the Arch package. Use an external-content FTS5 table over the tracks table, with the trigram tokeniser for fuzzy substring search. Use WAL mode. |
| Desktop metadata | KFileMetaData | `kfilemetadata` | Pulls in KDE Frameworks. It is unnecessary since TagLib covers it, and not worth the dependency here. |
| Watching | **inotify** (`inotify_add_watch`, or Qt `QFileSystemWatcher`) | built-in | Local disks only (section 4). fanotify needs privileges, so skip it. The default `max_user_watches` is high on modern kernels, but a 100k-file tree has many directories, so watch directories only. |
| SMB access | libsmbclient | `smbclient` | Rarely worth it. Mounting via the kernel (cifs-utils) gives plain-file semantics. |
| Network FS | KIO | `kio` | Adds a heavy KDE dependency. Prefer system mounts, which also work with TagLib and FFmpeg unmodified. |
| Decode | FFmpeg | `ffmpeg` | For decoding, probing and transcoding. |
| Waveform | FFmpeg decode, then peak-reduce | n/a | Generate in a background job and cache. |

**Gapless and crossfade in Qt Multimedia.** I checked the [Qt 6 `QMediaPlayer` docs](https://doc.qt.io/qt-6/qmediaplayer.html).
- There is no gapless or preload API in Qt 6. `setSource()` replaces the current media and stops playback.
- The Qt 5 `QMediaGaplessPlaybackControl` (next-media and crossfade-time) was a backend-dependent hook. I could not confirm it survived into Qt 6, and the Qt 6 docs show no crossfade.
- There is also no queue in `QMediaPlayer`. Qt 6.8 added `audioBufferOutput`, which emits decoded audio buffers, and 6.10 added pitch compensation.
- The practical path is to build a custom engine. Use two decoders (FFmpeg or `QMediaPlayer` with `QAudioBufferOutput`) feeding one `QAudioSink`. That gives true gapless playback, crossfade, ReplayGain gain and a waveform tap, all under your control. It is more work, but it is the only path where gapless is guaranteed. The alternative is two `QMediaPlayer` instances, with the second pre-loaded and started before the first finishes. That is simpler but risks an audible gap, especially with MP3 encoder padding.
- Strawberry (and Clementine before it) uses **GStreamer** directly, not Qt Multimedia, with its own crossfade and gapless logic. That is the reference design if you go GStreamer, and the `strawberry` and `gstreamer` packages are in the Arch repos. The trade-off is a larger dependency and plugin matrix; you gain proven gapless and ReplayGain support. Strawberry's engine design is from my own knowledge **(unverified against current source)**.
- Gapless correctness also depends on reading encoder delay/padding (LAME/Xing header or iTunSMPB) so you can trim samples.
- My recommendation: a custom FFmpeg-based engine, with the output device through Qt's `QAudioSink` (PipeWire-compatible).

## 4. NAS specifics

**Mounts.**
- **NFS** is simpler and faster for POSIX metadata, and has better small-file performance. Use NFSv4 with `noatime`.
- **SMB/CIFS** is more universal. Mount with `cifs-utils`, using `vers=3.x`, `uid`/`gid` and `file_mode` options, `noserverino` if inode numbers are unstable, and `actimeo` tuned for cache behaviour. systemd automount (`x-systemd.automount`, `x-systemd.idle-timeout`, `_netdev`, `nofail`) lets the mount come and go cleanly.

**Why inotify misses remote changes.** inotify is a kernel feature: it only sees changes that pass through the local kernel's VFS.
- On NFS, changes made by another client or the server never reach the local kernel's notification path. A KDE bug report states this directly: inotify does not detect NFS changes unless they originate from the watching system. [KDE bug 177892](https://bugs.kde.org/show_bug.cgi?id=177892).
- On SMB, the evidence is mixed. The kernel client can use server change-notify for some cases, and some people report it works. Do not rely on it.
- The Plex community's workaround is the standard one: scheduled partial rescans. [Plex forum thread](https://forums.plex.tv/t/plex-linux-nfs-inotify/527431).

**What to do instead:**
1. **Periodic rescan by fingerprint.** Compare `(size, mtime_ns, path)` against the database, and skip tag reads when they match. Walk only directories whose mtime changed; directory mtime is a cheap change signal on most NAS filesystems, though not guaranteed on all of them.
2. **Run the scanner on the NAS** (or a broker service there), where the local kernel does see changes. Navidrome does exactly this.
3. **Use the OpenSubsonic API against Navidrome on the NAS**, so Omamp never scans over the network at all. It exposes `getIndexes`, `getAlbumList2`, `search3`, `stream`, `getCoverArt`, `getLyricsBySongId`, `scrobble`, plus extensions for API-key auth, sonic similarity, transcoding and playback reports. [OpenSubsonic docs](https://opensubsonic.netlify.app/docs/opensubsonic-api/). This is the cleanest long-term route, and it also gives you phones for free.
4. **UPnP/DLNA** is a viable fallback but gives poor metadata, no write access and weak gapless playback. I would not prioritise it.

**Volumes that come and go.**
- Give every library root a stable volume ID: the filesystem UUID for local disks, and for network shares a `(server, share, export-path)` tuple plus a marker file (e.g. `.omamp-volume-id`) written once at the root.
- Store track paths relative to the volume root. Mark tracks `offline` when the volume is absent instead of deleting them; never "clean missing files" while any volume is unmounted. This is the main MediaMonkey pain point to fix.

**100k+ tracks over the network.**
- First scan: parallelise tag reads (4–8 workers is enough; the network round-trip is the limit), read only the headers/tags, and commit in batches inside one transaction.
- Rescans: only `stat` the files, using the size+mtime fingerprint. Over SMB this is roughly one metadata round-trip per file, and much cheaper than reading content.
- Never scan on startup in the foreground. Show the existing database immediately and refresh in the background.
- **Cache locally:** artwork thumbnails (resized to a few sizes), waveform peaks and fingerprints, all keyed by volume ID, relative path and fingerprint. The database itself lives on local disk, never on the NAS (SQLite over network filesystems risks locking and corruption).

## 5. Creature comforts for a 2026 desktop player

| Feature | Practical notes |
|---|---|
| Synced lyrics | LRCLIB (no key), plus embedded `SYLT`/`USLT` and sidecar `.lrc` files. Save fetched lyrics into the DB cache rather than the audio file unless the user opts in. |
| Waveform seekbar | Precompute peaks in a background job; draw with a `QQuickPaintedItem` or `Shape`. |
| ReplayGain | Read existing tags first (libebur128 only for missing ones). Support track and album modes, and clip prevention. |
| Gapless / crossfade | See section 3. Decide on the engine early, because it shapes everything else. |
| Smart playlists | A typed query language (like Quod Libet's) with live results; export to M3U for NAS clients. |
| Fuzzy command palette | Ctrl+K style, with trigram FTS5 over tracks plus actions. |
| MPRIS | `QDBusAbstractAdaptor` for `org.mpris.MediaPlayer2`. Straightforward on Wayland. It drives Hyprland media keys, playerctl and bar widgets. |
| ListenBrainz scrobbling | Easy: a token plus a queue that retries offline. Submit "playing now", then a "single" listen after 50% or 4 minutes. |
| Discord / now-playing | Discord Rich Presence needs a local IPC socket; low risk. Treat as optional. |
| Keyboard-first | Every action reachable from the palette; vim-style navigation as an option. |
| Undo for tag edits | Journal every write (old value, new value, file, time) in SQLite; undo replays the old values. Store a backup of the original tags before the first edit. |
| Dry-run for file moves | A preview table (old path, new path, conflicts) before applying; apply as a transaction with a rollback log. Moves across volumes are copy-then-delete, so verify before deleting. |
| Background jobs | A job queue with progress, cancel and pause. Surface it in one panel and as a notification, not as modal dialogs. |
| Headless CLI for agents | `omamp-cli` with JSON output (`query`, `scan`, `identify --dry-run`, `apply`). Same core library as the GUI. Define it early, since it forces a clean separation between core and UI. |
| DSD / hi-res | TagLib reads DSF/DFF tags, and FFmpeg decodes DSD to PCM. Native DSD output over PipeWire is generally not available, so plan on DoP or PCM conversion **(unverified)**. |
| Multi-room | Snapcast is simple to feed: write PCM to a pipe or TCP source it reads. A side feature for later. |
| Mobile sync | Syncthing for file-level copies (keep `.git` and the database out of it). MTP is awkward on Linux (`gvfs-mtp`/`jmtpfs`), so prefer Syncthing or a Subsonic client against Navidrome. |

## Risky or hard on Wayland/Linux

- **Global shortcuts and media keys:** Qt apps can't grab global keys on Wayland. Rely on MPRIS (playerctl) and Hyprland's own bindings; the GlobalShortcuts portal is not guaranteed on Hyprland.
- **System tray:** needs StatusNotifierItem and works with Omarchy's bar, but is not guaranteed everywhere.
- **Always-on-top mini-player:** windows can't position themselves on Wayland, so use Hyprland window rules (`float`, `pin`).
- **Drag-and-drop out of the app:** drop-in from file managers works; drag-out of files to other apps is patchy.
- **Qt Multimedia FFmpeg backend:** behaviour with network streams and odd codecs varies between Qt 6.x releases. Pin a tested Qt version and test CUE and APE files.
- **SQLite on a network mount:** avoid it; locking is unreliable.
- **Tag writing on SMB:** partial writes during a network drop can corrupt files. Write to a temp file and rename, or retry with verification, and keep the undo journal.
- **Licensing:** AcoustID is non-commercial only. Discogs and Last.fm have caching restrictions. Read the terms before shipping a built-in key.

## Suggested build order

1. Library core: SQLite schema with volume IDs, relative paths and the scan fingerprint.
2. A custom gapless engine with ReplayGain.
3. The query language and smart playlists.
4. Identify via Chromaprint, AcoustID and MusicBrainz, with a preview step and undo journal.
5. Rename/organise with a dry run.
6. ListenBrainz and LRCLIB.
7. A Subsonic client for the NAS.

## Sources

- https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting
- https://wiki.musicbrainz.org/ws/2
- https://acoustid.org/webservice
- https://listenbrainz.readthedocs.io/en/latest/users/api/index.html
- https://doc.qt.io/qt-6/qmediaplayer.html
- https://doc.qt.io/qt-5.14/qmediagaplessplaybackcontrol.html
- https://opensubsonic.netlify.app/docs/opensubsonic-api/
- https://bugs.kde.org/show_bug.cgi?id=177892
- https://forums.plex.tv/t/plex-linux-nfs-inotify/527431
- https://lrclib.net/docs (page content did not load; details unverified)
