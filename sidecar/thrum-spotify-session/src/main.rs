//! thrum-spotify-session: reads what Spotify won't let a new Web API app read
//! (the tracks of Spotify's own playlists, an artist's top tracks) the way
//! Spotify's own apps do, over a librespot session.
//!
//! It signs in with a librespot credential that already exists (by default
//! spotify_player's, ~/.cache/spotify-player/credentials.json), which it only
//! reads, and never stores a new one. It opens no Spotify Connect device and
//! plays nothing. Its requests go to Spotify's private spclient endpoints:
//!
//!   playlist     GET  /playlist/v2/playlist/<id>?decorate=…&from=…&length=…
//!   track names  POST /extended-metadata/v0/extended-metadata (TRACK_V4)
//!   artist       POST /extended-metadata/v0/extended-metadata (ARTIST_V4)
//!
//! These have no per-app quota, so they don't meet the Web API's rate
//! limits. Spotify's developer terms don't cover them, and Spotify can
//! change them.
//!
//! Protocol: one JSON object a line on stdin, one answer a line on stdout.
//!
//!   {"id":1,"cmd":"ping"}
//!   {"id":2,"cmd":"playlist","playlist":"<base62>","offset":0,"length":0}
//!   {"id":3,"cmd":"artist-top","artist":"<base62>"}
//!
//! The first line out is {"ready":true,…} once signed in, or
//! {"ready":false,"error":…}. Answers carry the request's id and "ok"; a
//! failure has "error" and, when Spotify gave one, "status". Tracks are
//! shaped as spotify_player prints them, with duration_ms.
//!
//! One-shot, for a person or a test: thrum-spotify-session playlist <id>,
//! artist-top <id>, or ping, each printing one answer.
//!
//! `thrum-spotify-session audio` is the other mode: it decodes Spotify's
//! tracks for Thrum to play (audio.rs, audio_protocol.rs). Its first frame
//! out is the same ready line.

mod audio;
mod audio_protocol;

use std::collections::HashMap;
use std::path::PathBuf;

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
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};

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
        "usage: thrum-spotify-session [--credentials <file>] [serve | audio | ping | playlist <id> | artist-top <id>]"
    );
    std::process::exit(2);
}

#[tokio::main]
async fn main() {
    let mut args = std::env::args().skip(1).collect::<Vec<_>>();
    let mut credentials = default_credentials();
    if let Some(at) = args.iter().position(|a| a == "--credentials") {
        if at + 1 >= args.len() {
            usage();
        }
        credentials = PathBuf::from(args.remove(at + 1));
        args.remove(at);
    }
    let audio_mode = args.first().map(String::as_str) == Some("audio");
    let one_shot = match args.first().map(String::as_str) {
        None | Some("serve") | Some("audio") => None,
        Some("ping") => Some(json!({"id": 0, "cmd": "ping"})),
        Some("playlist") if args.len() == 2 => Some(json!({"id": 0, "cmd": "playlist", "playlist": args[1]})),
        Some("artist-top") if args.len() == 2 => Some(json!({"id": 0, "cmd": "artist-top", "artist": args[1]})),
        Some("--help" | "-h") => usage(),
        _ => usage(),
    };

    let session = match connect(&credentials).await {
        Ok(s) => s,
        Err(f) => {
            let not_ready = json!({"ready": false, "error": f.message});
            if audio_mode {
                audio::Out::new().send(&not_ready, &[]);
            } else {
                println!("{not_ready}");
            }
            std::process::exit(1);
        }
    };

    if audio_mode {
        let out = audio::Out::new();
        let ready = json!({"ready": true, "user": session.username(), "country": session.country(),
                           "rate": audio_protocol::RATE, "channels": audio_protocol::CHANNELS});
        if out.send(&ready, &[]) {
            let (s, runtime) = (session.clone(), tokio::runtime::Handle::current());
            let _ = tokio::task::spawn_blocking(move || audio::serve(s, runtime, out)).await;
        }
        session.shutdown();
        // Streams still opening hold the runtime; nothing more is wanted.
        std::process::exit(0);
    }

    if let Some(request) = one_shot {
        let answer = handle(&session, &request).await;
        println!("{answer}");
        std::process::exit(if answer["ok"] == true { 0 } else { 1 });
    }

    let mut out = tokio::io::stdout();
    let ready = json!({"ready": true, "user": session.username(), "country": session.country()});
    let _ = out.write_all(format!("{ready}\n").as_bytes()).await;
    let _ = out.flush().await;
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        if line.trim().is_empty() {
            continue;
        }
        let answer = match serde_json::from_str::<Value>(&line) {
            Ok(request) if request["cmd"] == "quit" => break,
            Ok(request) => handle(&session, &request).await,
            Err(e) => json!({"id": Value::Null, "ok": false, "error": format!("not JSON: {e}")}),
        };
        if out.write_all(format!("{answer}\n").as_bytes()).await.is_err() || out.flush().await.is_err() {
            break;
        }
    }
    session.shutdown();
}

/// Signs in with the stored credential, without storing anything.
async fn connect(path: &PathBuf) -> Result<Session, Failure> {
    let text = std::fs::read_to_string(path)
        .map_err(|e| Failure::new(format!("can't read the Spotify sign-in at {}: {e}", path.display())))?;
    let credentials: Credentials = serde_json::from_str(&text)
        .map_err(|e| Failure::new(format!("the Spotify sign-in at {} isn't one librespot can use: {e}", path.display())))?;
    let session = Session::new(SessionConfig::default(), None);
    tokio::time::timeout(std::time::Duration::from_secs(20), session.connect(credentials, false))
        .await
        .map_err(|_| Failure::new("Spotify didn't answer the sign-in within 20 seconds"))??;
    Ok(session)
}

async fn handle(session: &Session, request: &Value) -> Value {
    let id = request["id"].clone();
    let result = match request["cmd"].as_str() {
        Some("ping") => Ok(json!({"user": session.username(), "country": session.country()})),
        Some("playlist") => {
            let offset = request["offset"].as_u64().unwrap_or(0) as u32;
            let length = request["length"].as_u64().unwrap_or(0) as u32;
            playlist(session, request["playlist"].as_str().unwrap_or(""), offset, length).await
        }
        Some("artist-top") => artist_top(session, request["artist"].as_str().unwrap_or("")).await,
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
/// in Spotify's order.
async fn artist_top(session: &Session, id: &str) -> Result<Value, Failure> {
    let uri = SpotifyUri::Artist { id: spotify_id(id)? };
    let bytes = session.spclient().get_artist_metadata(&uri).await?;
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
    let found = tracks(session, &uris).await?;
    let rows: Vec<Value> = uris.iter().filter_map(|u| found.get(u).cloned()).collect();
    Ok(json!({"artist": {"id": id, "name": artist.name()}, "tracks": rows}))
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
    fn a_bad_id_is_refused_before_asking_spotify() {
        assert!(spotify_id("not an id!").is_err());
        assert!(spotify_id("37i9dQZF1DZ06evO2aJbod").is_ok());
    }
}
