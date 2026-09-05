use ns_shared::crypto::{hmac_sha256, hmac_verify};
use ns_shared::protocol::{
    S2_AUDIO_CAPS_MAGIC, S2_AUDIO_PCM_BYTES, S2_AUDIO_PCM_MAGIC, S2_AUDIO_VERSION,
};

pub const AUDIO_CAPS_SIZE: usize = 36;
pub const AUDIO_CAPS_AUTH_SIZE: usize = AUDIO_CAPS_SIZE - 16;
pub const AUDIO_PCM_SIZE: usize = 36 + S2_AUDIO_PCM_BYTES;
pub const AUDIO_PCM_AUTH_SIZE: usize = AUDIO_PCM_SIZE - 16;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct AudioCapabilities {
    flags: u8,
    sequence: u32,
    timestamp_us: u64,
}

impl AudioCapabilities {
    #[must_use]
    pub const fn new(flags: u8, sequence: u32, timestamp_us: u64) -> Self {
        Self { flags, sequence, timestamp_us }
    }

    #[must_use]
    pub const fn flags(&self) -> u8 { self.flags }

    #[must_use]
    pub const fn sequence(&self) -> u32 { self.sequence }

    #[must_use]
    pub const fn timestamp_us(&self) -> u64 { self.timestamp_us }

    #[must_use]
    pub fn encode(&self, key: &[u8]) -> [u8; AUDIO_CAPS_SIZE] {
        let mut output = [0_u8; AUDIO_CAPS_SIZE];
        output[..4].copy_from_slice(&S2_AUDIO_CAPS_MAGIC.to_le_bytes());
        output[4] = S2_AUDIO_VERSION;
        output[5] = self.flags;
        output[8..12].copy_from_slice(&self.sequence.to_le_bytes());
        output[12..20].copy_from_slice(&self.timestamp_us.to_le_bytes());
        let tag = hmac_sha256(key, &output[..AUDIO_CAPS_AUTH_SIZE]);
        output[AUDIO_CAPS_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        output
    }

    #[must_use]
    pub fn decode(bytes: &[u8], key: &[u8]) -> Option<Self> {
        if bytes.len() != AUDIO_CAPS_SIZE
            || u32::from_le_bytes(bytes[..4].try_into().ok()?) != S2_AUDIO_CAPS_MAGIC
            || bytes[4] != S2_AUDIO_VERSION
            || !hmac_verify(key, &bytes[..AUDIO_CAPS_AUTH_SIZE], &bytes[AUDIO_CAPS_AUTH_SIZE..])
        { return None; }
        Some(Self {
            flags: bytes[5],
            sequence: u32::from_le_bytes(bytes[8..12].try_into().ok()?),
            timestamp_us: u64::from_le_bytes(bytes[12..20].try_into().ok()?),
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AudioPcmPacket {
    direction: u8,
    sequence: u32,
    timestamp_us: u64,
    pcm: [u8; S2_AUDIO_PCM_BYTES],
}

impl AudioPcmPacket {
    #[must_use]
    pub const fn new(
        direction: u8,
        sequence: u32,
        timestamp_us: u64,
        pcm: [u8; S2_AUDIO_PCM_BYTES],
    ) -> Self {
        Self { direction, sequence, timestamp_us, pcm }
    }

    #[must_use]
    pub const fn direction(&self) -> u8 { self.direction }

    #[must_use]
    pub const fn sequence(&self) -> u32 { self.sequence }

    #[must_use]
    pub const fn timestamp_us(&self) -> u64 { self.timestamp_us }

    #[must_use]
    pub const fn pcm(&self) -> &[u8; S2_AUDIO_PCM_BYTES] { &self.pcm }

    #[must_use]
    pub fn encode(&self, key: &[u8]) -> [u8; AUDIO_PCM_SIZE] {
        let mut output = [0_u8; AUDIO_PCM_SIZE];
        output[..4].copy_from_slice(&S2_AUDIO_PCM_MAGIC.to_le_bytes());
        output[4] = S2_AUDIO_VERSION;
        output[5] = self.direction;
        output[6..8].copy_from_slice(&(S2_AUDIO_PCM_BYTES as u16).to_le_bytes());
        output[8..12].copy_from_slice(&self.sequence.to_le_bytes());
        output[12..20].copy_from_slice(&self.timestamp_us.to_le_bytes());
        output[20..20 + S2_AUDIO_PCM_BYTES].copy_from_slice(&self.pcm);
        let tag = hmac_sha256(key, &output[..AUDIO_PCM_AUTH_SIZE]);
        output[AUDIO_PCM_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        output
    }

    #[must_use]
    pub fn decode(bytes: &[u8], key: &[u8]) -> Option<Self> {
        if bytes.len() != AUDIO_PCM_SIZE
            || u32::from_le_bytes(bytes[..4].try_into().ok()?) != S2_AUDIO_PCM_MAGIC
            || bytes[4] != S2_AUDIO_VERSION
            || usize::from(u16::from_le_bytes(bytes[6..8].try_into().ok()?)) != S2_AUDIO_PCM_BYTES
            || !hmac_verify(key, &bytes[..AUDIO_PCM_AUTH_SIZE], &bytes[AUDIO_PCM_AUTH_SIZE..])
        { return None; }
        let mut pcm = [0_u8; S2_AUDIO_PCM_BYTES];
        pcm.copy_from_slice(&bytes[20..20 + S2_AUDIO_PCM_BYTES]);
        Some(Self {
            direction: bytes[5],
            sequence: u32::from_le_bytes(bytes[8..12].try_into().ok()?),
            timestamp_us: u64::from_le_bytes(bytes[12..20].try_into().ok()?),
            pcm,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::{AudioCapabilities, AudioPcmPacket};
    use ns_shared::crypto::derive_key;
    use ns_shared::protocol::S2_AUDIO_PCM_BYTES;

    #[test]
    fn audio_wire_round_trips() {
        let key = derive_key("audio");
        let caps = AudioCapabilities::new(3, 7, 99);
        assert_eq!(AudioCapabilities::decode(&caps.encode(&key), &key), Some(caps));
        let packet = AudioPcmPacket::new(1, 8, 100, [0x55; S2_AUDIO_PCM_BYTES]);
        assert_eq!(AudioPcmPacket::decode(&packet.encode(&key), &key), Some(packet));
    }
}
