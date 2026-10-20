# The command line

The library manager's core also works from a terminal, and the window and the
command line share one library, so a scan or a tag edit from either shows in
the other. Run `omamp help` for every command and the query syntax.

## Opening the player

```sh
omamp                                    # open the player
omamp ~/Music/Album song.flac            # add these and play the first
omamp mini ~/Music/Album                 # open as the mini player
omamp micro                              # open as the micro player (Winamp's windowshade)
```

## The library

```sh
omamp scan ~/Music                       # add a folder to the library and scan it
omamp scan                               # rescan every library folder; unchanged files are only checked
omamp scan --quick                       # rescan, skipping folders whose modification time is unchanged
omamp watch                              # watch the library folders and rescan what changes (Ctrl+C stops)
omamp query 'artist:"neon coast" year:1997..2001'
omamp query 'rating:4.. -genre:christmas' --json
omamp stats                              # count what is in the library
omamp stats --top artists --since 30d    # what you played most lately
omamp locate /mnt/usb/Music              # find missing tracks that were moved there
omamp art album 12 --size 256            # print the path of an album's picture, made if needed
```

A file moved or renamed inside the library keeps its rating, play count,
history and playlists: a scan recognises it by its size, length and tags, or
by its contents. Tracks on a disk or share that isn't mounted show as offline
and are never removed. Folders on network shares are never watched, because
changes made by other machines can't be seen; `omamp watch` rescans them every
15 minutes instead, by folder modification time.

The query language is in [query-language.md](query-language.md).

## Tags and files

```sh
omamp tag 'album:"night drive"' --set genre=Synthwave
omamp tag-from-filename 'path:Downloads' --pattern '<Track#> - <Title>'
omamp organise 'artist:"neon coast"' --mask '<Album Artist>/<Album>/<Track#:2> <Title>' --dry-run
omamp undo                               # undo the last tag edit or organise
omamp journal                            # list recent changes
omamp audit                              # count tracks that need work (Files to Edit)
omamp duplicates --by fingerprint        # the same song twice; nothing is deleted
omamp identify 'album:"night drive"' --dry-run   # match with MusicBrainz
omamp fetch-art --missing                # front covers from the Cover Art Archive
omamp lyrics 'title:coastline' --fetch   # lyrics from LRCLIB
```

Masks are in [masks.md](masks.md), and the web services in
[web-metadata.md](web-metadata.md).

## Playlists and Auto-DJ

```sh
omamp playlist create 'Road trip'
omamp playlist add 'Road trip' 'genre:rock rating:4..'
omamp playlist add 'Road trip' spotify:track:<id>   # a Spotify track in an Omamp playlist
omamp playlist export 'Road trip' road-trip.m3u8
omamp autoplaylist create 'Five stars' 'rating:5'
omamp autodj --source 'playlist:Road trip' --count 20
omamp import mediamonkey MM.DB --map 'D:\Music=/mnt/music' --dry-run
```

See [playlists.md](playlists.md) and
[mediamonkey-import.md](mediamonkey-import.md).

## Devices and reports

```sh
omamp sync create Phone --to /run/media/$USER/PHONE/Music --kind usb \
    --source 'playlist:Road trip' --rule 'lossless->opus:128'
omamp sync run Phone --dry-run           # what a sync would copy, convert and delete
omamp sync run Phone
omamp report 'rating:5' --out five-stars.csv
omamp report --playlist 'Road trip' --format html --out road-trip.html
```

See [sync.md](sync.md).

## NAS and Navidrome

```sh
omamp volume list                        # disks and shares, and whether they are connected
omamp nas mount-guide --smb //nas/music --at /mnt/music
omamp remote add subsonic https://nas:4533 --user you --password-stdin
omamp remote sync all
```

See [nas.md](nas.md).

## Radio and Spotify

```sh
omamp radio list --group nightride       # the Nightride FM channels
omamp radio play darksynth               # tune the running Omamp in
omamp radio search 'jazz'                # find stations on radio-browser.info
omamp spotify status                     # is spotify_player signed in and running? Premium?
omamp spotify search 'neon coast' --json # Spotify's tracks, albums, artists and playlists
```

See [radio.md](radio.md) and [spotify.md](spotify.md).

## For scripts and agents

Every command takes `--json` for output a program can read and `--db <path>`
for another library, and exits 0 when it worked or 1 when it didn't, with the
reason on stderr. The library lives in `~/.local/share/omamp/library.db`.
