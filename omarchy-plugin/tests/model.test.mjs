// Tests for Model.js. It is QML-flavoured JavaScript (`.pragma library`, no
// exports), so it is loaded by stripping the pragma and evaluating it.
// Run with: node tests/model.test.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import assert from "node:assert/strict";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "..", "Model.js"), "utf8").replace(/^\s*\.pragma\s+library\s*$/m, "");
const names = [...source.matchAll(/^function\s+([A-Za-z0-9_]+)\s*\(/gm)].map((m) => m[1]);
const M = new Function(`${source}\nreturn {${names.join(",")}, GLYPH_PLAYING, GLYPH_PAUSED, GLYPH_STOPPED};`)();

let failures = 0;
function check(name, fn) {
  try { fn(); console.log(`  ok   ${name}`); }
  catch (e) { failures++; console.log(`  FAIL ${name}\n       ${e.message}`); }
}

const omamp = { dbusName: "org.mpris.MediaPlayer2.omamp", identity: "Omamp", desktopEntry: "omamp" };
const second = { dbusName: "org.mpris.MediaPlayer2.omamp.instance4242", identity: "Omamp", desktopEntry: "omamp" };
const spotify = { dbusName: "org.mpris.MediaPlayer2.spotify", identity: "Spotify", desktopEntry: "spotify" };
const lookalike = { dbusName: "org.mpris.MediaPlayer2.omampire", identity: "Omampire", desktopEntry: "omampire" };

check("finds Omamp among other players", () => assert.equal(M.findOmamp([spotify, omamp]), omamp));
check("prefers the main bus name over a second copy", () => assert.equal(M.findOmamp([second, omamp]), omamp));
check("falls back to a second copy", () => assert.equal(M.findOmamp([spotify, second]), second));
check("ignores other players and lookalike names", () => assert.equal(M.findOmamp([spotify, lookalike]), null));
check("counts every copy", () => assert.equal(M.countOmamp([omamp, spotify, second]), 2));
check("no players, no Omamp", () => assert.equal(M.findOmamp(null), null));
check("identity alone is enough while the bus name is unknown", () => assert.ok(M.isOmamp({ identity: "Omamp" })));

check("artist and title", () => assert.equal(M.trackLine("Neon Coast", "Midnight Highway"), "Neon Coast – Midnight Highway"));
check("title only", () => assert.equal(M.trackLine("", "Untitled"), "Untitled"));
check("empty playlist", () => assert.equal(M.trackLine("", ""), ""));

check("clock under an hour", () => assert.equal(M.clock(83), "01:23"));
check("clock at zero and junk", () => { assert.equal(M.clock(0), "00:00"); assert.equal(M.clock(NaN), "00:00"); assert.equal(M.clock(-4), "00:00"); });
check("clock past an hour", () => assert.equal(M.clock(3725), "1:02:05"));
check("clock long minutes", () => assert.equal(M.clock(754.9), "12:34"));

check("state names", () => {
  assert.equal(M.stateName(false, false, false), "closed");
  assert.equal(M.stateName(true, true, false), "playing");
  assert.equal(M.stateName(true, false, true), "stopped");
  assert.equal(M.stateName(true, false, false), "paused");
});
check("state glyphs", () => {
  assert.equal(M.stateGlyph(true, false), M.GLYPH_PLAYING);
  assert.equal(M.stateGlyph(false, true), M.GLYPH_STOPPED);
  assert.equal(M.stateGlyph(false, false), M.GLYPH_PAUSED);
});

const nightRide = `mode = "dark"\naccent = "#ff0080"\ngreen = "#50ffb4"\ncolor2 = "#00ff00"\n`;
check("theme green from colors.toml", () => assert.equal(M.pickGreen(M.themeColors(nightRide)), "#50ffb4"));
check("color2 when the theme has no green", () => assert.equal(M.pickGreen(M.themeColors(`color2 = '#a6e3a1'`)), "#a6e3a1"));
check("nothing when neither exists", () => assert.equal(M.pickGreen(M.themeColors(`accent = "#ffffff"`)), ""));

check("tooltips", () => {
  assert.match(M.tooltip(false, "", "closed"), /not running/);
  assert.equal(M.tooltip(true, "A – B", "playing"), "A – B");
  assert.equal(M.tooltip(true, "A – B", "paused"), "A – B (paused)");
  assert.match(M.tooltip(true, "", "stopped"), /nothing in the playlist/);
});

check("covers: file, web and inline pictures pass", () => {
  assert.equal(M.coverSource("file:///home/u/.cache/omamp/art/ab-600.jpg"), "file:///home/u/.cache/omamp/art/ab-600.jpg");
  assert.equal(M.coverSource("https://example.org/c.jpg"), "https://example.org/c.jpg");
  assert.equal(M.coverSource("data:image/png;base64,AAAA"), "data:image/png;base64,AAAA");
});
check("covers: nothing else does", () => {
  assert.equal(M.coverSource(""), "");
  assert.equal(M.coverSource(undefined), "");
  assert.equal(M.coverSource("/home/u/cover.jpg"), "");
  assert.equal(M.coverSource("javascript:alert(1)"), "");
});

const art = "file:///home/u/.cache/omamp/art/ab-600.jpg";
const art2 = "file:///home/u/.cache/omamp/art/cd-600.jpg";
check("cover step: a new cover replaces the old at once", () => {
  assert.deepEqual(M.coverStep(art, art2, true), { cover: art2, hold: false });
  assert.deepEqual(M.coverStep("", art, true), { cover: art, hold: false });
});
check("cover step: a missing cover holds the old one for a moment", () => {
  assert.deepEqual(M.coverStep(art, "", true), { cover: art, hold: true });
  assert.deepEqual(M.coverStep(art, "/not/a/url.jpg", true), { cover: art, hold: true });
});
check("cover step: nothing to hold, or a closed player, clears it", () => {
  assert.deepEqual(M.coverStep("", "", true), { cover: "", hold: false });
  assert.deepEqual(M.coverStep(art, art2, false), { cover: "", hold: false });
});

check("cover mode: large, medium, hidden, and large for anything else", () => {
  assert.equal(M.coverMode("Large"), "large");
  assert.equal(M.coverMode("Medium"), "medium");
  assert.equal(M.coverMode(" hidden "), "hidden");
  assert.equal(M.coverMode(undefined), "large");
  assert.equal(M.coverMode("Huge"), "large");
});
check("panel cover size follows the mode", () => {
  assert.equal(M.panelCoverSize("Large", 320, 96), 320);
  assert.equal(M.panelCoverSize("Medium", 320, 96), 96);
  assert.equal(M.panelCoverSize("Medium", 80, 96), 80);
  assert.equal(M.panelCoverSize("Hidden", 320, 96), 0);
  assert.equal(M.panelCoverSize("Large", NaN, 96), 0);
});
check("bar thumbnail fits the bar", () => {
  assert.equal(M.thumbSize(26, 8), 18);
  assert.equal(M.thumbSize(30, 8), 22);
  assert.equal(M.thumbSize(14, 8), 12);
  assert.equal(M.thumbSize(60, 8), 28);
  assert.equal(M.thumbSize(undefined, 8), 18);
});

check("clamp01", () => { assert.equal(M.clamp01(1.4), 1); assert.equal(M.clamp01(-1), 0); assert.equal(M.clamp01("x"), 0); });

if (failures) { console.log(`\n${failures} failed`); process.exit(1); }
console.log("\nall passed");
