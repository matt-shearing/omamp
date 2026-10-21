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

const thrum = { dbusName: "org.mpris.MediaPlayer2.thrum", identity: "Thrum", desktopEntry: "thrum" };
const second = { dbusName: "org.mpris.MediaPlayer2.thrum.instance4242", identity: "Thrum", desktopEntry: "thrum" };
const spotify = { dbusName: "org.mpris.MediaPlayer2.spotify", identity: "Spotify", desktopEntry: "spotify" };
const lookalike = { dbusName: "org.mpris.MediaPlayer2.thrumire", identity: "Thrumire", desktopEntry: "thrumire" };

check("finds Thrum among other players", () => assert.equal(M.findThrum([spotify, thrum]), thrum));
check("prefers the main bus name over a second copy", () => assert.equal(M.findThrum([second, thrum]), thrum));
check("falls back to a second copy", () => assert.equal(M.findThrum([spotify, second]), second));
check("ignores other players and lookalike names", () => assert.equal(M.findThrum([spotify, lookalike]), null));
check("counts every copy", () => assert.equal(M.countThrum([thrum, spotify, second]), 2));
check("no players, no Thrum", () => assert.equal(M.findThrum(null), null));
check("identity alone is enough while the bus name is unknown", () => assert.ok(M.isThrum({ identity: "Thrum" })));

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
  assert.equal(M.coverSource("file:///home/u/.cache/thrum/art/ab-600.jpg"), "file:///home/u/.cache/thrum/art/ab-600.jpg");
  assert.equal(M.coverSource("https://example.org/c.jpg"), "https://example.org/c.jpg");
  assert.equal(M.coverSource("data:image/png;base64,AAAA"), "data:image/png;base64,AAAA");
});
check("covers: nothing else does", () => {
  assert.equal(M.coverSource(""), "");
  assert.equal(M.coverSource(undefined), "");
  assert.equal(M.coverSource("/home/u/cover.jpg"), "");
  assert.equal(M.coverSource("javascript:alert(1)"), "");
});

const art = "file:///home/u/.cache/thrum/art/ab-600.jpg";
const art2 = "file:///home/u/.cache/thrum/art/cd-600.jpg";
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

// ------------------------------------------------------------ the visualiser
check("the feed's socket is under the runtime folder", () => {
  assert.equal(M.vizSocketPath("/run/user/1000"), "/run/user/1000/thrum-levels.sock");
  assert.equal(M.vizSocketPath("/run/user/1000/"), "/run/user/1000/thrum-levels.sock");
  assert.equal(M.vizSocketPath(""), "");
});
check("a frame parses, with levels as 0..1", () => {
  const f = M.parseVizFrame('{"style":"led","reduceMotion":true,"active":true,"levels":[0,50,100,140]}');
  assert.equal(f.style, "led");
  assert.equal(f.reduceMotion, true);
  assert.equal(f.active, true);
  assert.deepEqual(f.levels, [0, 0.5, 1, 1]);
});
check("an idle frame has no levels and is not active", () => {
  const f = M.parseVizFrame('{"style":"bars","reduceMotion":false,"active":false,"levels":[]}');
  assert.equal(f.active, false);
  assert.deepEqual(f.levels, []);
  assert.equal(M.parseVizFrame('{"active":true,"levels":[]}').active, false);
});
check("junk is not a frame", () => {
  assert.equal(M.parseVizFrame("not json"), null);
  assert.equal(M.parseVizFrame("[1,2]"), null);
  assert.equal(M.parseVizFrame(""), null);
  assert.equal(M.parseVizFrame('{"levels":"x"}').levels.length, 0);
});
check("the style setting", () => {
  assert.equal(M.vizChoice("Same as Thrum"), "same");
  assert.equal(M.vizChoice("Bars"), "bars");
  assert.equal(M.vizChoice("Line"), "line");
  assert.equal(M.vizChoice(undefined), "same");
});
check("every Thrum style has a small one, and unknown ones are bars", () => {
  const want = { bars: "bars", peaks: "bars", fire: "bars", mirror: "skyline", skyline: "skyline",
    scope: "ribbon", ribbon: "ribbon", line: "line", pulse: "pulse", led: "led", synthwave: "synthwave", stars: "stars" };
  for (const [thrum, mini] of Object.entries(want)) assert.equal(M.miniStyle(thrum, "Same as Thrum"), mini, thrum);
  assert.equal(M.miniStyle("hologram", "Same as Thrum"), "bars");
  assert.equal(M.miniStyle("", "Same as Thrum"), "bars");
});
check("a chosen style overrides Thrum's", () => {
  assert.equal(M.miniStyle("synthwave", "Bars"), "bars");
  assert.equal(M.miniStyle("synthwave", "Line"), "line");
});
check("subtle is fainter than normal, and busy styles fainter still", () => {
  assert.ok(M.vizOpacity("Subtle", "line") < M.vizOpacity("Normal", "line"));
  assert.ok(M.vizOpacity("Subtle", "bars") < M.vizOpacity("Subtle", "line"));
  assert.ok(M.vizOpacity("Normal", "synthwave") <= 0.6);
  assert.equal(M.vizOpacity(undefined, "line"), M.vizOpacity("Subtle", "line"));
});
check("frames faster than the limit are dropped", () => {
  assert.equal(M.frameDue(0, 1000, 40), true);
  assert.equal(M.frameDue(1000, 1020, 40), false);
  assert.equal(M.frameDue(1000, 1040, 40), true);
  assert.equal(M.frameDue(1000, 500, 40), true); // the clock went back
});
check("easing towards the new levels", () => {
  assert.deepEqual(M.easeLevels([0, 1], [1, 0], 1), [1, 0]);
  assert.deepEqual(M.easeLevels([0, 1], [1, 0], 0.5), [0.5, 0.5]);
  assert.deepEqual(M.easeLevels([], [1], 0.5), [0.5]);
});
check("resampling keeps the ends and interpolates between", () => {
  assert.deepEqual(M.resample([0, 1], 3), [0, 0.5, 1]);
  assert.deepEqual(M.resample([], 2), [0, 0]);
  assert.deepEqual(M.resample([0.4], 2), [0.4, 0.4]);
  assert.equal(M.resample([0, 1], 0).length, 0);
});
check("bars fit the width, centred, rising from the bottom", () => {
  const r = M.barRects([1, 0], 20, 10, 3, 2, false);
  assert.equal(r.length, 4); // 4*3 + 3*2 = 18 <= 20
  assert.equal(r[0].x, 1);
  assert.equal(r[0].h, 10);
  assert.equal(r[0].y, 0);
  assert.equal(r[3].h, 1); // silence still shows a hairline
  assert.ok(r[3].x + r[3].w <= 20);
  assert.deepEqual(M.barRects([1], 0, 10, 3, 2, false), []);
});
check("the skyline puts the bass in the middle", () => {
  const r = M.barRects([1, 0, 0, 0], 50, 10, 3, 2, true);
  const mid = Math.floor(r.length / 2);
  assert.ok(r[mid].h > r[0].h);
  assert.equal(r[0].h, r[r.length - 1].h);
});
check("the bass is the loudest of the lowest quarter", () => {
  assert.equal(M.bass([0.2, 0.8, 0, 0, 0, 0, 0, 0]), 0.8);
  assert.equal(M.bass([]), 0);
});
check("the star field is the same for the same seed", () => {
  assert.deepEqual(M.starField(5, 3), M.starField(5, 3));
  assert.notDeepEqual(M.starField(5, 3), M.starField(5, 4));
  for (const s of M.starField(20, 9)) { assert.ok(s.x >= 0 && s.x < 1); assert.ok(s.y >= 0 && s.y < 1); }
});

if (failures) { console.log(`\n${failures} failed`); process.exit(1); }
console.log("\nall passed");
