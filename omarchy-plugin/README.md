# Omamp in the Omarchy bar

A bar widget and panel for Omamp, plugin id `contra.omamp`.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../docs/omarchy-plugin-dark.png">
  <img alt="The Omamp widget in the bar and its panel" src="../docs/omarchy-plugin-light.png">
</picture>

The picture is rendered off-screen from `Widget.qml` with a made-up track and cover.

While Omamp is running, the bar shows a small cover, its play state and
"Artist – Title". A long line pans slowly inside a fixed width. The play
glyph turns the theme's green while music plays. With no cover, the glyph
stands alone. When Omamp closes, the widget disappears.

Click it to open the panel. The cover fills the top of the panel, with the
title, artist and album under it, then a seek bar, previous, play or pause,
stop and next, shuffle and repeat, Omamp's own volume, and a **Show Omamp**
button that brings the window forward.

The cover is the picture Omamp sends over MPRIS as `mpris:artUrl`, so local
files, Spotify tracks, radio station tiles and Navidrome albums all show
theirs. When the track changes, the old cover stays until the new one has
loaded, then fades across. A music note on the theme background stands in
when there is no cover.

| On the widget | Does |
| --- | --- |
| Left click | Open the panel |
| Right click | Play or pause |
| Middle click | Next track |
| Scroll | Previous or next track |

The time under the seek bar is Omamp's green clock. Click it to switch
between time played and time left, as in Omamp. Omamp's full LCD, with the
large clock and the scrolling title strip, is a setting (`showLcd`).

## Why it is not the built-in media widget

Omarchy's `omarchy.media` widget follows whichever player is active. It
offers play or pause, previous and next. This widget follows Omamp only.
It adds seek, stop, shuffle, repeat, Omamp's volume and raising the window,
all over Omamp's MPRIS interface (`org.mpris.MediaPlayer2.omamp`).

## Settings

In Setup > Bar, or inline on the `contra.omamp` entry in
`~/.config/omarchy/shell.json`:

| Key | Default | What it does |
| --- | --- | --- |
| `showThumbnail` | `true` | Shows the small cover in the bar |
| `coverSize` | `Large` | The cover in the panel: `Large` fills the top, `Medium` sits beside the track, `Hidden` leaves it out |
| `showLcd` | `false` | Adds Omamp's green clock and scrolling title strip to the panel |
| `maxLabelWidth` | `220` | The width past which the line pans instead of growing |
| `showClock` | `false` | Shows the elapsed time, in green, after the line |
| `showWhenClosed` | `false` | Keeps a dimmed icon while Omamp is closed; clicking it opens Omamp |
| `scrollAction` | `Previous/next track` | Or `Volume` (Omamp's), or `Nothing` |

The widget hides while Omamp is closed, as Omarchy's media widget does, so
the bar carries no dead icons. Turn on `showWhenClosed` if you would rather
have a launcher there.

## Install

```sh
omarchy-plugin/install                  # link the plugin and put it on the bar
omarchy-plugin/install --no-bar         # link only
omarchy-plugin/install --section left   # choose the section
omarchy-restart-shell
```

The install links `~/.config/omarchy/plugins/contra.omamp` to this folder,
so pulling the repo updates it. It puts the widget after `contra.nightride`
when that is on the bar, and in the right section otherwise.
`omarchy-plugin/uninstall` takes it off the bar and removes the link.

## Scripting it

```sh
omarchy-shell contra.omamp status        # JSON: state, track, artUrl, cover, position, volume, shuffle, repeat
omarchy-shell contra.omamp playPause     # also stop, next, previous, shuffle, repeat, raise, launch
omarchy-shell contra.omamp volume 40     # Omamp's volume, in percent
omarchy-shell shell toggle contra.omamp  # open or close the panel
```

## Tests

```sh
node omarchy-plugin/tests/model.test.mjs
/usr/lib/qt6/bin/qmllint omarchy-plugin/*.qml
omarchy plugin validate "$PWD/omarchy-plugin"
```
