# The Omamp website

A one-page website for Omamp. It is plain HTML and CSS with a few lines of
JavaScript for the Copy buttons, and it needs no build step: serve this
folder as it is.

| File | What it is |
| --- | --- |
| `index.html` | The page |
| `style.css` | All the styles, with the colours as tokens at the top |
| `copy.js` | Adds a Copy button to each command block; the page works without it |
| `img/` | The screenshots from `docs/`, as AVIF and WebP with a PNG for old browsers |
| `fonts/` | Latin subsets of Manrope and JetBrains Mono, served from here so no font service is involved |
| `og.jpg` | The picture shown when the link is shared |
| `favicon.svg`, `favicon-32.png`, `apple-touch-icon.png` | Icons made from `pkgbuild/omamp.svg` |
| `tools/` | The asset builder and the screenshot script; not published |

The page has no analytics, no trackers and no third-party requests.

## Preview it

```sh
python3 -m http.server 8765 --bind 127.0.0.1 -d site
```

Then open http://127.0.0.1:8765/. To take full-page screenshots at desktop
and phone widths in light and dark, without opening a window:

```sh
CHROMIUM_PATH=/usr/bin/chromium node site/tools/screenshots.mjs
```

They land in `site/preview/`, which git ignores. If Node cannot find
Playwright, set `PLAYWRIGHT_MODULE` to its folder.

## After the screenshots change

The images, icons, social picture and fonts are built from the repository and
committed. When the screenshots in `docs/` or the icon change, rebuild them:

```sh
site/tools/build-assets.sh
```

It needs ImageMagick 7 with AVIF and WebP, `rsvg-convert`, and Python's
fontTools with brotli. The fonts come from `~/.local/share/fonts/oq` unless
`FONTDIR` says otherwise.

Each screenshot appears in `index.html` as a `<picture>` with a dark and a
light source, so it follows the visitor's light or dark setting. Copy an
existing one when adding another.

## Publishing

`.github/workflows/pages.yml` publishes this folder to GitHub Pages. It runs
only when started by hand, and only after Pages is switched on for the
repository, so merging it changes nothing in public. The planned address is
`https://omamp.tetelestai.dev/`, which is also written into the page's
canonical link and social tags; change those three places in `index.html` if
the site moves.
