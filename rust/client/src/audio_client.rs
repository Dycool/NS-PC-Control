use ns_shared::crypto::{hmac_sha256, hmac_verify};
use ns_shared::protocol::{
    S2_AUDIO_CAPS_AUTH_SIZE, S2_AUDIO_CAPS_MAGIC, S2_AUDIO_CAPS_PACKET_SIZE,
    S2_AUDIO_DIR_CLIENT_TO_CONSOLE, S2_AUDIO_DIR_CONSOLE_TO_CLIENT, S2_AUDIO_PCM_AUTH_SIZE,
    S2_AUDIO_PCM_BYTES, S2_AUDIO_PCM_MAGIC, S2_AUDIO_PCM_PACKET_SIZE, S2_AUDIO_USB_FRAME_BYTES,
    S2_AUDIO_VERSION,
};
use std::io;
use std::net::{SocketAddr, UdpSocket};

const PLAYBACK_MIN_TARGET_MS: f64 = 10.0;
const PLAYBACK_MAX_TARGET_MS: f64 = 150.0;
const PLAYBACK_JITTER_MULT: f64 = 2.5;
const PLAYBACK_MAX_CONCEAL_DATAGRAMS: u32 = 8;
const PLAYBACK_MIN_RATIO: f64 = 0.9975;
const PLAYBACK_MAX_RATIO: f64 = 1.0025;
const PLAYBACK_TRIM_COEFF: f64 = 0.0005;
const MICROPHONE_MAX_MS: usize = 40;
const MICROPHONE_MAX_PACKETS_PER_PUMP: usize = 4;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlaybackPacket<'a> {
    pub sequence: u32,
    pub timestamp_us: u64,
    pub pcm: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlaybackDecision {
    pub conceal_datagrams: u32,
    pub target_ms: f64,
    pub frequency_ratio: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MicrophonePumpPlan {
    Idle,
    ClearBacklog,
    SendPackets(usize),
}

/// Mirror the C++ microphone pump's backlog policy without tying the state
/// machine to SDL. One audio USB frame is exactly one millisecond, and the C++
/// client drops the entire capture queue only once it grows strictly beyond
/// 40 ms. Otherwise, each update sends at most four complete UDP payloads.
#[must_use]
pub fn microphone_pump_plan(available_bytes: i32) -> MicrophonePumpPlan {
    let available = usize::try_from(available_bytes).unwrap_or(0);
    if available > S2_AUDIO_USB_FRAME_BYTES * MICROPHONE_MAX_MS {
        return MicrophonePumpPlan::ClearBacklog;
    }

    let packets = (available / S2_AUDIO_PCM_BYTES).min(MICROPHONE_MAX_PACKETS_PER_PUMP);
    if packets == 0 {
        MicrophonePumpPlan::Idle
    } else {
        MicrophonePumpPlan::SendPackets(packets)
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct PlaybackState {
    last_sequence: Option<u32>,
    jitter_last_arrival_us: u64,
    jitter_last_send_us: u64,
    have_jitter_sample: bool,
    jitter_estimate_us: f64,
}

impl PlaybackState {
    #[must_use]
    pub fn accept(
        &mut self,
        packet: PlaybackPacket<'_>,
        arrival_us: u64,
        queued_ms: f64,
    ) -> Option<PlaybackDecision> {
        let delta = match self.last_sequence {
            None => 1_i32,
            Some(previous) => packet.sequence.wrapping_sub(previous) as i32,
        };
        if delta <= 0 {
            return None;
        }

        if self.have_jitter_sample {
            let arrival_delta = i128::from(arrival_us) - i128::from(self.jitter_last_arrival_us);
            let send_delta = i128::from(packet.timestamp_us) - i128::from(self.jitter_last_send_us);
            let transit_delta = (arrival_delta - send_delta).unsigned_abs() as f64;
            self.jitter_estimate_us += (transit_delta - self.jitter_estimate_us) / 16.0;
        }
        self.jitter_last_arrival_us = arrival_us;
        self.jitter_last_send_us = packet.timestamp_us;
        self.have_jitter_sample = true;

        let jitter_ms = self.jitter_estimate_us / 1000.0;
        let target_ms = (PLAYBACK_MIN_TARGET_MS + PLAYBACK_JITTER_MULT * jitter_ms)
            .clamp(PLAYBACK_MIN_TARGET_MS, PLAYBACK_MAX_TARGET_MS);
        let conceal_datagrams = if self.last_sequence.is_some() {
            u32::try_from(delta - 1)
                .unwrap_or(PLAYBACK_MAX_CONCEAL_DATAGRAMS)
                .min(PLAYBACK_MAX_CONCEAL_DATAGRAMS)
        } else {
            0
        };

        self.last_sequence = Some(packet.sequence);
        let frequency_ratio = (1.0 + (queued_ms - target_ms) * PLAYBACK_TRIM_COEFF)
            .clamp(PLAYBACK_MIN_RATIO, PLAYBACK_MAX_RATIO);
        Some(PlaybackDecision {
            conceal_datagrams,
            target_ms,
            frequency_ratio,
        })
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    #[must_use]
    pub const fn jitter_estimate_us(&self) -> f64 {
        self.jitter_estimate_us
    }
}

pub struct AudioClient {
    socket: UdpSocket,
    server: SocketAddr,
    key: [u8; 32],
    microphone_sequence: u32,
    capabilities_sequence: u32,
}

impl AudioClient {
    pub fn new(socket: UdpSocket, server: SocketAddr, key: [u8; 32]) -> Self {
        Self {
            socket,
            server,
            key,
            microphone_sequence: 0,
            capabilities_sequence: 0,
        }
    }

    pub fn send_microphone_pcm(
        &mut self,
        timestamp_us: u64,
        pcm: &[u8; S2_AUDIO_PCM_BYTES],
    ) -> io::Result<usize> {
        let mut packet = [0_u8; S2_AUDIO_PCM_PACKET_SIZE];
        packet[..4].copy_from_slice(&S2_AUDIO_PCM_MAGIC.to_le_bytes());
        packet[4] = S2_AUDIO_VERSION;
        packet[5] = S2_AUDIO_DIR_CLIENT_TO_CONSOLE;
        packet[6..8].copy_from_slice(&(S2_AUDIO_PCM_BYTES as u16).to_le_bytes());
        packet[8..12].copy_from_slice(&self.microphone_sequence.to_le_bytes());
        packet[12..20].copy_from_slice(&timestamp_us.to_le_bytes());
        packet[20..20 + S2_AUDIO_PCM_BYTES].copy_from_slice(pcm);
        let tag = hmac_sha256(&self.key, &packet[..S2_AUDIO_PCM_AUTH_SIZE]);
        packet[S2_AUDIO_PCM_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        self.microphone_sequence = self.microphone_sequence.wrapping_add(1);
        self.socket.send_to(&packet, self.server)
    }

    pub fn send_capabilities(&mut self, timestamp_us: u64, flags: u8) -> io::Result<usize> {
        let mut packet = [0_u8; S2_AUDIO_CAPS_PACKET_SIZE];
        packet[..4].copy_from_slice(&S2_AUDIO_CAPS_MAGIC.to_le_bytes());
        packet[4] = S2_AUDIO_VERSION;
        packet[5] = flags;
        packet[8..12].copy_from_slice(&self.capabilities_sequence.to_le_bytes());
        packet[12..20].copy_from_slice(&timestamp_us.to_le_bytes());
        let tag = hmac_sha256(&self.key, &packet[..S2_AUDIO_CAPS_AUTH_SIZE]);
        packet[S2_AUDIO_CAPS_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        self.capabilities_sequence = self.capabilities_sequence.wrapping_add(1);
        self.socket.send_to(&packet, self.server)
    }

    #[must_use]
    pub fn decode_playback<'a>(&self, packet: &'a [u8]) -> Option<PlaybackPacket<'a>> {
        if packet.len() != S2_AUDIO_PCM_PACKET_SIZE
            || u32::from_le_bytes(packet[..4].try_into().ok()?) != S2_AUDIO_PCM_MAGIC
            || packet[4] != S2_AUDIO_VERSION
            || packet[5] != S2_AUDIO_DIR_CONSOLE_TO_CLIENT
            || u16::from_le_bytes(packet[6..8].try_into().ok()?) != S2_AUDIO_PCM_BYTES as u16
            || !hmac_verify(
                &self.key,
                &packet[..S2_AUDIO_PCM_AUTH_SIZE],
                &packet[S2_AUDIO_PCM_AUTH_SIZE..],
            )
        {
            return None;
        }

        Some(PlaybackPacket {
            sequence: u32::from_le_bytes(packet[8..12].try_into().ok()?),
            timestamp_us: u64::from_le_bytes(packet[12..20].try_into().ok()?),
            pcm: &packet[20..20 + S2_AUDIO_PCM_BYTES],
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{IpAddr, Ipv4Addr};

    fn client(key: [u8; 32]) -> AudioClient {
        let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).expect("bind UDP test socket");
        let server = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 9);
        AudioClient::new(socket, server, key)
    }

    fn playback_packet(
        key: &[u8; 32],
        sequence: u32,
        timestamp_us: u64,
    ) -> [u8; S2_AUDIO_PCM_PACKET_SIZE] {
        let mut packet = [0_u8; S2_AUDIO_PCM_PACKET_SIZE];
        packet[..4].copy_from_slice(&S2_AUDIO_PCM_MAGIC.to_le_bytes());
        packet[4] = S2_AUDIO_VERSION;
        packet[5] = S2_AUDIO_DIR_CONSOLE_TO_CLIENT;
        packet[6..8].copy_from_slice(&(S2_AUDIO_PCM_BYTES as u16).to_le_bytes());
        packet[8..12].copy_from_slice(&sequence.to_le_bytes());
        packet[12..20].copy_from_slice(&timestamp_us.to_le_bytes());
        let tag = hmac_sha256(key, &packet[..S2_AUDIO_PCM_AUTH_SIZE]);
        packet[S2_AUDIO_PCM_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        packet
    }

    #[test]
    fn microphone_pump_matches_cpp_backlog_and_batch_boundaries() {
        assert_eq!(microphone_pump_plan(-1), MicrophonePumpPlan::Idle);
        assert_eq!(microphone_pump_plan(0), MicrophonePumpPlan::Idle);
        assert_eq!(
            microphone_pump_plan((S2_AUDIO_PCM_BYTES - 1) as i32),
            MicrophonePumpPlan::Idle
        );
        assert_eq!(
            microphone_pump_plan(S2_AUDIO_PCM_BYTES as i32),
            MicrophonePumpPlan::SendPackets(1)
        );
        assert_eq!(
            microphone_pump_plan((S2_AUDIO_PCM_BYTES * 4) as i32),
            MicrophonePumpPlan::SendPackets(4)
        );
        assert_eq!(
            microphone_pump_plan((S2_AUDIO_USB_FRAME_BYTES * MICROPHONE_MAX_MS) as i32),
            MicrophonePumpPlan::SendPackets(4)
        );
        assert_eq!(
            microphone_pump_plan((S2_AUDIO_USB_FRAME_BYTES * MICROPHONE_MAX_MS + 1) as i32),
            MicrophonePumpPlan::ClearBacklog
        );
    }

    #[test]
    fn microphone_pump_never_sends_more_than_four_packets_per_update() {
        assert_eq!(
            microphone_pump_plan((S2_AUDIO_PCM_BYTES * 7) as i32),
            MicrophonePumpPlan::SendPackets(4)
        );
    }

    #[test]
    fn playback_decoder_requires_console_direction_and_exact_payload_size() {
        let key = [0x5a; 32];
        let audio = client(key);
        let mut packet = playback_packet(&key, 7, 123_456);
        let decoded = audio.decode_playback(&packet).expect("valid playback packet");
        assert_eq!(decoded.sequence, 7);
        assert_eq!(decoded.timestamp_us, 123_456);
        assert_eq!(decoded.pcm.len(), S2_AUDIO_PCM_BYTES);

        packet[5] = S2_AUDIO_DIR_CLIENT_TO_CONSOLE;
        let tag = hmac_sha256(&key, &packet[..S2_AUDIO_PCM_AUTH_SIZE]);
        packet[S2_AUDIO_PCM_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        assert!(audio.decode_playback(&packet).is_none());

        packet[5] = S2_AUDIO_DIR_CONSOLE_TO_CLIENT;
        packet[6..8].copy_from_slice(&0_u16.to_le_bytes());
        let tag = hmac_sha256(&key, &packet[..S2_AUDIO_PCM_AUTH_SIZE]);
        packet[S2_AUDIO_PCM_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        assert!(audio.decode_playback(&packet).is_none());
    }

    #[test]
    fn playback_state_rejects_duplicates_and_late_packets() {
        let pcm = [0_u8; S2_AUDIO_PCM_BYTES];
        let mut state = PlaybackState::default();
        let first = PlaybackPacket {
            sequence: 10,
            timestamp_us: 1_000,
            pcm: &pcm,
        };
        assert!(state.accept(first, 2_000, 10.0).is_some());
        assert!(state.accept(first, 2_100, 10.0).is_none());
        let late = PlaybackPacket {
            sequence: 9,
            timestamp_us: 900,
            pcm: &pcm,
        };
        assert!(state.accept(late, 2_200, 10.0).is_none());
    }

    #[test]
    fn playback_state_caps_gap_concealment_at_eight_datagrams() {
        let pcm = [0_u8; S2_AUDIO_PCM_BYTES];
        let mut state = PlaybackState::default();
        let first = PlaybackPacket {
            sequence: 1,
            timestamp_us: 1_000,
            pcm: &pcm,
        };
        let next = PlaybackPacket {
            sequence: 20,
            timestamp_us: 6_000,
            pcm: &pcm,
        };
        state.accept(first, 2_000, 10.0).expect("first packet");
        let decision = state.accept(next, 7_000, 10.0).expect("forward packet");
        assert_eq!(decision.conceal_datagrams, 8);
    }

    #[test]
    fn playback_state_tracks_rfc3550_style_jitter_and_trim_limits() {
        let pcm = [0_u8; S2_AUDIO_PCM_BYTES];
        let mut state = PlaybackState::default();
        state.accept(
            PlaybackPacket {
                sequence: 1,
                timestamp_us: 10_000,
                pcm: &pcm,
            },
            20_000,
            10.0,
        );
        let decision = state
            .accept(
                PlaybackPacket {
                    sequence: 2,
                    timestamp_us: 15_000,
                    pcm: &pcm,
                },
                27_000,
                500.0,
            )
            .expect("second packet");
        assert_eq!(state.jitter_estimate_us(), 125.0);
        assert!((decision.target_ms - 10.3125).abs() < f64::EPSILON);
        assert_eq!(decision.frequency_ratio, PLAYBACK_MAX_RATIO);

        let decision = state
            .accept(
                PlaybackPacket {
                    sequence: 3,
                    timestamp_us: 20_000,
                    pcm: &pcm,
                },
                32_000,
                0.0,
            )
            .expect("third packet");
        assert_eq!(decision.frequency_ratio, PLAYBACK_MIN_RATIO);
    }

    #[test]
    fn playback_state_accepts_wrapped_sequence_numbers() {
        let pcm = [0_u8; S2_AUDIO_PCM_BYTES];
        let mut state = PlaybackState::default();
        state.accept(
            PlaybackPacket {
                sequence: u32::MAX,
                timestamp_us: 1_000,
                pcm: &pcm,
            },
            2_000,
            10.0,
        );
        let decision = state
            .accept(
                PlaybackPacket {
                    sequence: 0,
                    timestamp_us: 6_000,
                    pcm: &pcm,
                },
                7_000,
                10.0,
            )
            .expect("wrapped sequence advances by one");
        assert_eq!(decision.conceal_datagrams, 0);
    }
}
