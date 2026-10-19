# Masks

A mask describes a file's place from its tags. Omamp uses masks to organise
files into folders, to set a tag from other tags, and, read backwards, to fill
tags from file names. The syntax is MediaMonkey's Auto-Organize mask, with a
few functions borrowed from foobar2000.

```text
<Album Artist>/<Album> (<Year>)/<Disc#>-<Track#:2> <Title>
    Neon Coast/Night Drive (1997)/1-02 Coastline.flac

<Artist@1>/$MovePrefix(<Artist>)/<Album>/<Track#:2> <Title>
    B/Beatles, The/Abbey Road/01 Come Together.mp3

<Album Artist>/<Album>/$If(<Artist>=<Album Artist>,,<Artist> - )<Title>
    Various Artists/Summer Mix/Sunset Riders - Heatwave.mp3
```

Omamp adds the file's own extension, so a mask never names one.

Try a mask without moving anything:

```sh
omamp organise 'album:"night drive"' --mask '<Album Artist>/<Album>/<Track#:2> <Title>' --dry-run
```

## Fields

A field is a name in angle brackets. Names ignore case, spaces and
underscores, so `<Album Artist>`, `<albumartist>` and `<album_artist>` are
the same field.

| Field | Gives |
|---|---|
| `<Title>` `<Album>` `<Genre>` `<Composer>` `<Comment>` | the tag |
| `<Artist>` | every artist, joined with "; " |
| `<Album Artist>` | the album artist; for a compilation without one, "Various Artists"; otherwise the artist |
| `<Year>` `<Original Date>` | the year, as four digits |
| `<Track#>` `<Disc#>` `<Track Total>` `<Disc Total>` | the numbers |
| `<Bitrate>` `<BPM>` | the numbers, bitrate in kbps |
| `<Rating>` | stars, such as 4.5 |
| `<Type>` | music, audiobook, podcast or classical |
| `<Format>` | the file's format, such as flac |
| `<Added>` | the day the track was added, as 2026-10-20 |
| `<Length>` | minutes and seconds, as 4:05 |
| `<Custom1>` to `<Custom5>` | the custom fields |
| `<Filename>` | the file's name without its extension |
| `<Extension>` | the extension without its dot |
| `<Folder>`, `<Folder:n>` | the folder holding the file; `:2` is the folder above that, and so on |
| `<Path>`, `<Path:n>` | the folders between the library folder and the file; `:1` is the first of them |
| `<Auto Number>` | the track's place in the batch, counting from 1 |

An empty field gives nothing. A field that the track lacks never fails the
mask.

### Modifiers

| Written | Gives | Example |
|---|---|---|
| `<Track#:2>` | a number padded with zeros to two digits | `07` |
| `<Album:10>` | text cut to ten characters | `Night Driv` |
| `<Artist@1>` | the artist's first letter, accents removed; `#` for a digit or symbol | `B` |
| `<Artist@3>` | the letter's group of three | `ABC`, `DEF`, … `YZ` |

MediaMonkey pads text with spaces for `:n`. Omamp cuts it instead, since
trailing spaces make poor file names.

## Folders

A `/` or a `\` in the mask starts a new folder. A slash inside a tag does not:
the artist "AC/DC" gives the folder `AC_DC`. When a field leaves a folder
name empty, that folder is left out, so `<Album Artist>/<Album>/<Title>`
on a track with no album artist gives `Night Drive/Coastline`.

Inside a function, a slash also starts a folder. This puts each disc in a
folder of its own, but only for albums with discs:

```text
<Album>/$If(<Disc#>,CD<Disc#>/,)<Track#:2> <Title>
```

## Functions

A function is a `$`, a name and its arguments in brackets, separated by
commas. Arguments can hold fields, text and other functions.

| Function | Gives | Example |
|---|---|---|
| `$If(cond,then,else)` | `then` when `cond` is not empty, otherwise `else` (which may be left out) | `$If(<Year>,(<Year>),)` |
| `$If(a=b,then,else)` | compares, ignoring case; numbers compare as numbers, so `01=1` | `$If(<Disc#>=1,,Disc <Disc#>)` |
| `$If(a!=b,then,else)` | the opposite | |
| `$IfEmpty(value,fallback)` | `value`, or `fallback` when it is empty | `$IfEmpty(<Album>,Singles)` |
| `$First(value)` | the first of several values | `$First(<Artist>)` gives "Neon Coast" from "Neon Coast; Lena Park" |
| `$First(a,b,…)` | the first argument that is not empty | `$First(<Composer>,<Artist>)` |
| `$Replace(text,from,to,…)` | each `from` replaced by its `to`; pairs may repeat | `$Replace(<Title>,feat.,ft.)` |
| `$RemovePrefix(text,prefixes…)` | text without a leading word; "The" when no prefix is given | "The Cure" gives "Cure" |
| `$MovePrefix(text,prefixes…)` | the leading word moved to the end | "The Cure" gives "Cure, The" |
| `$Left(text,n)` `$Right(text,n)` | the first or last n characters | |
| `$Mid(text,start,n)` | n characters from `start`, counting from 1; without n, the rest | |
| `$Trim(text)` | text without spaces at the ends, and runs of spaces made single | |
| `$Upper(text)` `$Lower(text)` | upper or lower case | |
| `$Title(text)` | each word with a capital first letter | "don't stop" gives "Don't Stop" |
| `$Len(text)` | the number of characters | |
| `$Pad(text,n,char)` | text padded on the left to n characters with `char` (zero when left out) | `$Pad(<Track#>,3)` gives `007` |

Write `$$` for a dollar sign. Inside a function, put text in double quotes
to keep commas, brackets and slashes as plain text:

```text
$Replace(<Genre>,"; ",", ")         Synthwave; Pop  becomes  Synthwave, Pop
$Replace(<Title>,"/",-)             AC/DC Live      becomes  AC-DC Live
```

Brackets that open and close inside an argument are plain text, so
`$If(<Year>,(<Year>),)` gives `(1997)`.

## Safe names

Every folder and file name is made safe before anything moves:

- `/`, control characters, and the characters Windows refuses
  (`\ < > : " | ? *`) become `_`.
- Spaces at either end go, and so do dots at the end.
- A name that starts with a dot gets `_` instead, since a hidden file would
  vanish from the library.
- Names Windows reserves, such as `CON` and `NUL`, get `_` added.
- A name longer than 255 bytes is cut, keeping its extension.

These rules suit a disk shared with Windows or a NAS over SMB. The window
and the core can change the replacement text, turn the Windows rules off, and
limit the length of the whole path, as MediaMonkey's "trim path" option does.

## Errors

A mask with a mistake is refused before anything happens, with the position
of the mistake:

```text
$ omamp organise 'path:inbox' --mask '<Artist>/<Titel>' --dry-run
omamp: Unknown field <Titel> (at character 10 of the mask)
```

Omamp reports unknown fields and functions, a `<` without its `>`, a function
without its `)`, a quote without its partner, and the wrong number of
arguments.

## Organising files

In the window, select tracks and press `Ctrl+Shift+O` (or right-click,
**Organise files…**). The mask field completes field names after `<` and
function names after `$` (`↑` `↓` and `Tab`), and says at which character a
mask goes wrong. The preview lists every old and new place; double-click a
new place to change it. Its default mask is MediaMonkey's example made
tolerant of missing tags:

```text
<Album Artist>/<Album>$If(<Year>, (<Year>))/$If(<Disc#>,<Disc#>-)<Track#:2> <Title>
```

From the command line:

```sh
omamp organise <query> --mask '<mask>' --dry-run      # show where each file would go
omamp organise <query> --mask '<mask>' --apply        # move them
omamp undo                                            # move them back
```

- Files go under their own library folder, or under `--to <folder>`, which
  must be on a volume the library knows.
- A name that is taken, by another file in the batch or by a file already on
  disk, gets " (2)", " (3)" and so on. Omamp never overwrites a file.
- A track's `.lrc` lyrics file moves with it. When every audio file in a
  folder moves to the same new folder, its pictures, cue sheets, logs and
  playlists go too.
- Folders left empty are deleted, up to the library folder. `--keep-folders`
  keeps them.
- Tracks keep their place in the library, so ratings, play history and
  playlists follow them.
- Within one disk a move is a rename. Between disks the file is copied,
  checked byte for byte, and only then deleted from its old place.

## Setting tags from a mask

`--set-mask` sets a tag from other tags, track by track:

```sh
omamp tag 'path:"live at home"' --set-mask 'album=<Folder:1>'
omamp tag 'artist:"the cure"' --set-mask 'artist=$MovePrefix(<Artist>)'
omamp tag 'album:"night drive"' --set-mask 'comment=Disc <Disc#> of <Disc Total>'
```

Here a slash is plain text, and names are not made safe.

## Tags from file names

A pattern reads tags back out of file names. It is a mask with fields and
text only, no functions:

```sh
omamp tag-from-filename 'artist:none' --pattern '<Track#> - <Artist> - <Title>' --dry-run
```

| Pattern | File | Tags |
|---|---|---|
| `<Track#> - <Artist> - <Title>` | `01 - Neon Coast - Midnight Highway.flac` | track 1, artist, title |
| `<Artist>/<Album>/<Track#> <Title>` | `Neon Coast/Night Drive/07 Afterglow.mp3` | the album and artist from the folders |
| `<Skip> - <Title>` | `whatever - Mr. Blue Sky.ogg` | the title only |
| `<Disc#:1><Track#:2> <Title>` | `101 Song.flac` | disc 1, track 1 |

- Each `/` in the pattern reaches one folder further up.
- `<Skip>` matches anything and keeps nothing.
- A number field matches digits only; `:n` asks for exactly n of them.
- When a text field could end in more than one place, it takes the shortest
  match, so in `<Artist> - <Title>` the first " - " ends the artist.
- Files that don't match are listed and left alone.

Every tag change, from any of these commands, can be undone with
`omamp undo`.
