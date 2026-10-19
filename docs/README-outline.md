# Outline for the full README

The current README is deliberately short. Once advanced mode works, replace it
with one built on this outline. Each section names what it needs before it can
be written.

1. **Hero:** one line ("The music library manager for Omarchy"), a GIF of the
   deck switching to advanced mode, and badges (AUR version, licence, CI).
   *Needs:* advanced mode, a recorded GIF made from demo music, an AUR package.
2. **Why Omamp:** three short paragraphs. Omarchy and Hyprland first; library
   management that scales to big local and NAS libraries; the 2000s comforts.
3. **Screenshots:** the deck, the library tree with the column browser, the tag
   editor, the organise preview, the NAS setup, and the bar plugin. Two themes
   side by side to show theme following.
4. **Install:** AUR (`yay -S omamp`), the Omarchy package if it is accepted,
   building from source, and the bar plugin.
5. **Quick start:** add a library folder or a NAS share, let the scan finish,
   fix tags, build an auto-playlist.
6. **Features:** grouped as Play, Library, Tag, Organise, Playlists, NAS and
   network, Sync, Integrations. One line each, linking to docs/.
7. **Keys:** the full table, generated from the app's own shortcut list.
8. **NAS guide:** link to a page covering SMB versus NFS, the Navidrome route,
   and offline tracks.
9. **For agents and scripts:** the headless CLI (`omamp scan`, `omamp query`,
   `omamp tag`) with `--json`, and the bundled agent skill.
10. **Coming from MediaMonkey:** a table of MediaMonkey features and where each
    lives in Omamp, and how to import a MediaMonkey database (ratings, play
    counts, playlists).
11. **Contributing, licence and credits.**
