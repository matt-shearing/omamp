# MediaMonkey feature inventory for a Linux "advanced mode" library manager

Most feature claims are fetched from the MediaMonkey wiki, which is cited below. Items marked **(unverified)** come from my background knowledge. They were either not confirmed or contradicted by the pages I could fetch.

## 0. Version map

- **Songs-DB (2001–02) → MM2 (2003–07).** Winamp plugins, scripting (since Songs-DB 1.2), iPod sync.
- **MM3 (2007–08).** Rebuilt around large libraries: 3.0.3 raised the supported size to more than 100k tracks. It introduced the "Filter" idea that became Collections.
- **MM4 (2011–2020s, still maintained).** This is the classic. Features:
  - Collections, video support, UPnP/DLNA sharing and client, and tabs.
  - A WASAPI audio engine and a portable mode that runs from a USB drive.
  - Secure ripping with AccurateRip, device profiles, integrated file monitoring, and Android sync (Wi-Fi arrived in 4.1).
  - VBScript/JScript scripting.
  - The core is Delphi/native Win32 with a SQLite database.
- **MM5 (public beta from Dec 2016, stable 2018+).** A rewrite with a Chromium/HTML/JS UI. Features:
  - Skins and addons written in JavaScript.
  - Per-node saved Views, Pinned items, and info panels.
  - Audio-fingerprint auto-tag (MusicBrainz-based).
  - Cloud storage (Dropbox, Google Drive, OneDrive), Deezer/Spotify sync, and Chromecast.
  - DSD playback, touch mode, and automatic DB backup.
  - Five extra custom fields and "extended tags", the latter editable in the Properties dialog only.
- **"2024" release.** Version 2024.0.0.3082 (Dec 2024) is the current stable Windows build. It is the same MM5 line under a year-based name, and I found no feature list that distinguishes it.
- **MediaMonkey Server (2018 alpha).** A server-only build with a browser UI, meant to scale better than UPnP.

## 1. Library database

**Engine and schema (MM4 documented, MM5 same lineage)**
- SQLite 3, UTF-16LE, with a custom case- and accent-insensitive collation (`IUNICODE`) and `NUMERICSTRING` for track and disc numbers.
- Core tables:
  - `Songs` (denormalized: artist text lives in the row for fast queries), `Albums`, `Artists`, `Genres`.
  - Many-to-many tables: `ArtistsSongs`, `ArtistsAlbums`, `GenresSongs`, `ListsSongs` (classifications), `PlaylistSongs`.
  - `Playlists` (static and auto), `Podcasts`/`PodcastEpisodes`, `Filters` (saved views), `Pinned`, `Radio` (MM5).
  - `Medias` (the sources the library has seen), `Folders`/`FoldersHier`, `Played` (play history), `OrganizeRules`, `Covers`, `Devices`/`DeviceTracks`/`Synch*`, `URLRequestCache`.
- Triggers keep counters in step, for example per-artist and per-genre track counts.
- The DB stores tags as well as the file: the help says changing an artist updates the library and, if auto tag-update is on, the file's tags. Multi-value Artist, Album Artist, Composer and Genre fields are separated by `;`, and a track appears under several tree nodes.
- Track properties include:
  - Standard tags, plus classifications (tempo, mood, occasion, quality).
  - Rating, play count, last played, date added, bitrate, BPM, original date, and 5 custom fields (MM4 had a couple of custom fields; MM5 added 5 more).
  - Content Type (see section 2), and volume-leveling coefficients per track and per album.
- Ratings and play data can be imported from Windows Media Player, iTunes and Winamp during the first scan. Ratings are stored in file tags or the DB, depending on settings, with POPM/Rating-tag mapping.
- **Field Maps** reference page: per-format mapping of properties to tag frames, for example custom fields to TXXX/Vorbis comments.

**Getting files in (Add/Rescan, Insert key)**
- Add locations: local folder, network folder, media server (UPnP), or cloud. Each location has a Type setting (or Auto-detect) that sets which Collections it feeds. Everything also appears under "Entire Library".
- Scan options:
  - File-type filters, and skip files under a minimum size or duration.
  - "Only for files with changed timestamp/size", so re-scans are cheap.
  - Analyze for duplicates, "Remove unavailable items", "Remove unused attributes" (purge orphaned artists/genres), "Infer properties" (guess tags from path), and "Scan folders for artwork".
  - "Create playlists of tracks newly added" (Recently Added).
  - Also imports `.m3u` files to Playlists > Imported Playlists, and can catalogue CDs without ripping.
- **Folder monitor.** Each location has a Schedule: Continuously, At startup, both, or Manual.
  - Monitoring adds new files, removes deleted ones, and detects moves, but only inside monitored folders.
  - Moves into unmonitored folders show up as dead links.
- **Removable and network drives.**
  - Drives are identified by a drive ID, not the letter, so a stick that remounts on a different letter still works. The wiki never mentions a "Media ID"; that is MM4-era terminology I could not confirm.
  - The ID changes after a PC swap, a Windows reinstall or a drive swap. The tracks then land under a "Disconnected" node, and the fix is to right-click the drive and re-assign it in its media properties. This only works for lettered drives with identical paths, not UNC paths.
  - Network guidance: use UNC paths, not mapped drives. Choose the "Network" type (editable, syncable) over "Media Server" (stream only).
  - A forum reply from an older version says real-time monitoring on network shares only catches changes made from the same PC; otherwise rely on startup scans or manual rescans.
- **Dead Links vs Disconnected.** Dead Links covers files missing on an *accessible* drive. Files on an unplugged drive are shown as Disconnected, not Dead, which avoids purging offline removable media.
- **Locate Moved/Missing Files (Gold).** Searches a chosen location and matches by size and timestamp, or by length/artist/album/title. It reports three tabs: located, unmoved and unlocatable, then updates paths or removes entries.
- **Folders / Pinned / Devices & Services nodes (MM5).** "Folders" is a file-explorer view that shows *all* media, including files not in the library. It can edit tags, browse a CD without cataloguing, and make `.m3u` files.
- **Moving the library to a new machine.** There is a documented procedure ("Moving MM to a new computer").

**Tree, Collections and Types**
- **Library tree nodes (MM4):**
  - Location (Hard Disk, CD, Network, Media Servers, each with an "All" node), Title, Artist, Album Artist, Composer, Album.
  - Genre, Year (decade and year sub-nodes), Rating, Classification, Virtual CD (Gold), Previews.
  - Files to Edit (section 3).
- **Collections** (MM4 and MM5; custom ones are Gold): a Collection is a saved search criterion, for example `Type=Classical` or `Type=Music AND Genre=Children;Kids`. Each Collection owns:
  - the columns and their order,
  - which tree nodes are shown and in what order,
  - the sort order, and
  - the active view (Details or Art Browser).
- A Collection can be shown, hidden, or shown only when non-empty. Playlists inherit the settings of their members' Collections. "Entire Library" has its own generic settings.
- **Content Types** are fixed: Music, Podcast, Audiobook, Classical Music, Music Video, Video, TV, Video Podcast. A Type controls:
  - Playback rules (bookmarking, ignoring Shuffle, disabling crossfade, Auto-DJ eligibility).
  - Which fields show in Properties.
  - Sync categories on a device.
  - The Type is auto-assigned on first scan from genre keywords and extension (M4B means Audiobook).
- **MM5 Views:** each node saves its own View (type, columns, sort, elements). Node types are Details, Art Browser (cover grid), and Tracklist/Album views. There are also full-screen "playing" modes, panel toggling, pinned items, and info panels.

## 2. Views and search

- **Now Playing / Playing list.** A queue that supports Shuffle, Continuous, auto-remove of played files (to keep it small), and a warning or silent skip for inaccessible files. MM5 adds "Shuffle by album" and "Stop after every file".
- **Quick search bar.** Scope is the Collection, the displayed list, or the entire library. The syntax:
  - `A B` means AND, and `A OR B` / `A ; B` means OR.
  - `A -B` means NOT.
  - `"A"` is an exact case-sensitive match, and `+A` matches the start of a word.
  - `Artist:A` limits the match to one field.
  - `Year:1978..Present` is a range, and `Rating: 4..` means that rating or higher.
  - OR binds tighter than AND.
- **Advanced Search** has three layers:
  - Criteria whose attributes OR together (Tempo is Fast OR Very Fast).
  - An All/Any combiner between criteria.
  - Multi-level sorts, including Random and Random Album, plus limits by count, size or time. Limit combined with Random re-rolls on each use.
  - Results can be saved as an AutoPlaylist.

## 3. Tagging and cleanup

- **Properties dialog (multi-select edit).** Per-field tri-state for batch edits.
  - Tabs: Details, Artwork, Lyrics (embedded and editable), Classification, Comments, Custom, and Playback-related settings (start/stop time, volume offset, bookmark).
  - MM5 adds a quick tag edit in the Preview window.
- **Album properties (album-level edit).** Edit shared fields once, with track-number and disc handling.
- **Auto-Tag from Web.**
  - MM3/MM4: Amazon by default. Freedb for audio CDs. Addons add other sources.
  - MM4 had a Discogs tagger addon; it is not available in MM5.
  - MM5 uses an audio fingerprint plus MusicBrainz. Users dislike it (section 11).
  - Workflow: match the track numbers, tick the properties to apply, then apply. The wiki warns there is no undo.
- **Auto-Tag from Filename.** Reverse-applies a mask: `<Skip:x>` ignores characters, and `<Folder:n>` / `<Path:n>` pick up path parts.
- **Update Tags / Synchronize Tags.** Pushes DB values into files, or the reverse. The "Unsynchronized Tags" node lists mismatches.
- **Clean Tags.** Normalizes case, trims, and similar.
- **Auto-Organize (rename and move by mask).**
  - Manual mode (Tools > Auto-Organize Files): Move or Copy (copy can add to the library), a destination mask, and a "New Path" preview column that is editable per file (F2) or per exclusion. Duplicate target paths get "(n)" appended. Accompanying files (art, lyrics) can move too. Emptied folders can be deleted. There is no undo.
  - Background mode (Gold): rules per library subset or Collection, run on add/edit. The "Unorganized Files" node feeds it. Enabling a rule does not touch existing files. There is a "trim path to <256 chars" option.
  - **Mask language** (a design goldmine):
    - Tags: `<Artist> <Album> <Album Artist> <Title> <Genre> <Composer> <Year> <Original Date> <Added> <BPM> <Bitrate> <Custom x>`
    - Numbering: `<Track#> <Disc#> <Auto Number> <Random>`
    - File parts: `<Type> <Extension> <Filename>`
    - Path parts: `<Folder:n> <Path:n>`
    - Modifiers: `<Field:n>` is zero- or space-padded width, `<Field@n>` groups alphabetically (`<Artist@3>` gives ABC/DEF).
    - Functions: `$If(cond,then,else)`, `$First`, `$RemovePrefix`, `$MovePrefix` ("The Beatles" becomes "Beatles, The"), `$Group`, `$Replace`, `$Left/$Right/$Mid`, `$Trim/$Upper/$Lower`, `$Len`, `$Assign/$Use`. A trailing `\` makes a path segment a directory; `.\` means "same folder as source".
    - Example: `<Album Artist> - <Album>\<Track#> $If(<Artist>=<Album Artist>,,<Artist> - )<Title>`
- **Artwork.** Embedded art and folder images (`folder.jpg`/similar) are both supported. "Scan folders for artwork" links external files, and it is best disabled if the layout is messy. The Art Browser view and "Unknown Album Art" node cover the gaps. Album-art search runs via the web lookups.
- **Duplicates.**
  - Duplicate Titles: a plain grouped-title list.
  - Duplicate Content: an audio-signature analysis, optionally run on add.
  - Deletion is always manual, because "automatic deletion could result in the wrong track being deleted".
- **Files to Edit nodes** (MM4 documented; MM5 keeps the idea under Collection > Files to Edit): Unknown Title/Artist/Album/Genre/Year/Rating, Unclassified, Unknown Album Art, Multiple Artist Albums (same album name with no common Album Artist), Dead Links, Unleveled Tracks, Unorganized (Gold), Unsynchronized Tags, Duplicate Titles, Duplicate Content.
- **Analyze Volume.**
  - Per-track and per-album coefficients stored in the DB as a deviation from a target (default 89 dB, up to 95 dB workable).
  - Clipping prevention.
  - "Level Playback Volume" applies them at play time and works only in MM's own player. Album mode falls back to track mode if the album gain is missing.
  - It also reads and writes ReplayGain tags.
  - "Level Track Volume" is a separate, permanent edit of the file, lossless for MP3 only.

## 4. Playlists

- **Static playlists.** Folders and nesting, drag and drop from Now Playing, and `.m3u` import (imports land under "Imported Playlists"). Export is covered by "Exporting Playlists" and "Generating Reports" (track-list export). The format list in the wiki is thin: M3U is confirmed, PLS and others are **(unverified)**.
- **AutoPlaylists** (MM4 core; "advanced" criteria are Gold):
  - Match All/Any, with nested conditions.
  - A Collection scope.
  - Limit by files, MB or minutes.
  - Selection mode, with an "(auto-refresh)" option for random picks.
  - Multi-level sort.
  - You cannot add, remove or reorder tracks by hand.
  - They live only in the DB.
  - Typical use is picking a subset of a large library to sync.

## 5. Playback

- **Auto-DJ.** Keeps N tracks queued, drawn at random from the entire library, a Collection or a playlist. The Content Type decides whether Auto-DJ applies. MM4 allowed Auto-DJ scripts.
- **Party Mode.** Locks adding, deleting and editing, with a password. It has three presets: interactive jukebox, shared DJ console, and now-playing display. Guests may or may not control playback and playlists.
- **Player.** Full player, MiniPlayer and MicroPlayer (tray toolbar), a 10-band equalizer with saved presets, visualizations, and a sleep timer ("Sleep Mode").
- **Crossfade and gapless.** Crossfade is a per-Type playback rule. Gapless playback, DSP chains, and WASAPI/output plugins exist in the MM4 and MM5 players, and Winamp 2 input, DSP and visualization plugins are supported in MM3/MM4 and in MM5 via addons. The wiki pages I fetched did not document these in depth, so details are **(unverified)**.
- **Output.** MM5 can play to UPnP/DLNA renderers and Google Cast.
- **Bookmarks.** Podcast and Audiobook Types remember position.

## 6. Device sync

- **Plugins by device class:** iPod/iPhone (MP3 and AAC, lyrics, art, ratings, play history, playlists, bookmarks), WMDM/MTP/Android (bidirectional), generic USB mass storage (tracks and playlists; syncs changes back), and iRiver.
- **Modes.** Auto-Sync takes the whole library, or a sync list built from playlists/AutoPlaylists/Collections, with an optional "Delete other files and playlists". Manual sync copies selected items once. Each device gets a *profile* (saved settings) and auto-detects on connect.
- **Auto-conversion** (Gold) transcodes unsupported formats during sync, UPnP and burning, with leveling and a per-device format rule list.
- **MediaMonkey for Android.** Wi-Fi sync needs the app's Wi-Fi Sync add-on or Pro, and the desktop media server running. The sync list chooses playlists, collections, artists or genres. Options are "Auto-sync server content", "Auto-sync device content" (back-copy to the PC), and "Delete unselected content". Sync starts from the phone, and configuration is richer on the desktop (format rules). AutoPlaylists re-sync changed content; random lists regenerate each sync.

## 7. Conversion, CD ripping and burning

- **Converter.** The formats tool is also used for rips and sync. The free version's LAME MP3 encoder is time-limited, and Gold removes the limit and adds multi-core processing.
- **Ripping.** The Rip dialog takes a directory/filename mask, a format, and a volume-level option that really modifies the output. A rip reports success or failure after verification. **Secure ripping** (Gold) slows the drive on read errors and checks against AccurateRip. Freedb is the CD-info lookup in MM4. MM5 uses MusicBrainz-based identification; gnudb is **(unverified)**.
- **Cataloguing without ripping.** Adds the CD to the library as a "Virtual CD" (Gold), showing which tracks you own on disk.
- **Burning.** Audio CD, data CD/DVD, and data discs with auto-conversion. The `<Playlist>` mask tag exists for burning and export names.

## 8. Network and web

- **UPnP/DLNA server** (MM4+). Enable in Options > Library > Media Sharing. Several named servers can run on separate IP ports (for example music vs movies), each exposing chosen content. An access-control tab can require approval per device, with IP-based restriction only. Real-time transcoding via the same Auto-Conversion rules. Client side: browse other servers (auto-discovered, or added by hand) and cast to renderers. MM can be controlled over UPnP as well.
- **MM5** adds Chromecast, cloud storage (Dropbox/Drive/OneDrive), and Deezer/Spotify sync.
- **Podcasts and Net Radio.** Podcast subscriptions with a download manager; Net Radio is a node in the tree (MM4), with a `Radio` table in MM5.

## 9. Extensibility

- **MM4 scripting.** VBScript/JScript, with a COM automation API (an `SDB` object for DB access, plus UI hooks to add menus and toolbars). There are "Search scripts" (extra Auto-Tag sources) and "Auto-DJ scripts" (custom queue logic). Install by dropping scripts in a folder with an `.ini`/`.mmip` package. Skins use an XML/image format. The wiki documents DB access by raw SQL against SQLite.
- **MM5 addons (`.mmip`).** A ZIP with `info.json` (id, version, `minAppVersion`, `type`, `config`, and others).
  - Addons override or extend core JS files (`file.js` replaces; `file_add.js` extends), add HTML views, define *actions* (title, `execute`, hotkey), and add menu entries with order numbers.
  - They use `app.db` for direct SQLite queries, `app.setValue/getValue` for persistent storage, and ship Winamp 2 DLL plugins. They self-update through `updateURL`.
  - The API reference and the "Controlling MM5 from external applications" page cover automation.
- **Popular addons.**
  - Discogs Tagger and Batch Discogs: MM4 only.
  - RegExp Find & Replace: MM4 only; its absence in MM5 is a "showstopper" for some users.
  - Magic Nodes (user-defined virtual tree nodes): MM4 era. I could not confirm an MM5 port.
  - Last.fm scrobbler: **(unverified)** in MM5.
  - MM5 "Links" addon (web links in list view).
  - **Lesson:** MM4 addons had to be rewritten for MM5, and several authors never did so.

## 10. Reports, backup, Gold vs free

- **Reports.** "Generating Reports" exports track lists (text/CSV-style), plus playlist export.
- **Backup.** MM4: copy the settings directory (`AppData\Roaming\MediaMonkey`). Registry settings are not included. MM5: automatic DB backup and restore. AutoPlaylists, collections, ratings and play stats live only in the DB, so losing it means losing them.
- **Portable mode (MM4):** run from USB. There is also a "Multi-user environments" guide.
- **Gold-only:** custom Collections, advanced AutoPlaylists, auto-conversion, secure ripping/AccurateRip, Virtual CD, background Auto-Organize, unlimited MP3 encoding, multi-core encoding, Locate Moved/Missing Files. Everything else is free for personal use.

## 11. What users loved and complained about

**Loved**
- Handles 100k+ tracks. MM3.0.3 explicitly targeted this, and one user ran 400 GB of music on MM4.
- The depth of automation: file-monitor, Auto-Organize and masks, AutoPlaylists, volume leveling, and sync profiles.
- Powerful, scriptable tag and library control, and a big MM4 addon ecosystem.
- A decade-old PCWorld review praises the organizing and the "Files to Edit" workflow, and criticizes the dated UI and steep learning curve.

**Complaints**
- *MM5 vs MM4:*
  - Weaker tagging: Discogs is gone and MusicBrainz is "far less comprehensive".
  - Missing addons, for example RegExp.
  - Limited keyboard operation and fewer power-user options.
  - The MM4 tree "All" node was removed.
  - Party mode reloads the app when it is turned off.
- *Large libraries:*
  - One user with about 150,000 files found drilling down by artist slow in MM5.
  - The vendor says early MM5 prioritized features over performance, and claims parity or better.
  - One NAS-scan anecdote showed MM5 faster (1:52 vs 3:21), attributed partly to art caching.
  - On a hi-fi forum, a user with a ~127k-file library reported MediaMonkey slowing down and being buggy. That post did not name the version, and the replies recommended JRiver.
- *Network and removable media:*
  - Drive IDs break on PC or OS changes.
  - Mapped drives are discouraged in favor of UNC.
  - Monitoring on remote shares is unreliable.
  - Files moved outside monitored folders become dead links.
- Network latency matters: many tag-writes on a NAS are slow (a common observation, from Mp3tag and Roon threads, not MM-specific). **(unverified)** I found no MM-specific benchmark.

## 12. Design takeaways for a Linux "advanced mode"

1. Use stable volume identity (filesystem UUID or label plus path) instead of device letters. Add an "offline/disconnected" state that is separate from "dead link", and a re-assign-volume action.
2. Keep Collections as saved criteria that bundle the visible columns and tree nodes, with Content Types driving playback and sync rules.
3. Build one query language and reuse it for search, AutoPlaylists, Collections, sync lists and Auto-DJ sources.
4. Copy the mask language: tags, padding and grouping modifiers, `$If`, `$Replace`, `$MovePrefix`, and a live preview with per-row override.
5. Ship the Files to Edit audit nodes as virtual queries.
6. Make duplicate removal manual by default.
7. Take a lesson from MM5: don't break addon compatibility. Keep scripting stable and expose DB-level access.
8. Keep the DB on local disk, and allow cheap rescans via timestamp and size.

## Sources
- https://en.wikipedia.org/wiki/MediaMonkey
- https://www.mediamonkey.com/wiki/Main_Page
- https://www.mediamonkey.com/wiki/WebHelp:Content/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Content/5.0
- https://www.mediamonkey.com/wiki/WebHelp:What's_New/5.0
- https://www.mediamonkey.com/wiki/What's_New_in_V4
- https://www.mediamonkey.com/wiki/WebHelp:Adding_Existing_Files_to_the_Library/5.0
- https://www.mediamonkey.com/wiki/WebHelp:Changing_Filenames/5.0
- https://www.mediamonkey.com/wiki/WebHelp:Configuring_Directory_and_File_Formats/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Playback_Rules/4.0/de
- https://www.mediamonkey.com/wiki/WebHelp:Library/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Search/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Using_AutoPlaylists/5.0
- https://www.mediamonkey.com/wiki/WebHelp:Using_Auto-DJ/5.0
- https://www.mediamonkey.com/wiki/WebHelp:Protecting_Your_Library_with_Party_Mode/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Player/4.0
- https://www.mediamonkey.com/wiki/WebHelp:My_Computer
- https://www.mediamonkey.com/wiki/WebHelp:Getting_Track_Information_from_the_Internet/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Deleting_Duplicates/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Volume_Leveling/5.0
- https://www.mediamonkey.com/wiki/WebHelp:Exporting_Tracks/4.0
- https://www.mediamonkey.com/wiki/Sync_MediaMonkey_with_an_Android_device
- https://www.mediamonkey.com/wiki/WebHelp:Ripping_Tracks_from_CDs/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Setting_UPnP_DLNA_Media_Servers/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Upgrading_to_the_Gold_Version/4.0
- https://www.mediamonkey.com/wiki/WebHelp:Backing_up_the_Library/4.0
- https://www.mediamonkey.com/wiki/Scripting_(MM4)
- https://www.mediamonkey.com/wiki/Database_Structure_(MM4)
- https://www.mediamonkey.com/wiki/Getting_Started_(Addons)
- https://www.mediamonkey.com/addons/browse/popular
- https://www.mediamonkey.com/forum/viewtopic.php?t=101555
- https://www.mediamonkey.com/forum/viewtopic.php?t=98094
- https://www.mediamonkey.com/forum/viewtopic.php?t=102821
- https://www.mediamonkey.com/forum/viewtopic.php?t=99800
- https://www.mediamonkey.com/forum/viewtopic.php?t=54508
- https://www.pcworld.com/article/2030141/review-mediamonkey-organizes-your-mp3s.html
- https://forum.polkaudio.com/discussion/comment/2130362/
