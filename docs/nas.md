# Music on a NAS

Omamp can manage a library that lives on a NAS in two ways. You can mount the
NAS's music share on this machine and scan it like a local folder, or, if the
NAS runs Navidrome, you can add Navidrome as a source and let it do the
scanning. Both can be used at once. This page explains how to choose, how to
mount a share so that it comes and goes cleanly, what Omamp does when the NAS
is off, and how fast to expect things to be.

Omamp never mounts anything itself and never runs `sudo`. It prints what to
run, and you run it.

## Choosing a route

| | Mounted share (SMB or NFS) | Navidrome on the NAS |
|---|---|---|
| What scans the files | Omamp, over the network | Navidrome, on the NAS itself |
| Rescan of 100,000 unchanged tracks | seconds to a minute (see below) | a few HTTP requests |
| Changes made on the NAS by other machines | found by scheduled quick rescans, every 15 minutes | Navidrome sees them at once |
| Tag editing, organising, duplicates by content | yes | no: Omamp only streams |
| Playback | the original file | a stream, original or transcoded |
| Phones and other clients | no | yes, any Subsonic app |
| Works with the NAS off | no (tracks show as offline) | no (tracks show as offline) |

Mount the share if you want to edit tags and organise files from Omamp. Add
Navidrome if the library is mostly for listening, if it is very large, or if
you also want it on your phone. With both, each album appears twice, once
from each source.

## SMB or NFS

Use **NFS** when the NAS offers NFSv4 and only Linux machines use the share.
It is simpler, it handles file metadata faster, and ownership maps cleanly
when your user id on the NAS matches the one here.

Use **SMB** when Windows or macOS machines use the same share, or when the
NAS's NFS support is poor. SMB 3.1.1 is encrypted and signed, and every NAS
from the last decade speaks it.

Neither tells Linux when another machine changes a file, so Omamp cannot watch
a share the way it watches a local folder. It rescans shares on a schedule
instead (see "Rescans" below).

## Mounting a share with systemd automount

`omamp nas mount-guide` prints two systemd units and the commands to install
them. An automount unit mounts the share the first time anything looks at the
folder, unmounts it after ten idle minutes, and never holds up boot when the
NAS is off.

For SMB:

```sh
omamp nas mount-guide --smb //nas/music --at /mnt/music --write-to ~/omamp-mount-units
```

For NFS:

```sh
omamp nas mount-guide --nfs nas:/volume1/music --at /mnt/music --write-to ~/omamp-mount-units
```

`--write-to` saves the unit files in that folder; without it, the guide only
prints them. `--uid` and `--gid` change the owner the files appear to have
(SMB only; the default is you), and `--idle` changes the idle timeout in
seconds. `--json` prints the units, an equivalent `/etc/fstab` line, and the
commands, for scripts.

The commands it prints for SMB are, in order:

1. Install `cifs-utils` (or `nfs-utils` for NFS).
2. Create `/etc/samba/credentials/<server>-<share>`, readable only by root,
   and put the NAS user name and password in it with `sudoedit`. The password
   never appears on a command line or in the unit.
3. Create the mount point.
4. Copy the two units into `/etc/systemd/system/`.
5. Reload systemd and enable the automount unit (not the mount unit).
6. Look at the folder once, which mounts it, then `omamp scan /mnt/music`.

The SMB options the guide uses, and why:

| Option | Why |
|---|---|
| `vers=3.1.1` | SMB 3.1.1: encryption and pre-authentication integrity. Use `vers=3.0` for an old NAS. |
| `credentials=…` | Keeps the password out of the unit and out of `ps`. |
| `uid=…,gid=…,forceuid,forcegid` | The files belong to you on this machine, whatever the NAS reports. |
| `noserverino` | The client makes up inode numbers. Some servers hand out unstable or colliding ones, which confuses programs that track files by inode. Omamp tracks files by path, so this is a safety setting. |
| `actimeo=30` | File attributes are cached for 30 seconds, which keeps rescans cheap. A change made on the NAS is visible here within 30 seconds. |
| `_netdev,nofail` | Wait for the network, and never fail boot over a missing share. |

For NFS the guide uses `vers=4.1,hard,noatime`. A `hard` mount retries for
ever instead of failing a write half way, which is what tag writing needs.
Its drawback is that a program touching the share while the NAS is off waits
until the NAS returns. Omamp itself never waits on a share (see the next
section).

## When the NAS is off

![Volumes: the local music folder, a share that isn't connected, and a Navidrome server](volumes.png)

Every library folder belongs to a volume. A share is recognised by a small
file, `.omamp-volume-id`, that Omamp writes at the folder you add (when the
share is writable). If the share is read-only, the server and share name
identify it instead. Because of this, a share mounted at a different path is
recognised without a rescan.

When a share cannot be reached, its volume is **offline**:

- Its tracks show as offline. They stay in playlists, keep their ratings and
  history, and are never marked missing or removed. Only a scan of a share
  that is online can mark a track missing.
- Omamp looks at a share with a time limit. A hung mount, which can make a
  plain `stat()` wait for minutes, costs at most 1.5 seconds the first time
  and nothing after that while it stays hung. A stale NFS handle (`ESTALE`)
  and an I/O error (`EIO`) also count as offline.
- When the share comes back, Omamp notices within 30 seconds (at once if the
  mount itself appears), marks its tracks online, and quick-rescans just that
  share. Both `omamp watch` and the window do this; the window's jobs bar
  says when a share is back and being rescanned.

`omamp volume list` shows each volume and why it is offline (`absent`,
`timed-out`, `stale`, `io-error`, `denied`). In the window, the Volumes page
(palette, Settings → Library, or the tree's Location menu) shows the same in
words, and **Connect a NAS…** there writes out the steps below for your
share.

### Moving to a new NAS

If you replace the NAS and copy the music across, the new share has the old
marker file (copied with the music) and Omamp finds it. If the marker was not
copied, or the share is read-only and was known by its server name, point the
old volume at the new place:

```sh
omamp volume list
omamp volume reassign 3 /mnt/newnas/music
```

Give the folder that corresponds to the old volume's root. Omamp checks that
the volume's tracks are there, writes the old marker onto the new share so it
is found next time, and brings the tracks back online with their ratings and
history. This is MediaMonkey's "re-assign drive".

`omamp volume forget <volume>` removes a volume and its tracks for good. It
refuses while the volume is online unless you add `--force`.

## The library database stays local

Omamp refuses to open a library database on a network share. SQLite relies on
file locks that SMB and NFS implement loosely or not at all, so a database
there can be corrupted by a second machine or a dropped connection. Keep the
library in `~/.local/share/omamp` and add the share's music folder to it.

## Rescans

Omamp compares each file's size and modification time with the database. A
file whose size and time are unchanged is never opened. On a share this makes
a rescan cost one listing per folder and one stat per file, each a round trip
to the NAS.

A **quick** rescan (`omamp scan --quick`, and the 15-minute schedule) also
skips the files of any folder whose own modification time is unchanged, so it
costs one listing per folder and one stat per subfolder. Adding, removing or
renaming a file changes its folder's time. Editing a file in place does not,
so every fourth scheduled rescan is a full one.

On SMB, folder times are reliable for files added, deleted or renamed through
any SMB client, but the Linux client caches them for `actimeo` seconds, so a
change shows up on the next rescan after that. Some NAS file systems update
folder times lazily; a full rescan catches what a quick one misses. This was
checked against the logic with simulated shares, not yet against a real NAS.

Omamp lists several folders at once to hide the round trips: 8 at a time on a
share and 4 on a local disk. Change it per volume with
`omamp volume workers <volume> <n>` (1 to 64; 0 restores the default).

## Performance expectations

Measured with `omamp_bench nas`, which walks a tree of 100,000 files in 11,001
folders (1,000 artists, 10 albums each, 10 tracks each) and waits a fixed time
for every listing and every stat:

| Rescan | Round trip | 1 walker | 4 | 8 | 16 |
|---|---|---:|---:|---:|---:|
| Full (stat every file) | 0.5 ms | 68 s | 17 s | 8.9 s | 4.8 s |
| Quick (folder times) | 0.5 ms | 12 s | 3.5 s | 2.0 s | 1.3 s |
| Full | 2 ms | | | 33 s | 18 s |
| Quick | 2 ms | | | 7.5 s | 4.8 s |

A full rescan makes 122,001 round trips and a quick one 22,001. Wired gigabit
to a NAS is typically 0.3 to 1 ms a round trip; Wi-Fi is 2 to 5 ms. These are
upper bounds: the Linux SMB and NFS clients fetch file attributes with the
folder listing and cache them, so many stats never reach the NAS. The end to
end benchmark (`omamp_bench files --latency-us 500 --walk-workers 8`, which
includes the database work) took 8.4 s for a full rescan and 1.4 s for a quick
one.

The first scan reads every file's tags, which over a network is limited by
throughput rather than round trips: expect roughly the time it takes to read
the first and last few hundred kilobytes of each file.

## Navidrome as a source

![Adding a Navidrome server, with the connection tested](remote-source.png)

If the NAS runs Navidrome (or another OpenSubsonic or Subsonic server), add it:

```sh
printf '%s\n' "$NAVIDROME_PASSWORD" | omamp remote add subsonic https://nas:4533 --user listener --password-stdin
omamp remote sync
```

An API key works the same way with `--api-key-stdin` and no `--user`; use one
if Navidrome offers it. Omamp never takes a password on the command line,
where it would land in your shell history.

The password or key is kept in the desktop keyring through `secret-tool` when
that is available, and otherwise in `~/.config/omamp/credentials.json`, which
only you can read (mode 0600). It is never stored in the library database.
Requests sign in with a salted token, so the password itself never crosses the
network.

A sync mirrors every song into the library as a track on a volume of kind
`subsonic`. Search, the browser, playlists, ratings, statistics and Auto-DJ
then treat them like files. The next sync lists the albums (500 per request)
and fetches only those that are new or changed, so an unchanged library costs
a few requests. `--full` fetches every album again. Songs removed from the
server become missing, and come back with their history if they reappear.
When the server cannot be reached, its tracks are offline.

```sh
omamp remote list
omamp remote set nas --max-bitrate 320 --format opus   # stream transcoded
omamp remote remove nas                                 # or --keep-tracks
```

Ratings and play counts on the server fill empty values in the library when
a song first syncs; after that the library's own values win.

## Commands

| Command | What it does |
|---|---|
| `omamp volume list` | Every volume, online or not, and why |
| `omamp volume show <volume>` | One volume in detail |
| `omamp volume reassign <volume> <folder>` | Point a volume at its music's new place |
| `omamp volume forget <volume> [--force]` | Remove a volume and its tracks |
| `omamp volume workers <volume> <n>` | Folders listed at once when scanning it |
| `omamp nas mount-guide --smb //server/share --at <folder>` | Print systemd automount units for SMB |
| `omamp nas mount-guide --nfs server:/export --at <folder>` | The same for NFS |
| `omamp remote add subsonic <url> …` | Add a Navidrome or Subsonic server |
| `omamp remote list`, `sync`, `set`, `remove` | Look after remote sources |

A volume is named by its id, its key, its label, or any path inside it. Every
command takes `--json`.
