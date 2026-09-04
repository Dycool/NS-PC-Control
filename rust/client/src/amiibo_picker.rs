use ns_shared::crypto::hmac_sha256;
use ns_shared::protocol::{AMIIBO_LIBRARY_MAGIC, AMIIBO_LIBRARY_VERSION};

pub const AMIIBO_LIBRARY_SELECT: u8 = 1;
pub const AMIIBO_LIBRARY_CLEAR: u8 = 2;
pub const AMIIBO_LIBRARY_PACKET_SIZE: usize = 32;
pub const AMIIBO_LIBRARY_AUTH_SIZE: usize = 16;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AmiiboSelection { action: u8, subpad: u8, head: u32, tail: u32 }

impl AmiiboSelection {
    pub fn select(subpad: u8, head: u32, tail: u32) -> Result<Self, String> {
        if subpad >= 4 || (head == 0 && tail == 0) { return Err("invalid Amiibo selection".to_string()); }
        Ok(Self { action: AMIIBO_LIBRARY_SELECT, subpad, head, tail })
    }
    pub fn clear(subpad: u8) -> Result<Self, String> {
        if subpad >= 4 { return Err("invalid controller slot".to_string()); }
        Ok(Self { action: AMIIBO_LIBRARY_CLEAR, subpad, head: 0, tail: 0 })
    }
    pub fn encode(&self, key: &[u8]) -> [u8; AMIIBO_LIBRARY_PACKET_SIZE] {
        let mut output = [0_u8; AMIIBO_LIBRARY_PACKET_SIZE];
        output[..4].copy_from_slice(&AMIIBO_LIBRARY_MAGIC.to_le_bytes());
        output[4] = AMIIBO_LIBRARY_VERSION;
        output[5] = self.action;
        output[6] = self.subpad;
        output[8..12].copy_from_slice(&self.head.to_le_bytes());
        output[12..16].copy_from_slice(&self.tail.to_le_bytes());
        let tag = hmac_sha256(key, &output[..AMIIBO_LIBRARY_AUTH_SIZE]);
        output[AMIIBO_LIBRARY_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        output
    }
}
