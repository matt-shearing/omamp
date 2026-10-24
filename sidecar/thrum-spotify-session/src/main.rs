//! thrum-spotify-session: Thrum's one librespot session. It reads what
//! Spotify won't let a new Web API app read (the tracks of Spotify's own
//! playlists, an artist's top tracks) the way Spotify's own apps do, decodes
//! Spotify's tracks for Thrum to play, saves tracks to Liked Songs, and can
//! make Thrum a Spotify Connect device.
//!
//! It signs in with a librespot credential that already exists (by default
//! spotify_player's, ~/.cache/spotify-player/credentials.json), which it only
//! reads, and never stores a new one. Its requests go to Spotify's private
//! spclient endpoints:
//!
//!   playlist     GET  /playlist/v2/playlist/<id>?decorate=…&from=…&length=…
//!   track names  POST /extended-metadata/v0/extended-metadata (TRACK_V4)
//!   artist       POST /extended-metadata/v0/extended-metadata (ARTIST_V4)
//!   album        POST /extended-metadata/v0/extended-metadata (ALBUM_V4)
//!   Liked Songs  POST /collection/v2/write, /collection/v2/contains
//!
//! These have no per-app quota, so they don't meet the Web API's rate
//! limits. Spotify's developer terms don't cover them, and Spotify can
//! change them. Liked Songs falls back to the Web API (/v1/me/tracks) with
//! the session's own token.
//!
//! `thrum-spotify-session [--connect <name>] serve` is what Thrum runs: one
//! JSON request a line on stdin, frames on stdout (audio_protocol.rs, and
//! docs/spotify.md). `audio` is the same, by its old name. With --connect
//! the session is also a Spotify Connect device called <name> (connect.rs).
//!
//! One-shot, for a person or a test, each printing one JSON line:
//! thrum-spotify-session playlist <id>, artist-top <id>, album <id>,
//! liked <track id>, like <track id>, unlike <track id>, or ping.

mod audio;
mod audio_protocol;
mod connect;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use librespot_core::authentication::Credentials;
use librespot_core::config::SessionConfig;
use librespot_core::session::Session;
use librespot_core::spotify_id::SpotifyId;
use librespot_core::spotify_uri::SpotifyUri;
use librespot_protocol::extended_metadata::{BatchedEntityRequest, EntityRequest, ExtensionQuery};
use librespot_protocol::extension_kind::ExtensionKind;
use librespot_protocol::playlist4_external::SelectedListContent;
use protobuf::{EnumOrUnknown, Message as _};
use serde_json::{json, Value};

/// How many tracks one metadata request asks about.
const BATCH: usize = 100;
/// How many playlist rows one read asks for.
const WINDOW: u32 = 500;

#[derive(Debug)]
struct Failure {
    message: String,
    status: Option<u16>,
}

impl Failure {
    fn new(message: impl Into<String>) -> Self {
        Self { message: message.into(), status: None }
    }
}

impl From<librespot_core::Error> for Failure {
    fn from(e: librespot_core::Error) -> Self {
        use librespot_core::error::ErrorKind;
        let status = match e.kind {
            ErrorKind::NotFound => Some(404),
            ErrorKind::PermissionDenied => Some(403),
            ErrorKind::Unauthenticated => Some(401),
            ErrorKind::ResourceExhausted => Some(429),
            _ => None,
        };
        Self { message: e.to_string(), status }
    }
}

impl From<protobuf::Error> for Failure {
    fn from(e: protobuf::Error) -> Self {
        Failure::new(format!("unreadable answer from Spotify: {e}"))
    }
}

fn default_credentials() -> PathBuf {
    let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_default();
    home.join(".cache/spotify-player/credentials.json")
}

fn usage() -> ! {
    eprintln!(
        "usage: thrum-spotify-session [--credentials <file>] [--connect <name>] \
         [serve | audio | ping | playlist <id> | artist-top <id> | album <id> | liked|like|unlike <track id>]"
    );
    std::process::exit(2);
}

/// Takes `--<name> <value>` out of args.
fn option(args: &mut Vec<String>, name: &str) -> Option<String> {
    let at = args.iter().position(|a| a == name)?;
    if at + 1 >= args.len() {
        usage();
    }
    let value = args.remove(at + 1);
    args.remove(at);
    Some(value)
}

#[tokio::main]
async fn main() {
    let mut args = std::env::args().skip(1).collect::<Vec<_>>();
    let credentials = option(&mut args, "--credentials").map(PathBuf::from).unwrap_or_else(default_credentials);
    let connect_name = option(&mut args, "--connect").filter(|n| !n.trim().is_empty());
    let one_shot = match args.first().map(String::as_str) {
        None | Some("serve") | Some("audio") => None,
        Some("ping") => Some(json!({"id": 0, "cmd": "ping"})),
        Some("playlist") if args.len() == 2 => Some(json!({"id": 0, "cmd": "playlist", "playlist": args[1]})),
        Some("artist-top") if args.len() == 2 => Some(json!({"id": 0, "cmd": "artist-top", "artist": args[1]})),
        Some("album") if args.len() == 2 => Some(json!({"id": 0, "cmd": "album", "album": args[1]})),
        Some("liked") if args.len() == 2 => Some(json!({"id": 0, "cmd": "liked", "tracks": [args[1]]})),
        Some(verb @ ("like" | "unlike")) if args.len() == 2 => {
            Some(json!({"id": 0, "cmd": "like", "tracks": [args[1]], "on": verb == "like"}))
        }
        _ => usage(),
    };

    if let Some(request) = one_shot {
        let session = match connect(&credentials).await {
            Ok(s) => s,
            Err(f) => {
                println!("{}", json!({"ready": false, "error": f.message}));
                std::process::exit(1);
            }
        };
        let answer = handle(&session, &request).await;
        println!("{answer}");
        std::process::exit(if answer["ok"] == true { 0 } else { 1 });
    }

    let out = audio::Out::new();
    let started = match &connect_name {
        Some(name) => start_device(&credentials, name, out.clone()).await.map(|(s, d)| (s, Some(d))),
        None => connect(&credentials).await.map(|s| (s, None)),
    };
    let (session, device) = match started {
        Ok(started) => started,
        Err(f) => {
            out.send(&json!({"ready": false, "error": f.message}), &[]);
            std::process::exit(1);
        }
    };
    let mut ready = json!({"ready": true, "user": session.username(), "country": session.country(),
                           "rate": audio_protocol::RATE, "channels": audio_protocol::CHANNELS});
    if let Some(name) = &connect_name {
        ready["connect"] = json!(name);
    }
    if out.send(&ready, &[]) {
        let (s, runtime, d) = (session.clone(), tokio::runtime::Handle::current(), device.clone());
        let _ = tokio::task::spawn_blocking(move || audio::serve(s, runtime, out, d)).await;
    }
    if let Some(d) = &device {
        d.shutdown();
        // Spirc says goodbye to Spotify, so phones stop listing the device.
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    }
    session.shutdown();
    // Streams still opening hold the runtime; nothing more is wanted.
    std::process::exit(0);
}

fn read_credentials(path: &PathBuf) -> Result<Credentials, Failure> {
    let text = std::fs::read_to_string(path)
        .map_err(|e| Failure::new(format!("can't read the Spotify sign-in at {}: {e}", path.display())))?;
    serde_json::from_str(&text)
        .map_err(|e| Failure::new(format!("the Spotify sign-in at {} isn't one librespot can use: {e}", path.display())))
}

/// Signs in as a Spotify Connect device called name.
async fn start_device(path: &PathBuf, name: &str, out: audio::Out) -> Result<(Session, Arc<connect::Device>), Failure> {
    let credentials = read_credentials(path)?;
    let session = Session::new(SessionConfig::default(), None);
    let started = tokio::time::timeout(
        std::time::Duration::from_secs(20),
        connect::Device::start(name, session.clone(), credentials, out),
    )
    .await
    .map_err(|_| Failure::new("Spotify didn't answer the sign-in within 20 seconds"))??;
    let (device, task) = started;
    tokio::spawn(task);
    Ok((session, Arc::new(device)))
}

/// Signs in with the stored credential, without storing anything.
async fn connect(path: &PathBuf) -> Result<Session, Failure> {
    let credentials = read_credentials(path)?;
    let session = Session::new(SessionConfig::default(), None);
    tokio::time::timeout(std::time::Duration::from_secs(20), session.connect(credentials, false))
        .await
        .map_err(|_| Failure::new("Spotify didn't answer the sign-in within 20 seconds"))??;
    Ok(session)
}

/// A catalogue request's answer, with its id and "ok".
pub(crate) async fn handle(session: &Session, request: &Value) -> Value {
    let id = request["id"].clone();
    let result = match request["cmd"].as_str() {
        Some("ping") => Ok(json!({"user": session.username(), "country": session.country()})),
        Some("playlist") => {
            let offset = request["offset"].as_u64().unwrap_or(0) as u32;
            let length = request["length"].as_u64().unwrap_or(0) as u32;
            playlist(session, request["playlist"].as_str().unwrap_or(""), offset, length).await
        }
        Some("artist-top") => artist_top(session, request["artist"].as_str().unwrap_or("")).await,
        Some("album") => album(session, request["album"].as_str().unwrap_or("")).await,
        Some("like") => {
            let on = request["on"].as_bool().unwrap_or(true);
            like(session, &ids_in(request), on).await
        }
        Some("liked") => liked(session, &ids_in(request)).await,
        _ => Err(Failure::new("unknown command")),
    };
    match result {
        Ok(mut answer) => {
            answer["id"] = id;
            answer["ok"] = json!(true);
            answer
        }
        Err(f) => {
            let mut answer = json!({"id": id, "ok": false, "error": f.message});
            if let Some(status) = f.status {
                answer["status"] = json!(status);
            }
            answer
        }
    }
}

fn spotify_id(text: &str) -> Result<SpotifyId, Failure> {
    SpotifyId::from_base62(text).map_err(|_| Failure::new(format!("not a Spotify id: {text:?}")))
}

/// A playlist's name, owner and tracks; length 0 reads them all.
async fn playlist(session: &Session, id: &str, offset: u32, length: u32) -> Result<Value, Failure> {
    let list_id = spotify_id(id)?;
    let mut uris: Vec<String> = Vec::new();
    let mut header: Option<SelectedListContent> = None;
    let mut from = offset;
    loop {
        let want = if length == 0 { WINDOW } else { (offset + length - from).min(WINDOW) };
        let endpoint = format!(
            "/playlist/v2/playlist/{}?decorate=revision,length,attributes,owner&from={from}&length={want}",
            list_id.to_base62()?
        );
        let bytes = session.spclient().request(&http::Method::GET, &endpoint, None, None).await?;
        let page = SelectedListContent::parse_from_bytes(&bytes)?;
        let rows: Vec<String> = page.contents.items.iter().map(|i| i.uri().to_string()).collect();
        let total = page.length() as u32;
        let got = rows.len() as u32;
        uris.extend(rows);
        if header.is_none() {
            header = Some(page);
        }
        from += got;
        let end = if length == 0 { total } else { total.min(offset + length) };
        if got == 0 || from >= end {
            break;
        }
    }
    let header = header.unwrap_or_default();
    let found = tracks(session, &uris).await?;
    // Rows in the playlist's order; a row Spotify no longer has, or that
    // isn't a song (a podcast episode, a local file), is left out.
    let rows: Vec<Value> = uris.iter().filter_map(|u| found.get(u).cloned()).collect();
    Ok(json!({
        "playlist": {
            "id": id,
            "name": header.attributes.name(),
            "description": header.attributes.description(),
            "owner_id": header.owner_username(),
            "total": header.length(),
        },
        "tracks": rows,
    }))
}

/// An artist's most played tracks in the account's country (or worldwide),
/// in Spotify's order. The artist's metadata names them; when Spotify
/// answers that without them (it does for some accounts), the first page of
/// the artist's context, which is those ten tracks, does.
async fn artist_top(session: &Session, id: &str) -> Result<Value, Failure> {
    let uri = SpotifyUri::Artist { id: spotify_id(id)? };
    let (name, mut uris) = match artist_metadata(session, &uri).await {
        Ok(found) => found,
        Err(_) => (String::new(), Vec::new()),
    };
    if uris.is_empty() {
        let context = session.spclient().get_context(&format!("spotify:artist:{id}")).await?;
        uris = context
            .pages
            .first()
            .map(|p| p.tracks.iter().map(|t| t.uri().to_string()).filter(|u| u.starts_with("spotify:track:")).take(10).collect())
            .unwrap_or_default();
    }
    let found = tracks(session, &uris).await?;
    let rows: Vec<Value> = uris.iter().filter_map(|u| found.get(u).cloned()).collect();
    let name = if name.is_empty() {
        rows.first()
            .and_then(|t| t["artists"].as_array()?.iter().find(|a| a["id"] == id).map(|a| a["name"].clone()))
            .and_then(|n| n.as_str().map(str::to_string))
            .unwrap_or_default()
    } else {
        name
    };
    Ok(json!({"artist": {"id": id, "name": name}, "tracks": rows}))
}

/// The artist's name and top tracks from their metadata.
async fn artist_metadata(session: &Session, uri: &SpotifyUri) -> Result<(String, Vec<String>), Failure> {
    let bytes = session.spclient().get_artist_metadata(uri).await?;
    let artist = librespot_protocol::metadata::Artist::parse_from_bytes(&bytes)?;
    let country = session.country();
    let top = artist
        .top_track
        .iter()
        .find(|t| t.country() == country)
        .or_else(|| artist.top_track.iter().find(|t| t.country() == "ZZ"))
        .or_else(|| artist.top_track.first());
    let uris: Vec<String> = top
        .map(|t| {
            t.track
                .iter()
                .filter_map(|track| SpotifyId::from_raw(track.gid()).ok())
                .filter_map(|tid| SpotifyUri::Track { id: tid }.to_uri().ok())
                .take(10)
                .collect()
        })
        .unwrap_or_default();
    Ok((artist.name().to_string(), uris))
}

/// An album's name and its tracks, disc by disc.
async fn album(session: &Session, id: &str) -> Result<Value, Failure> {
    let uri = SpotifyUri::Album { id: spotify_id(id)? };
    let bytes = session.spclient().get_album_metadata(&uri).await?;
    let album = librespot_protocol::metadata::Album::parse_from_bytes(&bytes)?;
    let uris: Vec<String> = album
        .disc
        .iter()
        .flat_map(|d| d.track.iter())
        .filter_map(|t| SpotifyId::from_raw(t.gid()).ok())
        .filter_map(|tid| SpotifyUri::Track { id: tid }.to_uri().ok())
        .collect();
    let found = tracks(session, &uris).await?;
    let rows: Vec<Value> = uris.iter().filter_map(|u| found.get(u).cloned()).collect();
    Ok(json!({"album": {"id": id, "name": album.name()}, "tracks": rows}))
}

/// The track ids a request names, checked.
fn ids_in(request: &Value) -> Vec<String> {
    request["tracks"]
        .as_array()
        .map(|a| a.iter().filter_map(Value::as_str).map(str::to_string).collect())
        .unwrap_or_default()
}

/// Saves tracks to Liked Songs (on) or takes them out, as Spotify's own app
/// does: a write to the account's "collection" set. The Web API with the
/// session's token is the fallback.
async fn like(session: &Session, ids: &[String], on: bool) -> Result<Value, Failure> {
    if ids.is_empty() {
        return Err(Failure::new("no tracks"));
    }
    for id in ids {
        spotify_id(id)?;
    }
    let mut body = Vec::new();
    pb::string(&mut body, 1, &session.username());
    pb::string(&mut body, 2, "collection");
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    for id in ids {
        let mut item = Vec::new();
        pb::string(&mut item, 1, &format!("spotify:track:{id}"));
        pb::varint(&mut item, 2, now);
        if !on {
            pb::varint(&mut item, 3, 1);
        }
        pb::bytes(&mut body, 3, &item);
    }
    let written = session
        .spclient()
        .request(&http::Method::POST, "/collection/v2/write", Some(collection_headers()), Some(&body))
        .await;
    let via = match written {
        Ok(_) => "collection",
        Err(first) => {
            let method = if on { http::Method::PUT } else { http::Method::DELETE };
            web_api(session, method, &format!("/v1/me/tracks?ids={}", ids.join(",")))
                .await
                .map_err(|f| both(&first.into(), f))?;
            "web-api"
        }
    };
    Ok(json!({"tracks": ids, "on": on, "via": via}))
}

/// Whether each track is in Liked Songs, in the order asked.
async fn liked(session: &Session, ids: &[String]) -> Result<Value, Failure> {
    if ids.is_empty() {
        return Ok(json!({"tracks": [], "liked": []}));
    }
    for id in ids {
        spotify_id(id)?;
    }
    let mut body = Vec::new();
    pb::string(&mut body, 1, &session.username());
    pb::string(&mut body, 2, "collection");
    // Each item is a CollectionItem with just its uri (a bare string is
    // refused with 400).
    for id in ids {
        let mut item = Vec::new();
        pb::string(&mut item, 1, &format!("spotify:track:{id}"));
        pb::bytes(&mut body, 3, &item);
    }
    let asked = session
        .spclient()
        .request(&http::Method::POST, "/collection/v2/contains", Some(collection_headers()), Some(&body))
        .await;
    let first = match &asked {
        Ok(b) => Failure::new(format!("an answer Thrum can't read ({} bytes)", b.len())),
        Err(e) => Failure::new(e.to_string()),
    };
    let found = match asked.ok().and_then(|b| pb::bools(&b, 1)).filter(|f| f.len() == ids.len()) {
        Some(found) => (found, "collection"),
        None => {
            let bytes = web_api(session, http::Method::GET, &format!("/v1/me/tracks/contains?ids={}", ids.join(",")))
                .await
                .map_err(|f| both(&first, f))?;
            let found: Vec<bool> = serde_json::from_slice(&bytes)
                .map_err(|e| Failure::new(format!("unreadable answer from Spotify: {e}")))?;
            (found, "web-api")
        }
    };
    Ok(json!({"tracks": ids, "liked": found.0, "via": found.1}))
}

/// The Web API's failure, saying why the collection endpoint failed first.
fn both(first: &Failure, then: Failure) -> Failure {
    Failure { message: format!("{} (Liked Songs directly: {})", then.message, first.message), status: then.status }
}

fn collection_headers() -> http::HeaderMap {
    let mut h = http::HeaderMap::new();
    let kind = http::HeaderValue::from_static("application/vnd.collection-v2.spotify.proto");
    h.insert(http::header::CONTENT_TYPE, kind.clone());
    h.insert(http::header::ACCEPT, kind);
    h
}

/// A Web API request with the session's own token.
async fn web_api(session: &Session, method: http::Method, path: &str) -> Result<bytes::Bytes, Failure> {
    let token = session.login5().auth_token().await?;
    let request = http::Request::builder()
        .method(method)
        .uri(format!("https://api.spotify.com{path}"))
        .header(http::header::AUTHORIZATION, format!("Bearer {}", token.access_token))
        .header(http::header::CONTENT_LENGTH, "0")
        .body(bytes::Bytes::new())
        .map_err(|e| Failure::new(e.to_string()))?;
    Ok(session.http_client().request_body(request).await?)
}

/// The little protobuf the collection endpoints need, which librespot
/// doesn't compile (collection2v2.proto).
mod pb {
    fn raw_varint(out: &mut Vec<u8>, mut v: u64) {
        while v >= 0x80 {
            out.push((v as u8) | 0x80);
            v >>= 7;
        }
        out.push(v as u8);
    }
    pub fn varint(out: &mut Vec<u8>, field: u32, v: u64) {
        raw_varint(out, u64::from(field) << 3);
        raw_varint(out, v);
    }
    pub fn bytes(out: &mut Vec<u8>, field: u32, b: &[u8]) {
        raw_varint(out, (u64::from(field) << 3) | 2);
        raw_varint(out, b.len() as u64);
        out.extend_from_slice(b);
    }
    pub fn string(out: &mut Vec<u8>, field: u32, s: &str) {
        bytes(out, field, s.as_bytes());
    }
    fn read_varint(b: &[u8], at: &mut usize) -> Option<u64> {
        let mut v = 0u64;
        for shift in (0..64).step_by(7) {
            let byte = *b.get(*at)?;
            *at += 1;
            v |= u64::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return Some(v);
            }
        }
        None
    }
    /// The repeated bool field, packed or not; None for anything unreadable.
    pub fn bools(b: &[u8], field: u32) -> Option<Vec<bool>> {
        let mut out = Vec::new();
        let mut at = 0;
        while at < b.len() {
            let key = read_varint(b, &mut at)?;
            let (f, wire) = ((key >> 3) as u32, key & 7);
            match wire {
                0 => {
                    let v = read_varint(b, &mut at)?;
                    if f == field {
                        out.push(v != 0);
                    }
                }
                2 => {
                    let len = read_varint(b, &mut at)? as usize;
                    let end = at.checked_add(len).filter(|&e| e <= b.len())?;
                    if f == field {
                        let mut i = at;
                        while i < end {
                            out.push(read_varint(&b[..end], &mut i)? != 0);
                        }
                    }
                    at = end;
                }
                5 => at += 4,
                1 => at += 8,
                _ => return None,
            }
        }
        Some(out)
    }
}

/// The details of each track among uris, by URI, a batch at a time.
async fn tracks(session: &Session, uris: &[String]) -> Result<HashMap<String, Value>, Failure> {
    let mut found = HashMap::new();
    let mut asked: Vec<&String> = uris.iter().filter(|u| u.starts_with("spotify:track:")).collect();
    asked.sort();
    asked.dedup();
    for chunk in asked.chunks(BATCH) {
        let mut request = BatchedEntityRequest::new();
        for uri in chunk {
            request.entity_request.push(EntityRequest {
                entity_uri: (*uri).clone(),
                query: vec![ExtensionQuery {
                    extension_kind: EnumOrUnknown::new(ExtensionKind::TRACK_V4),
                    ..Default::default()
                }],
                ..Default::default()
            });
        }
        let response = session.spclient().get_extended_metadata(request).await?;
        for array in response.extended_metadata {
            for data in array.extension_data {
                if !matches!(data.header.status_code, 0 | 200) {
                    continue;
                }
                let Some(any) = data.extension_data.as_ref() else { continue };
                let Ok(track) = librespot_protocol::metadata::Track::parse_from_bytes(&any.value) else {
                    continue;
                };
                found.insert(data.entity_uri.clone(), track_json(&track));
            }
        }
    }
    Ok(found)
}

fn base62(gid: &[u8]) -> String {
    SpotifyId::from_raw(gid).and_then(|id| id.to_base62()).unwrap_or_default()
}

/// A track as spotify_player prints one, with duration_ms.
fn track_json(t: &librespot_protocol::metadata::Track) -> Value {
    let artists: Vec<Value> = t.artist.iter().map(|a| json!({"id": base62(a.gid()), "name": a.name()})).collect();
    let album = &t.album;
    let date = album.date.as_ref().map(|d| {
        if d.has_month() && d.has_day() {
            format!("{:04}-{:02}-{:02}", d.year(), d.month(), d.day())
        } else {
            format!("{:04}", d.year())
        }
    });
    json!({
        "id": base62(t.gid()),
        "name": t.name(),
        "artists": artists,
        "album": {
            "id": base62(album.gid()),
            "name": album.name(),
            "release_date": date.unwrap_or_default(),
            "artists": album.artist.iter().map(|a| json!({"id": base62(a.gid()), "name": a.name()})).collect::<Vec<_>>(),
        },
        "duration_ms": t.duration(),
        "explicit": t.explicit(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use librespot_protocol::metadata::{Album, Artist, Date, Track};

    fn gid(byte: u8) -> Vec<u8> {
        vec![byte; 16]
    }

    #[test]
    fn a_track_is_shaped_as_spotify_player_prints_one() {
        let mut artist = Artist::new();
        artist.set_gid(gid(1));
        artist.set_name("Neon Coast".into());
        let mut date = Date::new();
        date.set_year(2019);
        date.set_month(6);
        date.set_day(14);
        let mut album = Album::new();
        album.set_gid(gid(2));
        album.set_name("Afterglow Avenue".into());
        album.date = protobuf::MessageField::some(date);
        album.artist.push(artist.clone());
        let mut track = Track::new();
        track.set_gid(gid(3));
        track.set_name("Coastline at 2 AM".into());
        track.set_duration(232000);
        track.artist.push(artist);
        track.album = protobuf::MessageField::some(album);

        let j = track_json(&track);
        assert_eq!(j["name"], "Coastline at 2 AM");
        assert_eq!(j["id"].as_str().unwrap().len(), 22);
        assert_eq!(j["artists"][0]["name"], "Neon Coast");
        assert_eq!(j["album"]["name"], "Afterglow Avenue");
        assert_eq!(j["album"]["release_date"], "2019-06-14");
        assert_eq!(j["duration_ms"], 232000);
        assert_eq!(j["explicit"], false);
    }

    #[test]
    fn refusals_keep_spotifys_status() {
        let f: Failure = librespot_core::Error::not_found("no such playlist").into();
        assert_eq!(f.status, Some(404));
        let f: Failure = librespot_core::Error::permission_denied("private").into();
        assert_eq!(f.status, Some(403));
        let f: Failure = librespot_core::Error::unavailable("offline").into();
        assert_eq!(f.status, None);
    }

    #[test]
    fn collection_messages_are_encoded_as_spotify_reads_them() {
        let mut b = Vec::new();
        pb::string(&mut b, 1, "sam");
        pb::varint(&mut b, 2, 300);
        assert_eq!(b, vec![0x0a, 3, b's', b'a', b'm', 0x10, 0xac, 0x02]);
        // found = [true, false, true], packed and not.
        assert_eq!(pb::bools(&[0x0a, 3, 1, 0, 1], 1), Some(vec![true, false, true]));
        assert_eq!(pb::bools(&[0x08, 1, 0x08, 0, 0x08, 1], 1), Some(vec![true, false, true]));
        assert_eq!(pb::bools(&[0x0a, 9, 1], 1), None);
    }

    #[test]
    fn liked_and_like_take_ids_from_tracks() {
        let r = json!({"cmd": "like", "tracks": ["4uLU6hMCjMI75M1A2tKUQC", 7]});
        assert_eq!(ids_in(&r), vec!["4uLU6hMCjMI75M1A2tKUQC".to_string()]);
        assert!(ids_in(&json!({})).is_empty());
    }

    #[test]
    fn a_bad_id_is_refused_before_asking_spotify() {
        assert!(spotify_id("not an id!").is_err());
        assert!(spotify_id("37i9dQZF1DZ06evO2aJbod").is_ok());
    }
}
