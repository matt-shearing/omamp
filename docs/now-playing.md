# Now playing

Omamp shows what is playing in five places: the deck, the strip at the top
of advanced mode, the Now Playing list, the info panel and the big Now
Playing view. Each shows the track's cover. The mini and micro players
shrink the window to a strip, and the deck can wear three styles.

![The big Now Playing view in the Night Ride theme](now-playing-big.png)

## The cover

The cover comes from the first of these that has one:

1. For a track in the library, the library's picture: the track's own
   embedded picture, else its album's (`cover.jpg`, `folder.jpg` or
   `front.jpg` beside the files, or a cover chosen from the Cover Art
   Archive).
2. For a Navidrome or Subsonic track, the server's cover.
3. For a file outside the library, the picture in its tags, else a cover,
   folder or front picture in its folder. A disc folder such as `CD1` also
   looks one folder up.

The deck finds covers without opening the library window. Pictures go into
the artwork cache, `~/.cache/omamp/art`, at 96, 256 and 600 pixels. When
nothing is found, a music note in the style's colours takes the cover's
place.

Omamp also sends the 600-pixel cover over MPRIS as `mpris:artUrl`, so the
Omarchy bar, `playerctl metadata` and other MPRIS clients show it.

## The big Now Playing view

Press `N`, click the cover on the deck or the strip, or choose **Big view**
in Now Playing. The view fills the window with:

- the cover, large;
- the title, artist, album and year;
- the rating, as five stars. Click a star to set it. The rating goes to the
  library, and to the file when **Settings > Library** says to write
  ratings;
- the position, with the time played, the time left and the length. Click
  or drag to seek;
- the transport, shuffle, repeat and volume;
- the next five tracks in the list. Double-click one to play it;
- timed lyrics, scrolling with the song, when the track has them (from a
  `.lrc` file, its tags, or LRCLIB once fetched in the info panel);
- a visualiser along the bottom.

| Key | Does |
| --- | --- |
| `N` | Open or close the big view |
| `F11` | Fullscreen, and back |
| `Esc` | Leave fullscreen, then the big view |
| `Shift+V` | Next visualiser style |
| `Shift+L` | Lyrics on or off |

The background is the cover blurred, under a shade of the style's colour.
The blur is worked out once on the CPU and saved beside the cover, so it
needs no graphics card and looks the same with Qt's software renderer.
**Settings > Appearance** can use a plain colour instead.

Party mode's now-playing display is this view, without the buttons that
party mode locks.

## The mini and micro players

| Key | Does |
| --- | --- |
| `Ctrl+M` | The mini player, and back |
| `Ctrl+Shift+M` | The micro player, and back |

![The mini player](mini-player.png)

The **mini player** is a 420 by 64 strip with the cover, the scrolling
title, the clock, a seek bar, the transport and the volume. It is
MediaMonkey's MiniPlayer.

![The micro player](micro-player.png)

The **micro player** is a 275 by 28 bar with the play state, the clock,
the scrolling title, the transport and a thin position line along the
bottom. It is Winamp's windowshade mode.

Click the button at the right end, or double-click an empty part, to go
back to the full window. Each player remembers its own size.
`omamp mini` and `omamp micro` start Omamp in that player.

### Floating and pinning them in Hyprland

Wayland lets no window place itself or keep itself on top, so a window rule
does it. The compact players have fixed titles, `Omamp mini` and
`Omamp micro`, for the rule to match. On Omarchy 4 (Hyprland's Lua
configuration), put this in a file of your own in `~/.config/hypr/`, such
as `omamp.lua`, and add `require("omamp")` to `~/.config/hypr/hyprland.lua`:

```lua
-- Omamp's mini and micro players: floating, on every workspace, top right
-- under the bar, without taking the focus.
o.window({ class = "^omamp$", title = "^Omamp mini$" }, {
  float = true,
  pin = true,
  no_initial_focus = true,
  size = { 420, 64 },
  move = { "(monitor_w-window_w-24)", "(monitor_h*0.05)" },
})
o.window({ class = "^omamp$", title = "^Omamp micro$" }, {
  float = true,
  pin = true,
  no_initial_focus = true,
  size = { 275, 28 },
  move = { "(monitor_w-window_w-24)", "(monitor_h*0.05)" },
})
```

On a Hyprland that still reads `hyprland.conf`, write the same rule in its
`windowrule` syntax: match the class `omamp` and the title, then float, pin,
size and move.

Hyprland applies `float`, `size` and `move` when a window opens. A window
that opens as a compact player, with `omamp mini`, gets the whole rule. A
full window that you switch to the mini player keeps floating or tiling as
it was. If it is tiled, `Super+T` floats it so it can shrink, and
`Super+O` floats and pins it in one go.

## Visualisers

![The five visualiser styles](visualisers.png)

The deck, the strip and the big view share one visualiser style. Click a visualiser, or press `Shift+V`, to move on to
the next:

- **Bars**: the analyser's bars, rising from green through cyan to the
  accent.
- **Bars with peaks**: the same with a falling cap on each bar, Omamp's
  original analyser and the default.
- **Mirrored spectrum**: the bass in the middle, spreading to both sides,
  with a faint reflection below.
- **Oscilloscope**: the sound wave itself, as Winamp drew it.
- **Fire**: flames fed by the spectrum, a nod to Winamp's AVS and MilkDrop.

All of them are drawn on the CPU with QPainter: a few dozen rectangles, one
line, or one small picture a frame. Their colours come from the deck style.

## Deck styles

**Settings > Appearance** chooses how the deck, the mini players and the big
view look. The library in advanced mode always stays in the Omarchy theme.

| Style | Looks like |
| --- | --- |
| Omarchy | The current Omarchy theme, and changes with it. The default |
| Classic | Winamp 2: charcoal frame, grey bevelled panels and buttons, the green readout on black, gold title stripes |
| Silver | MediaMonkey in the 2000s: brushed silver panels, glossy buttons, a navy readout, blue accents |

![The Classic style](deck-classic.png) ![The Silver style](deck-2000s.png)

The page also turns the cover on the deck on or off, picks the visualiser,
chooses the big view's background, and turns its lyrics on or off. The
palette (`Ctrl+K`) has **Deck style: Classic** and the rest.

These settings live in `~/.config/omamp/window.conf`, under `[appearance]`
and, for the compact players' sizes, `[compact]`.
