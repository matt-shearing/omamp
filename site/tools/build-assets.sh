#!/usr/bin/env bash
# Rebuilds the website's images, fonts and icons from the repository's own
# files. The results are committed, so the site itself needs no build step;
# run this only after the screenshots in docs/ or the icon change.
#
# Needs: ImageMagick 7 (magick, with AVIF and WebP), rsvg-convert, and
# Python's fontTools with brotli (for the WOFF2 fonts).
set -euo pipefail
cd "$(dirname "$0")/.."
root=..
img=img
mkdir -p "$img" fonts

# name:width-list. Wide screenshots get a full and a phone-sized copy.
wide=(hero-dark hero-light sidebar-dark sidebar-light tag-editor-dark tag-editor-light
      mixed-playlist-dark mixed-playlist-light radio-channels-dark radio-channels-light
      now-playing-big-dark now-playing-big-light bar-visualisers-dark bar-visualisers-light
      organise files-to-edit remote-source visualisers big-bar-visualiser-dark)
narrow=(deck-dark deck-light deck-classic deck-2000s omarchy-plugin-dark omarchy-plugin-light
        mini-player-dark mini-player-light micro-player-dark micro-player-light)

encode() { # src name width
  local src=$1 name=$2 w=$3
  magick "$src" -resize "${w}x>" -strip -quality 55 "$img/$name-$w.avif"
  magick "$src" -resize "${w}x>" -strip -quality 80 -define webp:method=6 "$img/$name-$w.webp"
}

for n in "${wide[@]}"; do
  src=$root/docs/$n.png
  full=$(magick identify -format %w "$src")
  encode "$src" "$n" "$full"
  encode "$src" "$n" 760
  # The PNG is only for browsers without AVIF or WebP, so the small one does.
  magick "$src" -resize 760x -strip -define png:compression-level=9 "$img/$n-760.png"
done
for n in "${narrow[@]}"; do
  src=$root/docs/$n.png
  w=$(magick identify -format %w "$src")
  encode "$src" "$n" "$w"
  magick "$src" -strip -define png:compression-level=9 "$img/$n-$w.png"
done

# Icons from the package icon.
cp "$root/pkgbuild/thrum.svg" favicon.svg
rsvg-convert -w 180 -h 180 "$root/pkgbuild/thrum.svg" -o apple-touch-icon.png
rsvg-convert -w 32 -h 32 "$root/pkgbuild/thrum.svg" -o favicon-32.png

# The social card: the dark hero screenshot under the name and the pitch.
rsvg-convert -w 132 -h 132 "$root/pkgbuild/thrum.svg" -o /tmp/thrum-og-icon.png
magick -size 1200x630 gradient:'#0b0d1a-#1a0f2e' \
  \( "$root/docs/hero-dark.png" -resize 1040x -bordercolor '#ff0080' -border 2 \) \
  -gravity northwest -geometry +120+330 -compose over -composite \
  /tmp/thrum-og-icon.png -geometry +64+56 -composite \
  -font "$HOME/.local/share/fonts/oq/JetBrainsMono-Bold.ttf" -fill '#f4f6fb' -pointsize 88 \
  -annotate +224+72 'Thrum' \
  -font "$HOME/.local/share/fonts/oq/Manrope-SemiBold.ttf" -fill '#c9d2f0' -pointsize 36 \
  -annotate +66+212 'A Winamp-style player and MediaMonkey-style library' \
  -annotate +66+258 'manager for Omarchy, Hyprland and Arch' \
  -strip -quality 88 og.jpg
rm -f /tmp/thrum-og-icon.png

# Fonts: Latin subsets of JetBrains Mono and Manrope (both SIL OFL 1.1).
fontdir=${FONTDIR:-$HOME/.local/share/fonts/oq}
sub() { python3 -m fontTools.subset "$fontdir/$1" --flavor=woff2 --layout-features='kern,liga,calt,tnum,zero' \
  --unicodes='U+0000-00FF,U+0131,U+0152-0153,U+02C6,U+02DA,U+02DC,U+2000-206F,U+2190-2193,U+2212,U+2215,U+25B6,U+25A0,U+2009' \
  --output-file="fonts/$2"; }
sub JetBrainsMono-Regular.ttf jetbrains-mono-400.woff2
sub JetBrainsMono-Bold.ttf jetbrains-mono-700.woff2
sub Manrope-Regular.ttf manrope-400.woff2
sub Manrope-SemiBold.ttf manrope-600.woff2
sub Manrope-ExtraBold.ttf manrope-800.woff2
echo "done: $(du -sh "$img" | cut -f1) of images"
