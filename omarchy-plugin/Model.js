.pragma library

// Pure helpers for the Thrum bar widget: which MPRIS player is Thrum, the
// labels the bar and panel draw, and the theme green. Kept free of QML so
// tests/model.test.mjs can run it under node.

// Glyphs are built from code points, never typed: editing tools can strip
// raw Nerd Font characters from a file.
var GLYPH_PLAYING = String.fromCodePoint(0xF040A) // nf-md-play
var GLYPH_PAUSED = String.fromCodePoint(0xF03E4)  // nf-md-pause
var GLYPH_STOPPED = String.fromCodePoint(0xF04DB) // nf-md-stop
var GLYPH_PREVIOUS = String.fromCodePoint(0xF04AE) // nf-md-skip_previous
var GLYPH_NEXT = String.fromCodePoint(0xF04AD)    // nf-md-skip_next
var GLYPH_SHUFFLE = String.fromCodePoint(0xF049D) // nf-md-shuffle
var GLYPH_REPEAT = String.fromCodePoint(0xF0456)  // nf-md-repeat
var GLYPH_REPEAT_OFF = String.fromCodePoint(0xF0457) // nf-md-repeat_off
var GLYPH_VOLUME = String.fromCodePoint(0xF057E)  // nf-md-volume_high
var GLYPH_MUTED = String.fromCodePoint(0xF075F)   // nf-md-volume_mute
var GLYPH_WINDOW = String.fromCodePoint(0xF05B2)  // nf-md-window_restore
var GLYPH_MUSIC = String.fromCodePoint(0xF075A)   // nf-md-music

// The bus name is the reliable part: Thrum registers
// org.mpris.MediaPlayer2.thrum, or ...thrum.instanceNNN for a second copy.
// Identity and DesktopEntry are the fallback while the player is still
// reading its properties.
function isThrum(player) {
  if (!player) return false
  var bus = String(player.dbusName || "")
  if (bus === "org.mpris.MediaPlayer2.thrum" || bus.indexOf("org.mpris.MediaPlayer2.thrum.") === 0) return true
  return String(player.desktopEntry || "") === "thrum" || String(player.identity || "") === "Thrum"
}

// The first Thrum copy wins, preferring the main bus name over an instance.
function findThrum(players) {
  var list = players || []
  var fallback = null
  for (var i = 0; i < list.length; i++) {
    var p = list[i]
    if (!isThrum(p)) continue
    if (String(p.dbusName || "") === "org.mpris.MediaPlayer2.thrum") return p
    if (!fallback) fallback = p
  }
  return fallback
}

// How many Thrum copies are on the bus; reported by `status` for checking.
function countThrum(players) {
  var list = players || []
  var n = 0
  for (var i = 0; i < list.length; i++) if (isThrum(list[i])) n++
  return n
}

// "Artist – Title", or the title alone, or nothing when the playlist is empty.
function trackLine(artist, title) {
  var a = String(artist || "").trim()
  var t = String(title || "").trim()
  if (a && t) return a + " – " + t
  return t || a
}

// mm:ss, or h:mm:ss past the hour, as Thrum's own clock shows it.
function clock(seconds) {
  var s = Number(seconds)
  if (!isFinite(s) || s < 0) s = 0
  var total = Math.floor(s)
  var h = Math.floor(total / 3600)
  var m = Math.floor((total % 3600) / 60)
  var sec = total % 60
  var mm = h > 0 && m < 10 ? "0" + m : String(m)
  if (h === 0 && mm.length < 2) mm = "0" + mm
  return (h > 0 ? h + ":" : "") + mm + ":" + (sec < 10 ? "0" : "") + sec
}

// The state as a word, for the tooltip and IPC status.
function stateName(running, playing, stopped) {
  if (!running) return "closed"
  if (playing) return "playing"
  return stopped ? "stopped" : "paused"
}

function stateGlyph(playing, stopped) {
  if (playing) return GLYPH_PLAYING
  return stopped ? GLYPH_STOPPED : GLYPH_PAUSED
}

function tooltip(running, line, state) {
  if (!running) return "Thrum is not running. Click to open it."
  if (!line) return "Thrum: nothing in the playlist"
  var word = state === "playing" ? "" : " (" + state + ")"
  return line + word
}

// Theme colours by name from colors.toml, the way Thrum itself reads them.
// Returns { green, accent } with "" for anything the theme leaves out.
function themeColors(raw) {
  var out = { green: "", accent: "", color2: "" }
  var lines = String(raw || "").split("\n")
  for (var i = 0; i < lines.length; i++) {
    var m = lines[i].match(/^\s*([A-Za-z0-9_-]+)\s*=\s*["']?(#[0-9A-Fa-f]{6})/)
    if (!m) continue
    if (m[1] === "green") out.green = m[2]
    else if (m[1] === "color2") out.color2 = m[2]
    else if (m[1] === "accent") out.accent = m[2]
  }
  return out
}

// The LCD green: the theme's `green`, then terminal colour 2, then nothing
// (the caller falls back to the theme accent).
function pickGreen(colors) {
  if (!colors) return ""
  return colors.green || colors.color2 || ""
}

function clamp01(v) {
  var n = Number(v)
  if (!isFinite(n)) return 0
  return Math.max(0, Math.min(1, n))
}

// The cover to show: Thrum sends a file:// URL; other players may send
// http(s) or an inline picture. Anything else shows the placeholder.
function coverSource(url) {
  var s = String(url || "")
  return /^(file|https?):\/\//.test(s) || s.indexOf("data:image/") === 0 ? s : ""
}

// What the bar and panel show while the cover changes. A new cover replaces
// the old at once. When the cover goes away, the old one is held for a
// moment (`hold`), because a player can send a new track's details before
// its picture, and the bar should not drop the thumbnail and then put it
// back. A closed player clears it at once.
function coverStep(current, incoming, running) {
  var next = coverSource(incoming)
  if (!running) return { cover: "", hold: false }
  if (next !== "") return { cover: next, hold: false }
  if (String(current || "") === "") return { cover: "", hold: false }
  return { cover: String(current), hold: true }
}

// The panel's cover setting: "large" (the default), "medium" or "hidden".
// Anything unknown reads as large.
function coverMode(value) {
  var v = String(value || "").trim().toLowerCase()
  if (v === "medium" || v === "hidden") return v
  return "large"
}

// The cover's side in the panel: the full content width when large, a
// thumbnail beside the details when medium, nothing when hidden.
function panelCoverSize(mode, contentWidth, mediumSize) {
  var m = coverMode(mode)
  var w = Math.max(0, Math.round(Number(contentWidth) || 0))
  if (m === "hidden") return 0
  if (m === "medium") return Math.min(w, Math.max(0, Math.round(Number(mediumSize) || 0)))
  return w
}

// The bar thumbnail's side: the bar's height less a little padding, kept
// between 12 and 28 px so a very thin or very tall bar still looks right.
function thumbSize(barSize, padding) {
  var b = Number(barSize)
  if (!isFinite(b) || b <= 0) b = 26
  var p = Number(padding)
  if (!isFinite(p) || p < 0) p = 0
  return Math.max(12, Math.min(28, Math.round(b - p)))
}

// ------------------------------------------------------------ the visualiser
// Thrum shares its analyser over a local socket (src/app/levelsfeed.h): one
// JSON line per frame, {style, reduceMotion, active, levels: 16 x 0..100}.
// The bar draws a small, faint copy of the style behind the track's name.

// The socket Thrum listens on, under $XDG_RUNTIME_DIR.
function vizSocketPath(runtimeDir) {
  var dir = String(runtimeDir || "")
  if (dir === "") return ""
  return dir.replace(/\/+$/, "") + "/thrum-levels.sock"
}

// One line of the feed, or null for anything that is not one. Levels come
// back as 0..1.
function parseVizFrame(line) {
  var o
  try { o = JSON.parse(String(line || "")) } catch (e) { return null }
  if (!o || typeof o !== "object" || Array.isArray(o)) return null
  var raw = Array.isArray(o.levels) ? o.levels : []
  var levels = []
  for (var i = 0; i < raw.length && i < 64; i++) levels.push(clamp01(Number(raw[i]) / 100))
  return {
    style: typeof o.style === "string" ? o.style : "",
    reduceMotion: o.reduceMotion === true,
    active: o.active === true && levels.length > 0,
    levels: levels
  }
}

// The style setting: "same" (as Thrum), "bars" or "line".
function vizChoice(value) {
  var v = String(value || "").trim().toLowerCase()
  if (v === "bars" || v === "line") return v
  return "same"
}

// Thrum's twelve styles fold into eight small ones the bar can draw from
// the levels alone (the oscilloscope and ribbon have no waveform here).
var MINI_STYLES = {
  bars: "bars", peaks: "bars", fire: "bars",
  mirror: "skyline", skyline: "skyline",
  scope: "ribbon", ribbon: "ribbon",
  line: "line", pulse: "pulse", led: "led",
  synthwave: "synthwave", stars: "stars"
}

function miniStyle(thrumStyle, choice) {
  var c = vizChoice(choice)
  if (c !== "same") return c
  var s = String(thrumStyle || "").toLowerCase()
  return MINI_STYLES.hasOwnProperty(s) ? MINI_STYLES[s] : "bars"
}

// How strongly it shows. Subtle by default; the busy, blocky styles are
// held back further so the words over them stay easy to read.
function vizOpacity(strength, style) {
  var base = String(strength || "").toLowerCase() === "normal" ? 0.55 : 0.32
  var s = String(style || "")
  var factor = (s === "bars" || s === "led" || s === "skyline") ? 0.8 : 1.0
  return Math.round(base * factor * 1000) / 1000
}

// Whether a frame is due: Thrum sends at most 20 a second, and the bar
// drops anything faster than minMs from a misbehaving feed.
function frameDue(lastMs, nowMs, minMs) {
  var last = Number(lastMs), now = Number(nowMs), min = Number(minMs)
  if (!isFinite(last) || last <= 0) return true
  if (!isFinite(min) || min <= 0) return true
  return now - last >= min || now < last
}

// Eases from the levels drawn last towards the new ones: k = 1 takes the
// new ones as they are (the analyser is smoothed already), less is calmer.
function easeLevels(prev, next, k) {
  var a = prev || [], b = next || []
  var t = Math.max(0, Math.min(1, Number(k)))
  if (!isFinite(t)) t = 1
  var out = []
  for (var i = 0; i < b.length; i++) {
    var p = i < a.length ? Number(a[i]) : 0
    out.push(p + (Number(b[i]) - p) * t)
  }
  return out
}

// The levels at n points across, interpolated, so the bar count can follow
// the width.
function resample(levels, n) {
  var l = levels || []
  var count = Math.max(0, Math.floor(Number(n) || 0))
  var out = []
  if (count === 0) return out
  if (l.length === 0) { for (var z = 0; z < count; z++) out.push(0); return out }
  for (var i = 0; i < count; i++) {
    var pos = count === 1 ? 0 : i * (l.length - 1) / (count - 1)
    var lo = Math.floor(pos), hi = Math.min(l.length - 1, lo + 1)
    var f = pos - lo
    out.push(clamp01(l[lo] * (1 - f) + l[hi] * f))
  }
  return out
}

// Bars that fit a width: each `bar` wide with `gap` between, centred.
// Returns [{x, y, w, h}] from the bottom of a box `height` tall. With
// `mirror`, the bass sits in the middle and the treble runs out to both
// ends (the skyline).
function barRects(levels, width, height, bar, gap, mirror) {
  var w = Math.max(0, Number(width) || 0), h = Math.max(0, Number(height) || 0)
  var bw = Math.max(1, Number(bar) || 3), g = Math.max(0, Number(gap) || 0)
  var count = Math.floor((w + g) / (bw + g))
  if (count <= 0 || h <= 0) return []
  var values
  if (mirror) {
    var half = Math.ceil(count / 2)
    var side = resample(levels, half)
    values = []
    for (var m = 0; m < count; m++) {
      var d = Math.abs(m - (count - 1) / 2)
      values.push(side[Math.min(half - 1, Math.floor(d))])
    }
  } else {
    values = resample(levels, count)
  }
  var used = count * bw + (count - 1) * g
  var x0 = (w - used) / 2
  var out = []
  for (var i = 0; i < count; i++) {
    var bh = Math.max(1, Math.round(values[i] * h))
    out.push({ x: x0 + i * (bw + g), y: h - bh, w: bw, h: bh })
  }
  return out
}

// The bass (the loudest of the lowest quarter), 0..1, for the pulse and
// the synthwave sun.
function bass(levels) {
  var l = levels || []
  var n = Math.max(1, Math.ceil(l.length / 4))
  var v = 0
  for (var i = 0; i < n && i < l.length; i++) v = Math.max(v, Number(l[i]) || 0)
  return clamp01(v)
}

// A fixed field of stars for a size: the same seed gives the same sky, so
// the bar does not flicker between frames. Each star follows one band.
function starField(count, seed) {
  var s = (Number(seed) >>> 0) || 7
  function rnd() { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296 }
  var stars = []
  for (var i = 0; i < count; i++)
    stars.push({ x: rnd(), y: rnd(), size: 0.6 + rnd() * 0.9, speed: 0.2 + rnd() * 0.8, band: rnd() })
  return stars
}
