# The query language

One language finds music everywhere in Omamp: the search box, auto-playlists,
collections, Auto-DJ sources, sync lists, the command palette and
`omamp query`. A query is a line of words and filters. Every part must match,
unless you join parts with `OR`.

```text
neon coast                              tracks with both words anywhere
artist:"neon coast" year:1997..2001     that artist, in those years
rating:4.. -genre:christmas             four stars or more, no Christmas songs
added:<30d plays:0                      added this month and never played
genre:rock played:>1y sort:random limit:2h
                                        two hours of rock you have not heard for a year
```

Try any of them from a terminal:

```sh
omamp query 'added:<30d plays:0'
omamp query --count 'genre:none'
omamp query --explain 'rating:4.. sort:-plays limit:50'
```

## Words

| You type | It finds |
|---|---|
| `coast` | tracks with "coast" anywhere in the title, artist, album, album artist, composer, genre or file name, including inside a word ("Neon Coast", "Coastline") |
| `neon coast` | tracks with both words, in any field and any order |
| `"night drive"` | the phrase, with its words together and in that order |
| `+go` | a word that starts with "go": "Go West" but not "Ego" |
| `+"night dr"` | a phrase at the start of a word |

Words ignore case. Words of three or more letters also ignore accents, so
`bjork` finds "Björk". Shorter words, and `+word`, match accents exactly,
and shorter words ignore case only for the letters A to Z.

`OR`, `|` and `;` join terms only when they stand on their own between
spaces. Lower-case `or` is an ordinary word.

## Fields

Put a field name and a colon before a word to search one field.

| You type | It finds |
|---|---|
| `artist:coast` | "coast" anywhere in the artist |
| `artist:+neo` | an artist with a word starting "neo" |
| `artist:=Björk` | the artist exactly Björk (case does not matter) |
| `title:/^live/` | titles matching a regular expression |
| `genre:=pop` | tracks with Pop as one of their genres |

Artist, album artist, composer and genre can hold several values ("Neon Coast;
Lia"). With `=`, the query matches any one of them, so `artist:=Lia` finds a
duet. Without `=`, it searches the whole text.

Regular expressions ignore case unless they say otherwise. Write `\/` for a
slash inside one.

## Numbers and ranges

| You type | It finds |
|---|---|
| `year:1997` | 1997 |
| `year:>2000` | after 2000; also `<`, `>=` and `<=` |
| `year:1997..2001` | 1997 to 2001, both included |
| `year:1997..` | 1997 or later |
| `year:..1979` | 1979 or earlier |
| `plays:0` | never played |
| `plays:10..` | played ten times or more |
| `rating:4` | four stars, or anything that rounds to four: from 3½ up to, but not including, 4½ |
| `rating:4..` | four stars or more |
| `bitrate:<192` | below 192 kbps |

Ratings are in stars, 0 to 5, with halves allowed (`rating:4.5..`).

## Lengths and sizes

Lengths take minutes and seconds the way a clock writes them, or units.

| You type | It finds |
|---|---|
| `length:>10m` | longer than ten minutes |
| `length:3:00..5:00` | from three to five minutes, up to 5:00.999 |
| `length:<90s` | shorter than a minute and a half |
| `length:1h30m..` | an hour and a half or more |
| `length:200` | 200 seconds, to the second |
| `length:4m` | four minutes, from 4:00 to 4:59 |

The units are `h`, `m` (or `min`) and `s`. A plain number is seconds.

Sizes take `kb`, `mb`, `gb` and `tb`, counted in 1,024s: `size:>20mb`,
`size:..4mb`. A plain number is bytes.

## Dates

Three fields are dates: `added` (when the track entered the library),
`played` (when it was last played; `lastplayed` is the same field) and
`modified` (when its tags last changed).

A date can be a calendar date or an age.

| You type | It finds |
|---|---|
| `added:2024` | any time in 2024 |
| `added:2024-06` | any time in June 2024 |
| `added:2024-06-30` | that day |
| `added:2024-01..2024-06` | from 1 January to the end of June 2024 |
| `added:<2024` | before 2024 |
| `added:>2024` | after 2024 |
| `added:today`, `added:yesterday` | that day |
| `added:<30d` | less than 30 days ago |
| `added:30d` | the same: in the last 30 days |
| `played:>1y` | last played more than a year ago |
| `played:never` | never played |
| `lastplayed:<7d` | played in the last week |
| `added:30d..7d` | between 30 and 7 days ago |

The age units are `min`, `h`, `d`, `w`, `m` (months) and `y`. In dates, `m`
means months; in lengths it means minutes.

With an age, `<` means more recent and `>` means longer ago, as in "less than
30 days ago". With a calendar date, `<` means before and `>` means after.

Dates use your computer's time zone. A track added at 23:30 on New Year's Eve
counts in the old year.

`played:>1y` leaves out tracks that were never played. For both, write
`played:>1y OR played:never`.

## Empty fields

| You type | It finds |
|---|---|
| `album:none` | tracks with no album |
| `genre:any` | tracks with a genre |
| `-genre:none` | the same as `genre:any` |
| `rating:none` | unrated tracks (a rating of 0 stars is a rating) |
| `played:never` | never played; `played:none` is the same |
| `year:none` | no year |

`none`, `never` and `any` work on every field. To search for the word itself,
quote it: `album:"none"`.

## Combining

| You type | It finds |
|---|---|
| `a b` | both |
| `a OR b` | either; also `a \| b` and `a ; b` |
| `-a` | not a |
| `-(a OR b)` | neither |
| `(a OR b) c` | a or b, and c |

`OR` binds tighter than the space between words, as in MediaMonkey:
`rating:4.. genre:rock OR genre:metal` means four stars or more, in rock or
metal. Use brackets when you mean something else.

A track whose field is empty never matches a filter on it, and so it does
match the opposite: `-rating:4..` includes unrated tracks.

## Sorting

`sort:` takes one or more fields, separated by commas. A minus sign sorts
that field from high to low.

| You type | It sorts by |
|---|---|
| `sort:-rating,artist` | rating, best first, then artist A to Z |
| `sort:-added` | newest first |
| `sort:played` | longest since last played first |
| `sort:-length` | longest first |
| `sort:random` | a shuffle |
| `sort:random:42` | the shuffle numbered 42, the same every time |
| `sort:randomalbum` | albums in a shuffled order, each album in track order |

Empty values go last whichever way round you sort, so `sort:-rating` and
`sort:rating` both end with the unrated tracks. Tracks that tie follow the
library order: album artist, album, disc, track.

Without `sort:`, tracks come in library order.

A shuffle is fixed by a seed. The window keeps one seed while you scroll, so
the list does not reshuffle under you, and picks a new one when you ask for
a new shuffle. `omamp query` picks a new seed each run and prints it in its
JSON as `seed`; pass `--seed` to repeat that run.

## Limits

`limit:` caps the result by count, playing time or size, after sorting.

| You type | It keeps |
|---|---|
| `limit:50` | the first 50 tracks |
| `limit:2h` | tracks until the next would take the total over two hours |
| `limit:90m`, `limit:1:30:00` | 90 minutes |
| `limit:700mb` | tracks until the next would take the total over 700 MB |
| `limit:4gb` | 4 GB |

A limit by time or size stops at the first track that does not fit; it does
not skip ahead to find a shorter one. A query has at most one `limit:`.

`sort:` and `limit:` apply to the whole query, so they cannot go inside
brackets, after a minus sign or next to `OR`. Put them anywhere else on the
line; the end reads best.

## Fields

| Field | Also | Kind |
|---|---|---|
| `title` | `ti` | text |
| `artist` | `ar` | text, several values |
| `albumartist` | `aa`, `album_artist` | text, several values |
| `album` | `al` | text |
| `composer` | `co` | text, several values |
| `genre` | `ge` | text, several values |
| `filename` | `file` | text: the file name without folder or extension |
| `path` | `folder` | text: the path inside its library folder |
| `comment`, `grouping`, `isrc` | | text |
| `publisher` | `label` | text |
| `custom1` to `custom5` | | text |
| `year` | `date` | number |
| `originalyear` | `original_year` | number |
| `track` | `tracknumber`, `tn` | number |
| `disc` | `discnumber` | number |
| `bpm` | `tempo` | number |
| `bitrate` | | number, kbps |
| `samplerate` | `sample_rate` | number, Hz |
| `channels` | | number |
| `plays` | `playcount`, `play_count` | number |
| `skips` | `skipcount`, `skip_count` | number |
| `compilation` | | 1 or 0 |
| `rating` | `stars` | stars, 0 to 5 |
| `length` | `duration`, `time` | length |
| `size` | `filesize` | size |
| `added` | `dateadded`, `date_added` | date |
| `played` | `lastplayed`, `last_played` | date |
| `modified` | `updated` | date |
| `type` | `content_type` | one of music, audiobook, podcast, classical |
| `status` | `availability` | one of online, offline, missing |
| `format` | `ext`, `codec` | flac, mp3, m4a, ogg, opus... |

Field names ignore case. An unknown field is an error, not a word, so a typo
never quietly returns the wrong tracks.

## Errors

Every error says what is wrong and where, counting characters from 1:

```text
$ omamp query 'artst:neon'
omamp: Unknown field "artst"; did you mean artist:? (at character 1)

$ omamp query 'added:soon'
omamp: added: needs a date (2024, 2024-06, 2024-06-30, today) or an age (30d, 2w, 6m, 1y), not "soon" (at character 1)

$ omamp query '(neon sort:year)'
omamp: sort: applies to the whole query, so it can't go inside ( ), after - or next to OR (at character 7)
```

With `--json`, the error is also printed on standard output as
`{"ok": false, "error": "..."}`, and the exit code is 1.

## Rules in the auto-playlist editor

The auto-playlist editor shows a query as rows of field, operator and value,
matched all (`genre:jazz rating:4..`) or any (`genre:rock OR genre:metal`),
plus `sort:` and `limit:`. The rows offer contains, is (`=`), does not
contain and is not (`-`), less than, at most, more than, at least, between
(`..`), matches the pattern (`/…/`), is empty (`none`) and is set (`any`).
The text stays the source of truth. Brackets, AND mixed with OR, and
`+word` can't be shown as rows; the editor then marks the query as advanced
and leaves it as text. A query counts as representable only when the rows
give back one that compiles to exactly the same SQL.

## The command line

```text
omamp query <text...>     print the tracks a query finds
  --json                  as JSON, with total, count, duration_ms and seed
  --count                 print only how many tracks match
  --limit <n>             print at most n (after the query's own limit:)
  --offset <n>            skip the first n
  --seed <n>              the shuffle for sort:random
  --explain               print the SQL instead of running it
  --db <path>             another library file
```

## For developers

`src/core/query.h` holds the language. `Query::parse()` turns text into a
tree of `Query::Node` plus the `sort:` and `limit:` clauses, each with its
position in the text. `Query::compile()` turns that into a SQL condition over
`tracks t` with its parameters, an `ORDER BY` and a `Query::Limit`. User text
only ever reaches SQL as a parameter. `Query::run()` runs a query and pages
through it; `Query::ids()` gives the ids in order for playlists and Auto-DJ.
`Query::Context` pins the clock and the shuffle seed.

Words of three or more letters use the trigram search index (`tracks_fts`);
shorter words fall back to `LIKE`. `+word` uses the index to narrow the rows,
then a regular expression to check the word start. The shuffle is plain SQL
arithmetic on the track id and the seed, so any connection can run a compiled
query without registering a function.

To add a field, add a line to `Query::fields()`. To add a kind of value,
extend `Parser::parseValue()` and the compiler's function for that field
type. Tests live in `tests/query_tests.cpp`; timings on 200,000 tracks are in
[plan/progress.md](plan/progress.md).
