//! Thrum as a Spotify Connect device: started with `--connect <name>`, the
//! sidecar shows up under that name in every Spotify app signed in to the
//! same account, and playing on it from a phone plays in Thrum.
//!
//! librespot-connect's Spirc speaks Connect and drives librespot's own
//! Player, as in librespot itself. The Player's sink here is no sound card:
//! it hands the decoded sound to Thrum, which pulls it through a stream
//! opened with `open-connect` and plays it through its own engine (its
//! volume, equaliser, visualiser and output). The sink waits while Thrum
//! hasn't taken what it has, so the Player goes at the pace Thrum plays.
//!
//! What happens on the device is sent to Thrum as event frames:
//!
//!   {"event":"connect","state":"playing"|"paused"|"stopped","position_ms":…}
//!   {"event":"connect","track":{"id","title","artists","album","duration_ms","art"}}
//!   {"event":"connect","position_ms":…}            a seek
//!   {"event":"connect","volume":0.0…1.0}
//!
//! and Thrum sends `connect` requests back (play, pause, next, previous,
//! seek, volume), as the phone's buttons would.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use librespot_connect::{ConnectConfig, Spirc};
use librespot_core::authentication::Credentials;
use librespot_core::config::DeviceType;
use librespot_core::session::Session;
use librespot_metadata::audio::{AudioItem, UniqueFields};
use librespot_playback::audio_backend::{Sink, SinkResult};
use librespot_playback::config::{Bitrate, PlayerConfig};
use librespot_playback::convert::Converter;
use librespot_playback::decoder::AudioPacket;
use librespot_playback::mixer::{Mixer, MixerConfig, NoOpVolume};
use librespot_playback::player::{Player, PlayerEvent};
use serde_json::{json, Value};

use crate::audio::Out;
use crate::audio_protocol as proto;

/// How much decoded sound waits for Thrum: a quarter of a second, so a
/// skip or a seek on the phone is heard soon.
const CAPACITY: usize = proto::RATE as usize / 4 * proto::CHANNELS;

#[derive(Default)]
struct Buffer {
    samples: VecDeque<f32>,
    /// Thrum paused: the sink takes what the Player still writes, so the
    /// Player never waits on a pause it hasn't heard yet.
    hold: bool,
    closed: bool,
}

/// The sound between the Player's thread and Thrum's reads.
#[derive(Default)]
pub struct Pcm {
    buffer: Mutex<Buffer>,
    changed: Condvar,
}

impl Pcm {
    fn lock(&self) -> std::sync::MutexGuard<'_, Buffer> {
        self.buffer.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Adds samples, waiting while the buffer is full and Thrum reads.
    pub fn push(&self, samples: &[f32]) {
        let mut b = self.lock();
        while b.samples.len() >= CAPACITY && !b.hold && !b.closed {
            b = self.changed.wait_timeout(b, Duration::from_millis(100)).unwrap_or_else(|e| e.into_inner()).0;
        }
        if !b.closed {
            b.samples.extend(samples.iter().copied());
        }
        self.changed.notify_all();
    }

    /// Up to frames frames, waiting at most wait for the first of them.
    /// Empty when nothing came: Thrum plays silence meanwhile.
    pub fn take(&self, frames: usize, wait: Duration) -> Vec<f32> {
        let mut b = self.lock();
        if b.samples.is_empty() && !b.closed {
            b = self.changed.wait_timeout(b, wait).unwrap_or_else(|e| e.into_inner()).0;
        }
        let n = (frames * proto::CHANNELS).min(b.samples.len()) / proto::CHANNELS * proto::CHANNELS;
        let out: Vec<f32> = b.samples.drain(..n).collect();
        self.changed.notify_all();
        out
    }

    pub fn set_hold(&self, on: bool) {
        self.lock().hold = on;
        self.changed.notify_all();
    }

    /// Drops what waits (after a seek or a stop, it is the wrong sound).
    pub fn clear(&self) {
        self.lock().samples.clear();
        self.changed.notify_all();
    }

    pub fn close(&self) {
        self.lock().closed = true;
        self.changed.notify_all();
    }
}

struct ThrumSink(Arc<Pcm>);

impl Sink for ThrumSink {
    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        if let AudioPacket::Samples(samples) = packet {
            self.0.push(&converter.f64_to_f32(&samples));
        }
        Ok(())
    }
}

/// The device's volume, which Thrum applies with its own slider.
struct ThrumMixer {
    volume: AtomicU16,
    out: Out,
}

impl Mixer for ThrumMixer {
    fn open(_: MixerConfig) -> Result<Self, librespot_core::Error> {
        Ok(ThrumMixer { volume: AtomicU16::new(u16::MAX / 2), out: Out::new() })
    }
    fn volume(&self) -> u16 {
        self.volume.load(Ordering::Relaxed)
    }
    fn set_volume(&self, volume: u16) {
        if self.volume.swap(volume, Ordering::Relaxed) != volume {
            self.out.send(&json!({"event": "connect", "volume": f64::from(volume) / f64::from(u16::MAX)}), &[]);
        }
    }
}

pub struct Device {
    spirc: Spirc,
    pub pcm: Arc<Pcm>,
}

impl Device {
    /// Signs session in through Spirc (which must do the signing in, after
    /// its listeners are set up) and makes it a Connect device called name.
    /// The future runs the device until it shuts down.
    pub async fn start(
        name: &str,
        session: Session,
        credentials: Credentials,
        out: Out,
    ) -> Result<(Device, impl std::future::Future<Output = ()>), librespot_core::Error> {
        let pcm = Arc::new(Pcm::default());
        let config = PlayerConfig { bitrate: Bitrate::Bitrate320, ..Default::default() };
        let sink_pcm = pcm.clone();
        let player = Player::new(config, session.clone(), Box::new(NoOpVolume), move || Box::new(ThrumSink(sink_pcm)));
        let mixer: Arc<dyn Mixer> = Arc::new(ThrumMixer::open(MixerConfig::default())?);
        let connect = ConnectConfig {
            name: name.to_string(),
            device_type: DeviceType::Computer,
            ..Default::default()
        };
        let events = player.get_player_event_channel();
        let (spirc, task) = Spirc::new(connect, session.clone(), credentials, player, mixer).await?;
        tokio::spawn(forward(events, session, pcm.clone(), out));
        Ok((Device { spirc, pcm }, task))
    }

    /// One of Thrum's buttons, as the phone's would press it.
    pub fn command(&self, action: &str, value: &Value) -> Result<(), librespot_core::Error> {
        match action {
            "play" => {
                self.pcm.set_hold(false);
                self.spirc.play()
            }
            "pause" => {
                self.pcm.set_hold(true);
                self.spirc.pause()
            }
            "next" => self.spirc.next(),
            "previous" => self.spirc.prev(),
            "seek" => self.spirc.set_position_ms(value.as_u64().unwrap_or(0).min(u32::MAX as u64) as u32),
            "volume" => {
                let v = value.as_f64().unwrap_or(0.5).clamp(0.0, 1.0);
                self.spirc.set_volume((v * f64::from(u16::MAX)).round() as u16)
            }
            _ => Ok(()),
        }
    }

    pub fn shutdown(&self) {
        self.pcm.close();
        let _ = self.spirc.shutdown();
    }
}

/// What a track is, for Thrum's display.
pub fn track_json(item: &AudioItem) -> Value {
    let (artists, album) = match &item.unique_fields {
        UniqueFields::Track { artists, album, .. } => {
            (artists.0.iter().map(|a| a.name.clone()).collect::<Vec<_>>(), album.clone())
        }
        UniqueFields::Episode { show_name, .. } => (vec![show_name.clone()], String::new()),
        _ => (Vec::new(), String::new()),
    };
    // The largest picture: Spotify lists them small to large.
    let art = item.covers.iter().max_by_key(|c| c.width).map(|c| c.url.clone()).unwrap_or_default();
    json!({
        "id": item.track_id.to_id().unwrap_or_default(),
        "uri": item.uri,
        "title": item.name,
        "artists": artists,
        "album": album,
        "duration_ms": item.duration_ms,
        "art": art,
    })
}

async fn forward(
    mut events: librespot_playback::player::PlayerEventChannel,
    _session: Session,
    pcm: Arc<Pcm>,
    out: Out,
) {
    while let Some(event) = events.recv().await {
        let news = match event {
            PlayerEvent::Playing { position_ms, .. } => {
                pcm.set_hold(false);
                json!({"event": "connect", "state": "playing", "position_ms": position_ms})
            }
            PlayerEvent::Paused { position_ms, .. } => {
                json!({"event": "connect", "state": "paused", "position_ms": position_ms})
            }
            PlayerEvent::Stopped { .. } => {
                pcm.clear();
                json!({"event": "connect", "state": "stopped"})
            }
            PlayerEvent::Seeked { position_ms, .. } => {
                pcm.clear();
                json!({"event": "connect", "position_ms": position_ms})
            }
            PlayerEvent::PositionCorrection { position_ms, .. } => {
                json!({"event": "connect", "position_ms": position_ms})
            }
            PlayerEvent::TrackChanged { audio_item } => json!({"event": "connect", "track": track_json(&audio_item)}),
            PlayerEvent::SessionClientChanged { client_name, .. } => {
                json!({"event": "connect", "client": client_name})
            }
            _ => continue,
        };
        if !out.send(&news, &[]) {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;

    #[test]
    fn the_sink_waits_for_thrum_to_take_the_sound() {
        let pcm = Arc::new(Pcm::default());
        let writer = {
            let pcm = pcm.clone();
            thread::spawn(move || {
                for _ in 0..4 {
                    pcm.push(&vec![0.25; CAPACITY / 2]);
                }
            })
        };
        // Two pushes fill it; the third waits.
        thread::sleep(Duration::from_millis(150));
        assert!(!writer.is_finished());
        let mut taken = 0;
        while taken < CAPACITY * 2 {
            taken += pcm.take(4096, Duration::from_millis(200)).len();
        }
        writer.join().unwrap();
        assert_eq!(taken, CAPACITY * 2);
    }

    #[test]
    fn a_pause_lets_the_player_finish_its_write() {
        let pcm = Pcm::default();
        pcm.push(&vec![0.0; CAPACITY]);
        pcm.set_hold(true);
        // Would wait for ever without the hold.
        pcm.push(&[0.5, 0.5]);
        assert_eq!(pcm.take(CAPACITY, Duration::ZERO).len(), CAPACITY + 2);
    }

    #[test]
    fn nothing_to_take_gives_nothing_after_the_wait() {
        let pcm = Pcm::default();
        assert!(pcm.take(4096, Duration::from_millis(10)).is_empty());
        pcm.push(&[0.1, 0.2, 0.3]);
        // Whole frames only.
        assert_eq!(pcm.take(4096, Duration::ZERO), vec![0.1, 0.2]);
    }
}
