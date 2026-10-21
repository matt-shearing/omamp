// Takes full-page screenshots of the site in a headless browser, at desktop
// and phone widths in light and dark, into site/preview/ (git-ignored).
// Usage: python3 -m http.server 8765 -d site &   node site/tools/screenshots.mjs [url]
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";
const require = createRequire(import.meta.url);
let pw;
try { pw = require("playwright"); } catch { pw = require(process.env.PLAYWRIGHT_MODULE); }
const url = process.argv[2] || "http://127.0.0.1:8765/";
const out = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "preview");
const browser = await pw.chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || undefined });
const report = [];
for (const [label, width, height, mobile] of [["desktop", 1440, 900, false], ["phone", 375, 812, true]]) {
  for (const scheme of ["light", "dark"]) {
    const ctx = await browser.newContext({ viewport: { width, height }, colorScheme: scheme,
      deviceScaleFactor: mobile ? 2 : 1, isMobile: mobile, hasTouch: mobile, reducedMotion: "reduce" });
    const page = await ctx.newPage();
    const errors = [];
    page.on("console", (m) => m.type() === "error" && errors.push(m.text()));
    page.on("requestfailed", (r) => errors.push("failed: " + r.url()));
    await page.goto(url, { waitUntil: "networkidle" });
    // Scroll through the page so every lazy image loads, then wait for them.
    await page.evaluate(async () => {
      for (let y = 0; y < document.body.scrollHeight; y += 400) { window.scrollTo(0, y); await new Promise((r) => setTimeout(r, 60)); }
      window.scrollTo(0, 0);
    });
    await page.waitForLoadState("networkidle");
    await page.evaluate(() => Promise.all([...document.images].map((i) => i.decode().catch(() => 0))));
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    const file = path.join(out, `${label}-${scheme}.png`);
    await page.screenshot({ path: file, fullPage: true });
    await page.screenshot({ path: path.join(out, `${label}-${scheme}-top.png`) });
    report.push(`${file}  horizontal overflow: ${overflow}px  errors: ${errors.length ? errors.join("; ") : "none"}`);
    await ctx.close();
  }
}
await browser.close();
console.log(report.join("\n"));
