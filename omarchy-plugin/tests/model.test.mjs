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

check("clamp01", () => { assert.equal(M.clamp01(1.4), 1); assert.equal(M.clamp01(-1), 0); assert.equal(M.clamp01("x"), 0); });

if (failures) { console.log(`\n${failures} failed`); process.exit(1); }
console.log("\nall passed");
