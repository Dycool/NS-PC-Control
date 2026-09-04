use ns_shared::crypto::{hmac_sha256, hmac_verify};
use ns_shared::protocol::{S2_AUDIO_DIR_CLIENT_TO_CONSOLE, S2_AUDIO_PCM_BYTES, S2_AUDIO_PCM_MAGIC, S2_AUDIO_VERSION};
use std::io;
use std::net::{SocketAddr, UdpSocket};

const AUDIO_PCM_SIZE: usize = 36 + S2_AUDIO_PCM_BYTES;
const AUDIO_AUTH_SIZE: usize = AUDIO_PCM_SIZE - 16;

pub struct AudioClient { socket: UdpSocket, server: SocketAddr, key: [u8; 32], sequence: u32 }

impl AudioClient {
    pub fn new(socket: UdpSocket, server: SocketAddr, key: [u8; 32]) -> Self { Self { socket, server, key, sequence: 0 } }
    pub fn send_microphone_pcm(&mut self, timestamp_us: u64, pcm: &[u8; S2_AUDIO_PCM_BYTES]) -> io::Result<usize> {
        let mut packet = [0_u8; AUDIO_PCM_SIZE];
        packet[..4].copy_from_slice(&S2_AUDIO_PCM_MAGIC.to_le_bytes());
        packet[4] = S2_AUDIO_VERSION;
        packet[5] = S2_AUDIO_DIR_CLIENT_TO_CONSOLE;
        packet[6..8].copy_from_slice(&(S2_AUDIO_PCM_BYTES as u16).to_le_bytes());
        packet[8..12].copy_from_slice(&self.sequence.to_le_bytes());
        packet[12..20].copy_from_slice(&timestamp_us.to_le_bytes());
        packet[20..20 + S2_AUDIO_PCM_BYTES].copy_from_slice(pcm);
        let tag = hmac_sha256(&self.key, &packet[..AUDIO_AUTH_SIZE]);
        packet[AUDIO_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        self.sequence = self.sequence.wrapping_add(1);
        self.socket.send_to(&packet, self.server)
    }
    pub fn decode_playback<'a>(&self, packet: &'a [u8]) -> Option<&'a [u8]> {
        if packet.len() != AUDIO_PCM_SIZE
            || u32::from_le_bytes(packet[..4].try_into().ok()?) != S2_AUDIO_PCM_MAGIC
            || packet[4] != S2_AUDIO_VERSION
            || !hmac_verify(&self.key, &packet[..AUDIO_AUTH_SIZE], &packet[AUDIO_AUTH_SIZE..])
        { return None; }
        Some(&packet[20..20 + S2_AUDIO_PCM_BYTES])
    }
}
