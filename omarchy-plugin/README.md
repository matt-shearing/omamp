# Omamp in the Omarchy bar

A bar widget and panel for Omamp, plugin id `contra.omamp`.

![The Omamp widget and its panel](../docs/omarchy-plugin.png)

The picture is rendered off-screen from `Widget.qml` with made-up track data.

While Omamp is running, the bar shows its play state and "Artist – Title".
A long line pans slowly inside a fixed width. The play glyph turns the
theme's green while music plays. When Omamp closes, the widget disappears.

Click it to open the panel. The panel has Omamp's green clock and title
strip, the track, artist and album, a seek bar, previous, play or pause,
stop and next, shuffle and repeat, Omamp's own volume, and a **Show Omamp**
button that brings the window forward.

| On the widget | Does |
| --- | --- |
| Left click | Open the panel |
| Right click | Play or pause |
| Middle click | Next track |
| Scroll | Previous or next track |

Click the clock in the panel to switch between time played and time left,
as in Omamp.

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
omarchy-shell contra.omamp status        # JSON: state, track, position, volume, shuffle, repeat
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
