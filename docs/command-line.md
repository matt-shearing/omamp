# The command line

The library manager's core also works from a terminal, and the window and the
command line share one library, so a scan or a tag edit from either shows in
the other. Run `thrum help` for every command and the query syntax.

## Opening the player

```sh
thrum                                    # open the player
thrum ~/Music/Album song.flac            # add these and play the first
thrum mini ~/Music/Album                 # open as the mini player
thrum micro                              # open as the micro player (Winamp's windowshade)
```

## The library

```sh
thrum scan ~/Music                       # add a folder to the library and scan it
thrum scan                               # rescan every library folder; unchanged files are only checked
thrum scan --quick                       # rescan, skipping folders whose modification time is unchanged
thrum watch                              # watch the library folders and rescan what changes (Ctrl+C stops)
thrum query 'artist:"neon coast" year:1997..2001'
thrum query 'rating:4.. -genre:christmas' --json
thrum stats                              # count what is in the library
thrum stats --top artists --since 30d    # what you played most lately
thrum locate /mnt/usb/Music              # find missing tracks that were moved there
thrum art album 12 --size 256            # print the path of an album's picture, made if needed
```

A file moved or renamed inside the library keeps its rating, play count,
history and playlists: a scan recognises it by its size, length and tags, or
by its contents. Tracks on a disk or share that isn't mounted show as offline
and are never removed. Folders on network shares are never watched, because
changes made by other machines can't be seen; `thrum watch` rescans them every
15 minutes instead, by folder modification time.

The query language is in [query-language.md](query-language.md).

## Tags and files

```sh
thrum tag 'album:"night drive"' --set genre=Synthwave
thrum tag-from-filename 'path:Downloads' --pattern '<Track#> - <Title>'
thrum organise 'artist:"neon coast"' --mask '<Album Artist>/<Album>/<Track#:2> <Title>' --dry-run
thrum undo                               # undo the last tag edit or organise
thrum journal                            # list recent changes
thrum audit                              # count tracks that need work (Files to Edit)
thrum duplicates --by fingerprint        # the same song twice; nothing is deleted
thrum identify 'album:"night drive"' --dry-run   # match with MusicBrainz
thrum fetch-art --missing                # front covers from the Cover Art Archive
thrum lyrics 'title:coastline' --fetch   # lyrics from LRCLIB
```

Masks are in [masks.md](masks.md), and the web services in
[web-metadata.md](web-metadata.md).

## Playlists and Auto-DJ

```sh
thrum playlist create 'Road trip'
thrum playlist add 'Road trip' 'genre:rock rating:4..'
thrum playlist add 'Road trip' spotify:track:<id>   # a Spotify track in a Thrum playlist
thrum playlist export 'Road trip' road-trip.m3u8
thrum autoplaylist create 'Five stars' 'rating:5'
thrum autodj --source 'playlist:Road trip' --count 20
thrum import mediamonkey MM.DB --map 'D:\Music=/mnt/music' --dry-run
```

See [playlists.md](playlists.md) and
[mediamonkey-import.md](mediamonkey-import.md).

## Devices and reports

```sh
thrum sync create Phone --to /run/media/$USER/PHONE/Music --kind usb \
    --source 'playlist:Road trip' --rule 'lossless->opus:128'
thrum sync run Phone --dry-run           # what a sync would copy, convert and delete
thrum sync run Phone
thrum report 'rating:5' --out five-stars.csv
thrum report --playlist 'Road trip' --format html --out road-trip.html
```

See [sync.md](sync.md).

## NAS and Navidrome

```sh
thrum volume list                        # disks and shares, and whether they are connected
thrum nas mount-guide --smb //nas/music --at /mnt/music
thrum remote add subsonic https://nas:4533 --user you --password-stdin
thrum remote sync all
```

See [nas.md](nas.md).

## Radio and Spotify

```sh
thrum radio list --group nightride       # the Nightride FM channels
thrum radio play darksynth               # tune the running Thrum in
thrum radio search 'jazz'                # find stations on radio-browser.info
thrum spotify status                     # is spotify_player signed in and running? Premium?
thrum spotify search 'neon coast' --json # Spotify's tracks, albums, artists and playlists
```

See [radio.md](radio.md) and [spotify.md](spotify.md).

## For scripts and agents

Every command takes `--json` for output a program can read and `--db <path>`
for another library, and exits 0 when it worked or 1 when it didn't, with the
reason on stderr. The library lives in `~/.local/share/thrum/library.db`.
