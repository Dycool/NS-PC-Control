use crate::crypto::{hmac_sha256, hmac_verify};

pub const JOYCON_MOUSE_MAGIC: u32 = 0x4e53_4a4d;
pub const JOYCON_MOUSE_VERSION: u8 = 3;
pub const JOYCON_MOUSE_FLAG_ACTIVE: u8 = 0x01;
pub const JOYCON_MOUSE_FLAG_LEFT_BUTTON: u8 = 0x02;
pub const JOYCON_MOUSE_FLAG_RIGHT_BUTTON: u8 = 0x04;
pub const JOYCON_MOUSE_AUTH_SIZE: usize = 31;
pub const JOYCON_MOUSE_PACKET_SIZE: usize = JOYCON_MOUSE_AUTH_SIZE + 16;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct JoyconMousePacket {
    flags: u8,
    subpad: u8,
    sequence: u32,
    delta_x: i32,
    delta_y: i32,
    scroll_y: i32,
    timestamp_us: u64,
}

impl JoyconMousePacket {
    #[must_use]
    pub const fn new(
        flags: u8,
        subpad: u8,
        sequence: u32,
        delta_x: i32,
        delta_y: i32,
        scroll_y: i32,
        timestamp_us: u64,
    ) -> Self {
        Self {
            flags,
            subpad,
            sequence,
            delta_x,
            delta_y,
            scroll_y,
            timestamp_us,
        }
    }

    #[must_use]
    pub const fn flags(self) -> u8 {
        self.flags
    }

    #[must_use]
    pub const fn subpad(self) -> u8 {
        self.subpad
    }

    #[must_use]
    pub const fn sequence(self) -> u32 {
        self.sequence
    }

    #[must_use]
    pub const fn delta_x(self) -> i32 {
        self.delta_x
    }

    #[must_use]
    pub const fn delta_y(self) -> i32 {
        self.delta_y
    }

    #[must_use]
    pub const fn scroll_y(self) -> i32 {
        self.scroll_y
    }

    #[must_use]
    pub const fn timestamp_us(self) -> u64 {
        self.timestamp_us
    }

    #[must_use]
    pub const fn active(self) -> bool {
        self.flags & JOYCON_MOUSE_FLAG_ACTIVE != 0
    }

    #[must_use]
    pub const fn left_down(self) -> bool {
        self.active() && self.flags & JOYCON_MOUSE_FLAG_LEFT_BUTTON != 0
    }

    #[must_use]
    pub const fn right_down(self) -> bool {
        self.active() && self.flags & JOYCON_MOUSE_FLAG_RIGHT_BUTTON != 0
    }

    #[must_use]
    pub fn encode_authenticated(self, key: &[u8; 32]) -> [u8; JOYCON_MOUSE_PACKET_SIZE] {
        let mut output = [0u8; JOYCON_MOUSE_PACKET_SIZE];
        output[0..4].copy_from_slice(&JOYCON_MOUSE_MAGIC.to_le_bytes());
        output[4] = JOYCON_MOUSE_VERSION;
        output[5] = self.flags;
        output[6] = self.subpad;
        output[7..11].copy_from_slice(&self.sequence.to_le_bytes());
        output[11..15].copy_from_slice(&self.delta_x.to_le_bytes());
        output[15..19].copy_from_slice(&self.delta_y.to_le_bytes());
        output[19..23].copy_from_slice(&self.scroll_y.to_le_bytes());
        output[23..31].copy_from_slice(&self.timestamp_us.to_le_bytes());
        let tag = hmac_sha256(key, &output[..JOYCON_MOUSE_AUTH_SIZE]);
        output[JOYCON_MOUSE_AUTH_SIZE..].copy_from_slice(&tag[..16]);
        output
    }

    #[must_use]
    pub fn decode_authenticated(bytes: &[u8], key: &[u8; 32]) -> Option<Self> {
        if bytes.len() != JOYCON_MOUSE_PACKET_SIZE
            || u32::from_le_bytes(bytes[0..4].try_into().ok()?) != JOYCON_MOUSE_MAGIC
            || bytes[4] != JOYCON_MOUSE_VERSION
            || bytes[6] >= 4
            || !hmac_verify(
                key,
                &bytes[..JOYCON_MOUSE_AUTH_SIZE],
                &bytes[JOYCON_MOUSE_AUTH_SIZE..],
            )
        {
            return None;
        }
        Some(Self {
            flags: bytes[5],
            subpad: bytes[6],
            sequence: u32::from_le_bytes(bytes[7..11].try_into().ok()?),
            delta_x: i32::from_le_bytes(bytes[11..15].try_into().ok()?),
            delta_y: i32::from_le_bytes(bytes[15..19].try_into().ok()?),
            scroll_y: i32::from_le_bytes(bytes[19..23].try_into().ok()?),
            timestamp_us: u64::from_le_bytes(bytes[23..31].try_into().ok()?),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packet_is_cpp_packed_size_and_round_trips() {
        let key = [0x5a; 32];
        let packet = JoyconMousePacket::new(
            JOYCON_MOUSE_FLAG_ACTIVE | JOYCON_MOUSE_FLAG_LEFT_BUTTON,
            2,
            u32::MAX,
            -123_456,
            654_321,
            -120,
            0x0102_0304_0506_0708,
        );
        let encoded = packet.encode_authenticated(&key);
        assert_eq!(encoded.len(), 47);
        assert_eq!(JoyconMousePacket::decode_authenticated(&encoded, &key), Some(packet));
    }

    #[test]
    fn authentication_version_and_subpad_are_fail_closed() {
        let key = [7; 32];
        let packet = JoyconMousePacket::new(JOYCON_MOUSE_FLAG_ACTIVE, 0, 1, 2, 3, 0, 4);
        let mut encoded = packet.encode_authenticated(&key);
        encoded[11] ^= 1;
        assert!(JoyconMousePacket::decode_authenticated(&encoded, &key).is_none());

        let mut wrong_version = packet.encode_authenticated(&key);
        wrong_version[4] = JOYCON_MOUSE_VERSION.wrapping_add(1);
        assert!(JoyconMousePacket::decode_authenticated(&wrong_version, &key).is_none());

        let mut wrong_subpad = packet.encode_authenticated(&key);
        wrong_subpad[6] = 4;
        assert!(JoyconMousePacket::decode_authenticated(&wrong_subpad, &key).is_none());
    }
}
