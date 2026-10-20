# Writes made-up spotify_player CLI output, shaped exactly as spotify_player
# 0.25.1 serialises it (state/model.rs and rspotify-model 0.16.1).
import json, os, hashlib, sys
out = sys.argv[1]
def sid(seed):
    h = hashlib.sha256(seed.encode()).digest()
    a = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    n = int.from_bytes(h, 'big'); s = ""
    for _ in range(22): s += a[n % 62]; n //= 62
    return s
artists = {n: sid("artist"+n) for n in ["Neon Coast","Midnight Arcade","Velvet Static","Chrome Harbor","Sunset Protocol","Lunar Drive"]}
albums = [("Afterglow Avenue","Neon Coast","2019-06-14"),("Pixel Rain","Midnight Arcade","2021-03-05"),
          ("Static Bloom","Velvet Static","2018-11-02"),("Harbor Lights","Chrome Harbor","2020-08-21"),
          ("Overdrive Hearts","Sunset Protocol","2022-02-11"),("Night Ride Home","Lunar Drive","2023-09-29")]
alb = {a[0]: {"id": sid("album"+a[0]), "artist": a[1], "date": a[2]} for a in albums}
tracklist = {
 "Afterglow Avenue": [("Coastline at 2 AM",232),("Afterglow Avenue",251),("Palm Shadows",198),("Saltwater Neon",244)],
 "Pixel Rain": [("Pixel Rain",219),("Insert Coin",187),("High Score Heart",226)],
 "Static Bloom": [("Static Bloom",263),("Velvet Signal",241),("Cassette Moon",208)],
 "Harbor Lights": [("Harbor Lights",255),("Container Sunrise",230),("Foghorn Dreams",271)],
 "Overdrive Hearts": [("Overdrive Hearts",214),("Redline Romance",199),("Turbo Twilight",236)],
 "Night Ride Home": [("Night Ride Home",284),("Tail Lights",221),("Last Exit Glow",249)],
}
def artist(n): return {"id": artists[n], "name": n}
def album_obj(t):  # spotify_player Album
    a = alb[t]; return {"id": a["id"], "release_date": a["date"], "name": t, "artists": [artist(a["artist"])], "typ": "album", "added_at": 0}
tracks = []
for t, ts in tracklist.items():
    for name, secs in ts:
        tracks.append({"id": sid("track"+name), "name": name, "artists": [artist(alb[t]["artist"])], "album": album_obj(t),
                       "duration": {"secs": secs, "nanos": 0}, "explicit": False})
byname = {t["name"]: t for t in tracks}
def full(t):  # rspotify FullTrack, as in the playback state
    a = alb[t["album"]["name"]]
    return {"album": {"album_type": "album", "artists": [{"external_urls": {"spotify": "https://open.spotify.com/artist/"+artists[a["artist"]]}, "href": None, "id": artists[a["artist"]], "name": a["artist"]}],
                      "external_urls": {"spotify": "https://open.spotify.com/album/"+a["id"]}, "href": None, "id": a["id"],
                      "images": [{"height": 640, "url": "http://art.invalid/album-%s-640.jpg" % a["id"], "width": 640},
                                 {"height": 300, "url": "http://art.invalid/album-%s-300.jpg" % a["id"], "width": 300},
                                 {"height": 64, "url": "http://art.invalid/album-%s-64.jpg" % a["id"], "width": 64}],
                      "name": t["album"]["name"], "release_date": a["date"], "release_date_precision": "day"},
            "artists": [{"external_urls": {}, "href": None, "id": artists[a["artist"]], "name": a["artist"]}],
            "disc_number": 1, "duration_ms": t["duration"]["secs"]*1000, "explicit": False,
            "external_ids": {"isrc": "QZXXX" + t["id"][:7]}, "external_urls": {"spotify": "https://open.spotify.com/track/"+t["id"]},
            "href": None, "id": t["id"], "is_local": False, "name": t["name"], "popularity": 0, "preview_url": None,
            "track_number": 1, "type": "track"}
def pl(name, owner, uid, desc, ids):
    return {"id": sid("pl"+name), "collaborative": False, "name": name, "owner": [owner, uid], "desc": desc, "current_folder_id": 0, "snapshot_id": "AAAA"+sid("snap"+name)[:10]}, ids
playlists = [pl("Late Night Drive","Matt Example","mattexample","For the long way home",["Coastline at 2 AM","Night Ride Home","Tail Lights","Harbor Lights","Turbo Twilight"]),
             pl("Focus Synth","Matt Example","mattexample","",["Static Bloom","Velvet Signal","Pixel Rain"]),
             pl("Gym Runs","Matt Example","mattexample","Fast ones",["Overdrive Hearts","Redline Romance","Insert Coin"]),
             pl("Synthwave Essentials","Spotify","spotify","The essential tracks.\nUpdated weekly.",["Afterglow Avenue","Pixel Rain","Harbor Lights"]),
             pl("Retro Futures","Kai Rivers","kairivers","Chrome and\nneon",["Cassette Moon","Last Exit Glow"])]
def cli(obj):  # what the CLI prints: serde_json, then "\\n" -> newline
    return json.dumps(obj, separators=(",",":"), ensure_ascii=False).replace("\\n", "\n") + "\n"
def w(path, obj):
    with open(os.path.join(out, path), "w") as f: f.write(cli(obj))
w("playlists.json", [p for p, _ in playlists])
liked = ["Coastline at 2 AM","Pixel Rain","Velvet Signal","Harbor Lights","Overdrive Hearts","Night Ride Home","Cassette Moon","Tail Lights"]
w("liked.json", [byname[n] for n in liked])
w("albums.json", [dict(album_obj(a[0]), added_at=1700000000+i) for i, a in enumerate(albums)])
w("artists.json", [artist(n) for n in artists])
w("top.json", [byname[n] for n in liked[:5]])
for p, ids in playlists:
    w("items/playlist-%s.json" % p["id"], {"playlist": p, "tracks": [byname[n] for n in ids]})
for a in albums:
    w("items/album-%s.json" % alb[a[0]]["id"], {"album": album_obj(a[0]), "tracks": [dict(t, album=None) for t in tracks if t["album"]["name"] == a[0]]})
for n, i in artists.items():
    mine = [t for t in tracks if t["artists"][0]["name"] == n]
    w("items/artist-%s.json" % i, {"artist": artist(n), "top_tracks": mine, "albums": [album_obj(a[0]) for a in albums if a[1] == n], "related_artists": []})
for t in tracks:
    w("items/track-%s.json" % t["id"], t)
    with open(os.path.join(out, "tracks/full-%s.json" % t["id"]), "w") as f: f.write(json.dumps(full(t), separators=(",",":")))
w("search.json", {"tracks": [byname[n] for n in ["Coastline at 2 AM","Saltwater Neon","Afterglow Avenue","Palm Shadows","Night Ride Home"]],
                  "artists": [artist("Neon Coast"), artist("Lunar Drive")],
                  "albums": [album_obj("Afterglow Avenue"), album_obj("Night Ride Home")],
                  "playlists": [playlists[0][0], playlists[3][0]], "shows": [], "episodes": []})
w("devices.json", [{"id": "d1f00dfeedbeef0000000000000000000000000a", "is_active": True, "is_private_session": False, "is_restricted": False, "name": "spotify-player", "type": "Speaker", "volume_percent": 70},
                   {"id": "d1f00dfeedbeef0000000000000000000000000b", "is_active": False, "is_private_session": False, "is_restricted": False, "name": "Kitchen speaker", "type": "Speaker", "volume_percent": 40}])
with open(os.path.join(out, "ids.txt"), "w") as f:
    for t in tracks: f.write("track\t%s\t%s\n" % (t["name"], t["id"]))
    for a in albums: f.write("album\t%s\t%s\n" % (a[0], alb[a[0]]["id"]))
    for p, _ in playlists: f.write("playlist\t%s\t%s\n" % (p["name"], p["id"]))
    for n, i in artists.items(): f.write("artist\t%s\t%s\n" % (n, i))
# A second search, for the screenshots: the fake answers `search lights`
# from search-lights.json.
w("search-lights.json", {"tracks": [byname[n] for n in ["Harbor Lights","Tail Lights","Last Exit Glow","Container Sunrise"]],
                         "artists": [artist("Chrome Harbor"), artist("Lunar Drive")],
                         "albums": [album_obj("Harbor Lights"), album_obj("Night Ride Home")],
                         "playlists": [playlists[0][0]], "shows": [], "episodes": []})
