//! The sidecar's wire format, kept apart from the playing so it can be
//! tested without Spotify. docs/spotify.md describes it for people.
//!
//! In: one JSON request a line on stdin.
//!
//!   {"id":1,"cmd":"open","stream":7,"track":"<base62>","position_ms":0}
//!   {"id":2,"cmd":"read","stream":7,"frames":4096}
//!   {"id":3,"cmd":"seek","stream":7,"position_ms":60000}
//!   {"id":4,"cmd":"close","stream":7}
//!   {"id":5,"cmd":"ping"}
//!   {"id":6,"cmd":"playlist","playlist":"<base62>"}       the catalogue:
//!   {"id":7,"cmd":"artist-top","artist":"<base62>"}       answered on the
//!   {"id":8,"cmd":"album","album":"<base62>"}             session's runtime,
//!   {"id":9,"cmd":"like","tracks":["<base62>"],"on":true}  beside the streams
//!   {"id":10,"cmd":"liked","tracks":["<base62>"]}
//!   {"id":11,"cmd":"open-connect","stream":8}   the Connect device's sound
//!   {"id":12,"cmd":"connect","action":"pause"}  play, pause, next, previous,
//!                                               seek (position_ms), volume
//!   {"cmd":"quit"}
//!
//! Out: frames on stdout, each a little-endian u32 length and then that many
//! bytes: a JSON header, a newline, and for a read the samples (interleaved
//! stereo f32 little-endian at 44.1 kHz) the header counts in "frames".
//! A frame with "event" instead of "id" is news from the Connect device
//! (connect.rs), sent whenever it happens.

use serde_json::{json, Value};

/// The rate and channels librespot decodes Spotify's files to.
pub const RATE: u32 = 44100;
pub const CHANNELS: usize = 2;
/// The most frames one read returns, so one answer stays small.
pub const MAX_READ_FRAMES: usize = 16384;

#[derive(Debug, PartialEq)]
pub enum Request {
    Open { id: Value, stream: u64, track: String, position_ms: u32 },
    Read { id: Value, stream: u64, frames: usize },
    Seek { id: Value, stream: u64, position_ms: u32 },
    Close { id: Value, stream: u64 },
    Ping { id: Value },
    /// A catalogue request (playlist, artist-top, album, like, liked),
    /// passed on whole.
    Catalogue { id: Value, request: Value },
    OpenConnect { id: Value, stream: u64 },
    Connect { id: Value, action: String, value: Value },
    Quit,
}

/// The commands answered by the catalogue rather than a stream.
pub const CATALOGUE: [&str; 5] = ["playlist", "artist-top", "album", "like", "liked"];

/// A request that couldn't be read: the id to answer (null when there was
/// none) and why.
#[derive(Debug, PartialEq)]
pub struct BadRequest {
    pub id: Value,
    pub error: String,
}

pub fn parse(line: &str) -> Result<Request, BadRequest> {
    let v: Value = serde_json::from_str(line)
        .map_err(|e| BadRequest { id: Value::Null, error: format!("not JSON: {e}") })?;
    let id = v["id"].clone();
    let bad = |error: &str| BadRequest { id: id.clone(), error: error.to_string() };
    let stream = || v["stream"].as_u64().ok_or_else(|| bad("no stream"));
    let ms = |key: &str| v[key].as_u64().unwrap_or(0).min(u32::MAX as u64) as u32;
    match v["cmd"].as_str() {
        Some("open") => {
            let track = v["track"].as_str().unwrap_or("").to_string();
            if track.is_empty() {
                return Err(bad("no track"));
            }
            Ok(Request::Open { id: id.clone(), stream: stream()?, track, position_ms: ms("position_ms") })
        }
        Some("read") => {
            let frames = v["frames"].as_u64().unwrap_or(4096) as usize;
            Ok(Request::Read { id: id.clone(), stream: stream()?, frames: frames.clamp(1, MAX_READ_FRAMES) })
        }
        Some("seek") => Ok(Request::Seek { id: id.clone(), stream: stream()?, position_ms: ms("position_ms") }),
        Some("close") => Ok(Request::Close { id: id.clone(), stream: stream()? }),
        Some("ping") => Ok(Request::Ping { id: id.clone() }),
        Some(cmd) if CATALOGUE.contains(&cmd) => Ok(Request::Catalogue { id: id.clone(), request: v.clone() }),
        Some("open-connect") => Ok(Request::OpenConnect { id: id.clone(), stream: stream()? }),
        Some("connect") => {
            let action = v["action"].as_str().unwrap_or("").to_string();
            if !["play", "pause", "next", "previous", "seek", "volume"].contains(&action.as_str()) {
                return Err(bad("unknown Connect action"));
            }
            let value = if action == "seek" { v["position_ms"].clone() } else { v["value"].clone() };
            Ok(Request::Connect { id: id.clone(), action, value })
        }
        Some("quit") => Ok(Request::Quit),
        _ => Err(bad("unknown command")),
    }
}

/// One frame for stdout: the length, the header, a newline and the data.
pub fn frame(header: &Value, data: &[u8]) -> Vec<u8> {
    let head = header.to_string();
    let len = head.len() + 1 + data.len();
    let mut out = Vec::with_capacity(4 + len);
    out.extend_from_slice(&(len as u32).to_le_bytes());
    out.extend_from_slice(head.as_bytes());
    out.push(b'\n');
    out.extend_from_slice(data);
    out
}

/// A failure's answer.
pub fn failure(id: &Value, error: &str, reason: Option<&str>) -> Value {
    let mut v = json!({"id": id, "ok": false, "error": error});
    if let Some(r) = reason {
        v["reason"] = json!(r);
    }
    v
}

/// librespot's samples (f64, -1..1) as the f32 little-endian bytes Thrum
/// reads.
pub fn to_bytes(samples: &[f64]) -> Vec<u8> {
    let mut out = Vec::with_capacity(samples.len() * 4);
    for s in samples {
        out.extend_from_slice(&(s.clamp(-1.0, 1.0) as f32).to_le_bytes());
    }
    out
}

/// What Spotify's refusal means for the person: "premium" when the account
/// can't stream, "unavailable" when the track can't be played here.
pub fn reason_for(message: &str) -> Option<&'static str> {
    let m = message.to_ascii_lowercase();
    if m.contains("premium") {
        Some("premium")
    } else if m.contains("unavailable") || m.contains("not available") || m.contains("embargo") || m.contains("region") {
        Some("unavailable")
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requests_are_read() {
        assert_eq!(
            parse(r#"{"id":1,"cmd":"open","stream":7,"track":"4uLU6hMCjMI75M1A2tKUQC","position_ms":1500}"#),
            Ok(Request::Open { id: json!(1), stream: 7, track: "4uLU6hMCjMI75M1A2tKUQC".into(), position_ms: 1500 })
        );
        assert_eq!(
            parse(r#"{"id":2,"cmd":"read","stream":7,"frames":4096}"#),
            Ok(Request::Read { id: json!(2), stream: 7, frames: 4096 })
        );
        assert_eq!(
            parse(r#"{"id":3,"cmd":"seek","stream":7,"position_ms":60000}"#),
            Ok(Request::Seek { id: json!(3), stream: 7, position_ms: 60000 })
        );
        assert_eq!(parse(r#"{"id":4,"cmd":"close","stream":7}"#), Ok(Request::Close { id: json!(4), stream: 7 }));
        assert_eq!(parse(r#"{"id":5,"cmd":"ping"}"#), Ok(Request::Ping { id: json!(5) }));
        assert_eq!(parse(r#"{"cmd":"quit"}"#), Ok(Request::Quit));
    }

    #[test]
    fn catalogue_and_connect_requests_are_read() {
        let like = r#"{"id":9,"cmd":"like","tracks":["4uLU6hMCjMI75M1A2tKUQC"],"on":true}"#;
        match parse(like) {
            Ok(Request::Catalogue { id, request }) => {
                assert_eq!(id, json!(9));
                assert_eq!(request["on"], true);
            }
            other => panic!("{other:?}"),
        }
        assert!(matches!(parse(r#"{"id":1,"cmd":"playlist","playlist":"x"}"#), Ok(Request::Catalogue { .. })));
        assert_eq!(
            parse(r#"{"id":2,"cmd":"open-connect","stream":3}"#),
            Ok(Request::OpenConnect { id: json!(2), stream: 3 })
        );
        assert_eq!(
            parse(r#"{"id":3,"cmd":"connect","action":"seek","position_ms":5000}"#),
            Ok(Request::Connect { id: json!(3), action: "seek".into(), value: json!(5000) })
        );
        assert_eq!(parse(r#"{"id":4,"cmd":"connect","action":"dance"}"#).unwrap_err().error, "unknown Connect action");
    }

    #[test]
    fn reads_are_bounded() {
        assert_eq!(
            parse(r#"{"id":1,"cmd":"read","stream":1,"frames":99999999}"#),
            Ok(Request::Read { id: json!(1), stream: 1, frames: MAX_READ_FRAMES })
        );
        assert_eq!(
            parse(r#"{"id":1,"cmd":"read","stream":1,"frames":0}"#),
            Ok(Request::Read { id: json!(1), stream: 1, frames: 1 })
        );
    }

    #[test]
    fn bad_requests_keep_their_id() {
        assert_eq!(parse("{nope").unwrap_err().id, Value::Null);
        let e = parse(r#"{"id":9,"cmd":"open","stream":1}"#).unwrap_err();
        assert_eq!((e.id, e.error.as_str()), (json!(9), "no track"));
        assert_eq!(parse(r#"{"id":8,"cmd":"read"}"#).unwrap_err().error, "no stream");
        assert_eq!(parse(r#"{"id":7,"cmd":"dance"}"#).unwrap_err().error, "unknown command");
    }

    #[test]
    fn a_frame_is_length_header_newline_data() {
        let f = frame(&json!({"id":2,"ok":true,"frames":1}), &[1, 2, 3, 4, 5, 6, 7, 8]);
        let len = u32::from_le_bytes([f[0], f[1], f[2], f[3]]) as usize;
        assert_eq!(len, f.len() - 4);
        let body = &f[4..];
        let nl = body.iter().position(|&b| b == b'\n').unwrap();
        let header: Value = serde_json::from_slice(&body[..nl]).unwrap();
        assert_eq!(header["frames"], 1);
        assert_eq!(&body[nl + 1..], &[1, 2, 3, 4, 5, 6, 7, 8]);
    }

    #[test]
    fn samples_become_clamped_f32() {
        let b = to_bytes(&[0.5, -2.0]);
        assert_eq!(b.len(), 8);
        assert_eq!(f32::from_le_bytes([b[0], b[1], b[2], b[3]]), 0.5);
        assert_eq!(f32::from_le_bytes([b[4], b[5], b[6], b[7]]), -1.0);
    }

    #[test]
    fn refusals_are_named() {
        assert_eq!(reason_for("Premium account required"), Some("premium"));
        assert_eq!(reason_for("Service unavailable { NotAvailable }"), Some("unavailable"));
        assert_eq!(reason_for("connection reset"), None);
        assert_eq!(failure(&json!(3), "x", Some("premium"))["reason"], "premium");
    }
}
