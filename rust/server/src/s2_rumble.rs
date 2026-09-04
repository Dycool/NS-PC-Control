pub const RUMBLE_GAIN_PERCENT: u16 = 40;
pub const RUMBLE_MIN_NONZERO: u8 = 1;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct DecodedRumble {
    left: u8,
    right: u8,
}

impl DecodedRumble {
    #[must_use]
    pub const fn new(left: u8, right: u8) -> Self {
        Self { left, right }
    }

    #[must_use]
    pub const fn left(self) -> u8 {
        self.left
    }

    #[must_use]
    pub const fn right(self) -> u8 {
        self.right
    }
}

#[must_use]
pub fn decode_s2_rumble(packet: &[u8]) -> Option<DecodedRumble> {
    if packet.len() < 7 || !matches!(packet[0], 0x01 | 0x02) {
        return None;
    }
    let left = motor_amplitude(packet.get(2..7)?)?;
    let right = if packet[0] == 0x02 && packet.len() >= 23 {
        motor_amplitude(packet.get(18..23)?)?
    } else {
        left
    };
    Some(DecodedRumble::new(left, right))
}

fn motor_amplitude(bytes: &[u8]) -> Option<u8> {
    let bytes: &[u8; 5] = bytes.try_into().ok()?;
    let packed = u64::from(bytes[0])
        | (u64::from(bytes[1]) << 8)
        | (u64::from(bytes[2]) << 16)
        | (u64::from(bytes[3]) << 24)
        | (u64::from(bytes[4]) << 32);
    let amplitude0 = ((packed >> 10) & 0x03ff) as u16;
    let amplitude1 = ((packed >> 30) & 0x03ff) as u16;
    Some(scale_capture_delta(u8::try_from(amplitude0.max(amplitude1) >> 2).ok()?))
}

fn scale_capture_delta(value: u8) -> u8 {
    let scaled = u16::from(value) * RUMBLE_GAIN_PERCENT / 100;
    if scaled == 0 {
        0
    } else {
        u8::try_from(scaled.max(u16::from(RUMBLE_MIN_NONZERO)).min(255)).unwrap_or(255)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn encode_motor(amplitude0: u16, amplitude1: u16) -> [u8; 5] {
        let packed = (u64::from(amplitude0 & 0x03ff) << 10)
            | (u64::from(amplitude1 & 0x03ff) << 30)
            | 0x155
            | (0x2aau64 << 20);
        let bytes = packed.to_le_bytes();
        [bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]]
    }

    #[test]
    fn frequencies_do_not_create_idle_buzz() {
        let mut packet = vec![0u8; 7];
        packet[0] = 0x01;
        packet[2..7].copy_from_slice(&encode_motor(0, 0));
        assert_eq!(decode_s2_rumble(&packet), Some(DecodedRumble::new(0, 0)));
    }

    #[test]
    fn joycon_single_motor_is_mirrored_to_both_client_channels() {
        let mut packet = vec![0u8; 7];
        packet[0] = 0x01;
        packet[2..7].copy_from_slice(&encode_motor(400, 800));
        let expected = scale_capture_delta((800 >> 2) as u8);
        assert_eq!(decode_s2_rumble(&packet), Some(DecodedRumble::new(expected, expected)));
    }

    #[test]
    fn pro2_uses_second_lra_block_for_right_channel() {
        let mut packet = vec![0u8; 23];
        packet[0] = 0x02;
        packet[2..7].copy_from_slice(&encode_motor(200, 300));
        packet[18..23].copy_from_slice(&encode_motor(900, 100));
        let left = scale_capture_delta((300 >> 2) as u8);
        let right = scale_capture_delta((900 >> 2) as u8);
        assert_eq!(decode_s2_rumble(&packet), Some(DecodedRumble::new(left, right)));
    }

    #[test]
    fn rejects_non_s2_or_short_output_reports() {
        assert_eq!(decode_s2_rumble(&[]), None);
        assert_eq!(decode_s2_rumble(&[0x10; 7]), None);
        assert_eq!(decode_s2_rumble(&[0x02; 6]), None);
    }
}
