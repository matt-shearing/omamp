# The Omamp AUR package

This folder holds the files the AUR needs for Omamp. Nothing here has been published.

| File | What it is |
| --- | --- |
| `PKGBUILD` | Builds Omamp 0.2.0 from the GitHub tag's source tarball |
| `.SRCINFO` | The metadata the AUR reads, generated from `PKGBUILD` |
| `.gitignore` | Keeps build output out of git |

The package installs the `omamp` binary, the desktop entry, the icon, the licence, the README, and the
Omarchy bar plugin in `/usr/share/omamp/omarchy-plugin/`. The plugin is per-user, so each user links it
with `/usr/share/omamp/omarchy-plugin/install` and then runs `omarchy-restart-shell`.

## The name is free

On 2026-10-20 the AUR had no package called `omamp`, and a search for "omamp" found nothing.

## Which version to publish

`PKGBUILD` builds 0.2.0, the current release. Device sync and reports (Phase 6) arrive after it, so
it may be better to tag 0.3.0 once they are merged and publish that instead; the steps under "The next
release" below cover it.

## Before publishing: push the tag

The `v0.2.0` tag exists in your local checkout but not on GitHub, so the tarball URL in `PKGBUILD`
returns 404 today. The commit itself (`ee538bd`) is on GitHub's `main`.

The checksum in `PKGBUILD` is for a local `git archive` of the tag with the same contents. GitHub
compresses its tarballs itself, so its checksum will almost certainly differ. After pushing the tag,
refresh it:

```sh
cd ~/dev/omamp && git push origin v0.2.0
cd ~/dev/omamp/aur
updpkgsums                              # downloads the real tarball and rewrites sha256sums
makepkg --printsrcinfo > .SRCINFO
```

## How it was checked

It was built with `makepkg -f` in a scratch folder, and the built binary reports `omamp 0.2.0`. It was
not installed.

`namcap PKGBUILD` reports nothing. `namcap` on the built package reports four warnings, all expected:

```
omamp W: Referenced QML module 'qs.Commons' is an uninstalled dependency (...omarchy-plugin/Widget.qml, Service.qml)
omamp W: Referenced QML module 'qs.Ui' is an uninstalled dependency (...omarchy-plugin/Widget.qml)
omamp W: Dependency quickshell detected but optional (...omarchy-plugin/Widget.qml, Service.qml)
omamp W: Dependency included, but may not be needed ('ttf-jetbrains-mono-nerd')
```

- `qs.Commons` and `qs.Ui` come from the Omarchy shell itself, which no package provides.
- Quickshell is needed only by the bar plugin, so it is an optional dependency.
- The JetBrains Mono Nerd font is named in the QML (the deck's glyphs), which namcap cannot see.

## Dependencies

The binary links Qt 6 (base, declarative, multimedia), TagLib, SQLite and FFmpeg's libraries; those,
glibc and the GCC runtime are the hard dependencies. Omamp starts these programs only if they are
present, so they are optional: `fpcalc` (chromaprint) for duplicates and identification, `secret-tool`
(libsecret) for NAS passwords, `xdg-open`, `hyprctl`, and Quickshell for the bar plugin. Device sync also
runs the `ffmpeg` command to convert files, which the `ffmpeg` dependency already provides.

There is no `check()`. The test suite builds a separate binary, generates audio fixtures with the
`ffmpeg` command, and several of its tests open windows, so it does not belong in a clean package build.

## Publishing to the AUR

Once, if you have no AUR account key yet:

1. Make a key: `ssh-keygen -t ed25519 -f ~/.ssh/aur -C aur`.
2. Paste `~/.ssh/aur.pub` into your account at https://aur.archlinux.org (My Account > SSH Public Key).
3. Add to `~/.ssh/config`:

   ```
   Host aur.archlinux.org
     IdentityFile ~/.ssh/aur
     User aur
   ```

Then, after pushing the tag and refreshing the checksum (above):

```sh
git clone ssh://aur@aur.archlinux.org/omamp.git ~/dev/omamp-aur
cp ~/dev/omamp/aur/PKGBUILD ~/dev/omamp/aur/.SRCINFO ~/dev/omamp-aur/
cd ~/dev/omamp-aur
makepkg -f                              # optional last check against the real tarball
git add PKGBUILD .SRCINFO
git commit -m "omamp 0.2.0"
git push
```

Cloning a name that does not exist yet gives an empty repository, and the first push creates the package.

## The next release

1. Tag the release and push the tag, e.g. `git tag v0.3.0 && git push origin v0.3.0`.
2. In `PKGBUILD`, set `pkgver=0.3.0` and `pkgrel=1`. Raise only `pkgrel` when the packaging changes but
   Omamp does not.
3. Run `updpkgsums`, then `makepkg --printsrcinfo > .SRCINFO`.
4. Build once with `makepkg -f`, then copy both files into the AUR clone, commit and push.
