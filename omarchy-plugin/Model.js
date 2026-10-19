.pragma library

// Pure helpers for the Omamp bar widget: which MPRIS player is Omamp, the
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

// The bus name is the reliable part: Omamp registers
// org.mpris.MediaPlayer2.omamp, or ...omamp.instanceNNN for a second copy.
// Identity and DesktopEntry are the fallback while the player is still
// reading its properties.
function isOmamp(player) {
  if (!player) return false
  var bus = String(player.dbusName || "")
  if (bus === "org.mpris.MediaPlayer2.omamp" || bus.indexOf("org.mpris.MediaPlayer2.omamp.") === 0) return true
  return String(player.desktopEntry || "") === "omamp" || String(player.identity || "") === "Omamp"
}

// The first Omamp copy wins, preferring the main bus name over an instance.
function findOmamp(players) {
  var list = players || []
  var fallback = null
  for (var i = 0; i < list.length; i++) {
    var p = list[i]
    if (!isOmamp(p)) continue
    if (String(p.dbusName || "") === "org.mpris.MediaPlayer2.omamp") return p
    if (!fallback) fallback = p
  }
  return fallback
}

// How many Omamp copies are on the bus; reported by `status` for checking.
function countOmamp(players) {
  var list = players || []
  var n = 0
  for (var i = 0; i < list.length; i++) if (isOmamp(list[i])) n++
  return n
}

// "Artist – Title", or the title alone, or nothing when the playlist is empty.
function trackLine(artist, title) {
  var a = String(artist || "").trim()
  var t = String(title || "").trim()
  if (a && t) return a + " – " + t
  return t || a
}

// mm:ss, or h:mm:ss past the hour, as Omamp's own clock shows it.
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
  if (!running) return "Omamp is not running. Click to open it."
  if (!line) return "Omamp: nothing in the playlist"
  var word = state === "playing" ? "" : " (" + state + ")"
  return line + word
}

// Theme colours by name from colors.toml, the way Omamp itself reads them.
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

// The cover to show: Omamp sends a file:// URL; other players may send
// http(s) or an inline picture. Anything else shows the placeholder.
function coverSource(url) {
  var s = String(url || "")
  return /^(file|https?):\/\//.test(s) || s.indexOf("data:image/") === 0 ? s : ""
}
