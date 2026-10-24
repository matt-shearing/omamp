//! The server Thrum talks to: Spotify's tracks decoded for Thrum, which
//! plays them through its own engine (its equaliser, volume, visualiser and
//! output), the catalogue's answers and the Connect device, over one
//! session. Nothing here touches a sound card. The wire format is in
//! audio_protocol.
//!
//! Thrum pulls: it opens a stream for a track, reads samples when its
//! engine wants more, seeks and closes. Pausing is Thrum not reading. Each
//! stream has a thread of its own, so a track being opened ahead (for a
//! gapless join) never holds up the one playing.
//!
//! The track is fetched and decrypted as librespot's own player does it
//! (librespot-audio, librespot-playback's Symphonia decoder): the Ogg Vorbis
//! file at 320 kbps when Spotify has it, then lower rates and MP3.

use std::collections::HashMap;
use std::io::{self, BufRead, Read, Seek, SeekFrom, Write};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::thread;
use std::time::Duration;

use librespot_audio::{AudioDecrypt, AudioFile, StreamLoaderController};
use librespot_core::session::Session;
use librespot_core::spotify_id::SpotifyId;
use librespot_core::spotify_uri::SpotifyUri;
use librespot_metadata::audio::{AudioFileFormat, AudioFiles, AudioItem, UniqueFields};
use librespot_playback::decoder::{AudioDecoder, AudioPacket, SymphoniaDecoder};
use serde_json::{json, Value};
use symphonia::core::io::MediaSource;
use symphonia::core::probe::Hint;

use crate::audio_protocol::{self as proto, Request};
use crate::connect::Device;

/// Where Spotify's Ogg files keep the audio, after their own header.
const OGG_HEADER_END: u64 = 0xa7;
/// Where that header keeps the loudness (track gain, peak, album gain, peak).
const NORMALISATION_AT: u64 = 144;

/// The formats tried, best first: 320 kbps Ogg Vorbis is "Very High".
const FORMATS: [AudioFileFormat; 7] = [
    AudioFileFormat::OGG_VORBIS_320,
    AudioFileFormat::MP3_320,
    AudioFileFormat::MP3_256,
    AudioFileFormat::OGG_VORBIS_160,
    AudioFileFormat::MP3_160,
    AudioFileFormat::OGG_VORBIS_96,
    AudioFileFormat::MP3_96,
];

fn kbps(format: AudioFileFormat) -> u32 {
    match format {
        AudioFileFormat::OGG_VORBIS_320 | AudioFileFormat::MP3_320 => 320,
        AudioFileFormat::MP3_256 => 256,
        AudioFileFormat::OGG_VORBIS_160 | AudioFileFormat::MP3_160 => 160,
        _ => 96,
    }
}

/// Writes whole frames, one at a time, from any thread. Every Out writes
/// through the same lock, so frames never interleave.
#[derive(Clone)]
pub struct Out(Arc<Mutex<io::Stdout>>);

impl Out {
    pub fn new() -> Self {
        static OUT: OnceLock<Out> = OnceLock::new();
        OUT.get_or_init(|| Out(Arc::new(Mutex::new(io::stdout())))).clone()
    }
    pub fn send(&self, header: &Value, data: &[u8]) -> bool {
        let bytes = proto::frame(header, data);
        let mut out = self.0.lock().unwrap_or_else(|e| e.into_inner());
        out.write_all(&bytes).is_ok() && out.flush().is_ok()
    }
}

enum Job {
    Read { id: Value, frames: usize },
    Seek { id: Value, position_ms: u32 },
}

/// How long a read of the Connect device's sound waits for some.
const CONNECT_WAIT: Duration = Duration::from_millis(60);

/// Answers requests from stdin until it closes or says quit. device: the
/// Connect device, when the sidecar was started as one.
pub fn serve(session: Session, runtime: tokio::runtime::Handle, out: Out, device: Option<Arc<Device>>) {
    let mut streams: HashMap<u64, mpsc::Sender<Job>> = HashMap::new();
    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        let Ok(line) = line else { break };
        if line.trim().is_empty() {
            continue;
        }
        match proto::parse(&line) {
            Err(bad) => {
                out.send(&proto::failure(&bad.id, &bad.error, None), &[]);
            }
            Ok(Request::Quit) => break,
            Ok(Request::Ping { id }) => {
                out.send(&json!({"id": id, "ok": true, "user": session.username()}), &[]);
            }
            Ok(Request::Open { id, stream, track, position_ms }) => {
                if streams.contains_key(&stream) {
                    out.send(&proto::failure(&id, "that stream is already open", None), &[]);
                    continue;
                }
                let (tx, rx) = mpsc::channel();
                streams.insert(stream, tx);
                let (session, runtime, out) = (session.clone(), runtime.clone(), out.clone());
                thread::spawn(move || run_stream(session, runtime, out, id, track, position_ms, rx));
            }
            Ok(Request::Read { id, stream, frames }) => {
                pass(&mut streams, &out, stream, id.clone(), Job::Read { id, frames });
            }
            Ok(Request::Seek { id, stream, position_ms }) => {
                pass(&mut streams, &out, stream, id.clone(), Job::Seek { id, position_ms });
            }
            Ok(Request::Catalogue { id: _, request }) => {
                // Lists can take seconds; streams carry on meanwhile.
                let (session, out) = (session.clone(), out.clone());
                runtime.spawn(async move {
                    let answer = crate::handle(&session, &request).await;
                    out.send(&answer, &[]);
                });
            }
            Ok(Request::OpenConnect { id, stream }) => {
                let Some(device) = device.clone() else {
                    out.send(&proto::failure(&id, "Thrum isn't a Spotify Connect device now", None), &[]);
                    continue;
                };
                if streams.contains_key(&stream) {
                    out.send(&proto::failure(&id, "that stream is already open", None), &[]);
                    continue;
                }
                let (tx, rx) = mpsc::channel();
                streams.insert(stream, tx);
                let out = out.clone();
                thread::spawn(move || run_connect_stream(device, out, id, rx));
            }
            Ok(Request::Connect { id, action, value }) => {
                let answer = match &device {
                    None => proto::failure(&id, "Thrum isn't a Spotify Connect device now", None),
                    Some(d) => match d.command(&action, &value) {
                        Ok(()) => json!({"id": id, "ok": true}),
                        Err(e) => proto::failure(&id, &e.to_string(), None),
                    },
                };
                out.send(&answer, &[]);
            }
            Ok(Request::Close { id, stream }) => {
                // Dropping the sender ends the stream's thread.
                streams.remove(&stream);
                out.send(&json!({"id": id, "ok": true}), &[]);
            }
        }
    }
}

/// Hands a job to its stream's thread. A stream whose thread has ended
/// (its open failed) is forgotten.
fn pass(streams: &mut HashMap<u64, mpsc::Sender<Job>>, out: &Out, stream: u64, id: Value, job: Job) {
    let alive = streams.get(&stream).map(|tx| tx.send(job).is_ok()).unwrap_or(false);
    if !alive {
        streams.remove(&stream);
        out.send(&proto::failure(&id, "no such stream", None), &[]);
    }
}

/// The Connect device's sound as a stream: it has no end, and a read
/// returns what there is (nothing while the phone is between tracks).
fn run_connect_stream(device: Arc<Device>, out: Out, id: Value, jobs: mpsc::Receiver<Job>) {
    out.send(&json!({"id": id, "ok": true, "rate": proto::RATE, "channels": proto::CHANNELS, "live": true,
                     "title": "Spotify Connect"}), &[]);
    while let Ok(job) = jobs.recv() {
        match job {
            Job::Read { id, frames } => {
                let samples = device.pcm.take(frames, CONNECT_WAIT);
                let bytes: Vec<u8> = samples.iter().flat_map(|s| s.to_le_bytes()).collect();
                let header = json!({"id": id, "ok": true, "frames": samples.len() / proto::CHANNELS, "end": false});
                if !out.send(&header, &bytes) {
                    return;
                }
            }
            Job::Seek { id, position_ms } => {
                let answer = match device.command("seek", &json!(position_ms)) {
                    Ok(()) => json!({"id": id, "ok": true, "position_ms": position_ms}),
                    Err(e) => proto::failure(&id, &e.to_string(), None),
                };
                out.send(&answer, &[]);
            }
        }
    }
}

struct Track {
    decoder: Box<dyn AudioDecoder + Send>,
    loader: StreamLoaderController,
    /// Decoded but not yet read, interleaved stereo.
    pending: Vec<f64>,
    ended: bool,
}

fn run_stream(
    session: Session,
    runtime: tokio::runtime::Handle,
    out: Out,
    id: Value,
    track: String,
    position_ms: u32,
    jobs: mpsc::Receiver<Job>,
) {
    let mut track = match open(&session, &runtime, &track, position_ms) {
        Ok((t, mut header)) => {
            header["id"] = id;
            header["ok"] = json!(true);
            out.send(&header, &[]);
            t
        }
        Err(message) => {
            out.send(&proto::failure(&id, &message, proto::reason_for(&message)), &[]);
            return;
        }
    };
    while let Ok(job) = jobs.recv() {
        match job {
            Job::Read { id, frames } => {
                let (samples, end) = read(&mut track, frames);
                let header = json!({"id": id, "ok": true, "frames": samples.len() / proto::CHANNELS, "end": end});
                if !out.send(&header, &proto::to_bytes(&samples)) {
                    return;
                }
            }
            Job::Seek { id, position_ms } => {
                track.loader.set_random_access_mode();
                let answer = match track.decoder.seek(position_ms) {
                    Ok(at) => {
                        track.pending.clear();
                        track.ended = false;
                        json!({"id": id, "ok": true, "position_ms": at})
                    }
                    Err(e) => proto::failure(&id, &e.to_string(), None),
                };
                track.loader.set_stream_mode();
                out.send(&answer, &[]);
            }
        }
    }
    track.loader.close();
}

/// Up to frames frames, and whether the track has ended.
fn read(track: &mut Track, frames: usize) -> (Vec<f64>, bool) {
    let want = frames * proto::CHANNELS;
    while track.pending.len() < want && !track.ended {
        match track.decoder.next_packet() {
            Ok(Some((_, AudioPacket::Samples(s)))) => track.pending.extend_from_slice(&s),
            Ok(Some((_, AudioPacket::Raw(_)))) => {}
            // A damaged packet ends the track rather than the stream.
            Ok(None) | Err(_) => track.ended = true,
        }
    }
    let n = want.min(track.pending.len());
    let samples: Vec<f64> = track.pending.drain(..n).collect();
    (samples, track.ended && track.pending.is_empty())
}

fn open(session: &Session, runtime: &tokio::runtime::Handle, track: &str, position_ms: u32) -> Result<(Track, Value), String> {
    let track_id = SpotifyId::from_base62(track).map_err(|_| format!("not a Spotify track id: {track:?}"))?;
    let (item, format, file) = runtime.block_on(async {
        let uri = SpotifyUri::Track { id: track_id };
        let mut item = AudioItem::get_file(session, uri).await.map_err(|e| e.to_string())?;
        if item.files.is_empty() {
            // Relinked: the same song under another id, playable here.
            if let Some(alternatives) = item.alternatives.clone() {
                for alt in alternatives.0 {
                    if let Ok(a) = AudioItem::get_file(session, alt).await {
                        if a.availability.is_ok() && !a.files.is_empty() {
                            item = a;
                            break;
                        }
                    }
                }
            }
        }
        if let Err(why) = &item.availability {
            return Err(format!("This track is unavailable ({why:?})"));
        }
        let (format, file_id) = FORMATS
            .iter()
            .find_map(|f| item.files.get(f).map(|id| (*f, *id)))
            .ok_or_else(|| "This track is unavailable in any format Thrum can play".to_string())?;
        let bytes_per_second = (kbps(format) as usize * 1024) / 8;
        let file = AudioFile::open(session, file_id, bytes_per_second).await.map_err(|e| e.to_string())?;
        let key = session.audio_key().request(track_id, file_id).await.ok();
        Ok::<_, String>((item, format, (file, key)))
    })?;
    let (file, key) = file;
    let loader = file.get_stream_loader_controller().map_err(|e| e.to_string())?;
    let length = loader.len() as u64;
    let mut decrypted = AudioDecrypt::new(key, file);
    let ogg = AudioFiles::is_ogg_vorbis(format);
    let gains = if ogg { normalisation(&mut decrypted).ok() } else { None };
    let offset = if ogg { OGG_HEADER_END } else { 0 };
    let sub = Subfile::new(decrypted, offset, length).map_err(|e| e.to_string())?;
    let mut hint = Hint::new();
    if let Some(mime) = AudioFiles::mime_type(format) {
        hint.mime_type(mime);
    }
    let mut decoder = SymphoniaDecoder::new(sub, hint).map_err(|e| e.to_string())?;
    let start = if position_ms > item.duration_ms { 0 } else { position_ms };
    let at = decoder.seek(start).map_err(|e| e.to_string())?;
    loader.set_stream_mode();

    let (artists, album, album_artists) = match &item.unique_fields {
        UniqueFields::Track { artists, album, album_artists, .. } => {
            (artists.0.iter().map(|a| a.name.clone()).collect::<Vec<_>>(), album.clone(), album_artists.clone())
        }
        _ => (Vec::new(), String::new(), Vec::new()),
    };
    let mut header = json!({
        "rate": proto::RATE,
        "channels": proto::CHANNELS,
        "duration_ms": item.duration_ms,
        "position_ms": at,
        "bitrate": kbps(format),
        "format": format!("{format:?}"),
        "title": item.name,
        "artists": artists,
        "album": album,
        "album_artists": album_artists,
    });
    if let Some((track_gain, track_peak, album_gain, album_peak)) = gains {
        header["track_gain_db"] = json!(track_gain);
        header["track_peak"] = json!(track_peak);
        header["album_gain_db"] = json!(album_gain);
        header["album_peak"] = json!(album_peak);
    }
    Ok((Track { decoder: Box::new(decoder), loader, pending: Vec::new(), ended: false }, header))
}

/// Spotify's loudness figures from its Ogg header.
fn normalisation<T: Read + Seek>(file: &mut T) -> io::Result<(f32, f32, f32, f32)> {
    file.seek(SeekFrom::Start(NORMALISATION_AT))?;
    let mut b = [0u8; 16];
    file.read_exact(&mut b)?;
    let f = |i: usize| f32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
    Ok((f(0), f(4), f(8), f(12)))
}

/// The file from offset on, as the decoder sees it.
struct Subfile<T: Read + Seek> {
    stream: T,
    offset: u64,
    length: u64,
}

impl<T: Read + Seek> Subfile<T> {
    fn new(mut stream: T, offset: u64, length: u64) -> io::Result<Self> {
        stream.seek(SeekFrom::Start(offset))?;
        Ok(Subfile { stream, offset, length })
    }
}

impl<T: Read + Seek> Read for Subfile<T> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.stream.read(buf)
    }
}

impl<T: Read + Seek> Seek for Subfile<T> {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let pos = match pos {
            SeekFrom::Start(o) => SeekFrom::Start(o + self.offset),
            other => other,
        };
        let at = self.stream.seek(pos)?;
        Ok(at.saturating_sub(self.offset))
    }
}

impl<T: Read + Seek + Send + Sync> MediaSource for Subfile<T> {
    fn is_seekable(&self) -> bool {
        true
    }
    fn byte_len(&self) -> Option<u64> {
        Some(self.length.saturating_sub(self.offset))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn the_subfile_hides_spotifys_header() {
        let data: Vec<u8> = (0u8..=255).collect();
        let mut s = Subfile::new(Cursor::new(data), OGG_HEADER_END, 256).unwrap();
        let mut b = [0u8; 2];
        s.read_exact(&mut b).unwrap();
        assert_eq!(b, [0xa7, 0xa8]);
        assert_eq!(s.seek(SeekFrom::Start(1)).unwrap(), 1);
        s.read_exact(&mut b).unwrap();
        assert_eq!(b, [0xa8, 0xa9]);
        assert_eq!(s.byte_len(), Some(256 - OGG_HEADER_END));
    }

    #[test]
    fn loudness_comes_from_the_header() {
        let mut data = vec![0u8; 200];
        for (i, v) in [-6.5f32, 0.9, -7.25, 0.95].iter().enumerate() {
            data[144 + i * 4..148 + i * 4].copy_from_slice(&v.to_le_bytes());
        }
        assert_eq!(normalisation(&mut Cursor::new(data)).unwrap(), (-6.5, 0.9, -7.25, 0.95));
    }

    #[test]
    fn very_high_is_tried_first() {
        assert_eq!(FORMATS[0], AudioFileFormat::OGG_VORBIS_320);
        assert_eq!(kbps(FORMATS[0]), 320);
    }
}
