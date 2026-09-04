//! Compatibility helpers used while the C++ front-end/back-end are being ported.
//!
//! These helpers deliberately expose mutation only through checked methods; the
//! protocol structs themselves keep their fields crate-private and wire encoding
//! remains explicit rather than relying on memory layout.

use crate::crypto::{hmac_sha256, hmac_verify};
use crate::protocol::{
    ControllerType, HidReport, HoriHidReport, MultiReport, Packet, RumblePacket, WireError,
    HMAC_TAG_SIZE, PACKET_AUTH_SIZE, PACKET_SIZE,
};

impl TryFrom<u8> for ControllerType {
    type Error = WireError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Default),
            1 => Ok(Self::JoyconL),
            2 => Ok(Self::JoyconR),
            3 => Ok(Self::Pro),
            4 => Ok(Self::JoyconPair),
            5 => Ok(Self::Hori),
            6 => Ok(Self::ProS2),
            7 => Ok(Self::JoyconLS2),
            8 => Ok(Self::JoyconRS2),
            9 => Ok(Self::JoyconPairS2),
            other => Err(WireError::InvalidValue("controller type", other)),
        }
    }
}

impl HoriHidReport {
    pub const WIRE_SIZE: usize = crate::protocol::HORI_REPORT_SIZE;

    pub fn set_buttons(&mut self, buttons: u16) {
        self.buttons = buttons;
    }

    pub fn set_hat(&mut self, hat: crate::protocol::Hat) {
        self.hat = hat;
    }

    pub fn set_sticks(&mut self, lx: u8, ly: u8, rx: u8, ry: u8) {
        self.lx = lx;
        self.ly = ly;
        self.rx = rx;
        self.ry = ry;
    }

    pub fn set_vendor(&mut self, vendor: u8) {
        self.vendor = vendor;
    }

    #[must_use]
    pub const fn sticks(&self) -> [u8; 4] {
        self.axes()
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    #[must_use]
    pub fn to_wire(&self) -> [u8; crate::protocol::HORI_REPORT_SIZE] {
        self.encode()
    }

    pub fn from_wire(bytes: &[u8]) -> Result<Self, WireError> {
        Self::decode(bytes)
    }
}

impl HidReport {
    #[must_use]
    pub fn input_mut(&mut self) -> &mut HoriHidReport {
        &mut self.input
    }

    #[must_use]
    pub const fn status_bytes(&self) -> [u8; 3] {
        self.reserved
    }

    #[must_use]
    pub const fn requested_profile_raw(&self) -> u8 {
        self.reserved[2]
    }

    #[must_use]
    pub fn neutral_preserving_requested_profile(&self) -> Self {
        let mut neutral = Self::default();
        neutral.reserved[2] = self.reserved[2];
        neutral
    }

    pub fn controller_type(&self) -> Result<ControllerType, WireError> {
        ControllerType::try_from(self.reserved[2])
    }

    pub fn set_motion(&mut self, samples: [crate::protocol::MotionReport; 3], has_motion: bool) {
        self.motion = samples;
        self.has_motion = has_motion;
    }

    pub fn set_status_bytes(&mut self, status: [u8; 3]) -> Result<(), WireError> {
        let _ = ControllerType::try_from(status[2])?;
        self.reserved = status;
        Ok(())
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn from_wire(bytes: &[u8]) -> Result<Self, WireError> {
        Self::decode(bytes)
    }
}

impl MultiReport {
    #[must_use]
    pub const fn pad(&self, index: usize) -> Option<&HidReport> {
        if index < self.pads.len() {
            Some(&self.pads[index])
        } else {
            None
        }
    }

    #[must_use]
    pub fn pad_mut(&mut self, index: usize) -> Option<&mut HidReport> {
        self.pads.get_mut(index)
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }
}

impl Packet {
    pub fn set_flags(&mut self, flags: u8) {
        self.flags = flags;
    }

    pub fn set_sequence(&mut self, sequence: u32) {
        self.seq = sequence;
    }

    pub fn set_timestamp_us(&mut self, timestamp_us: u64) {
        self.ts_us = timestamp_us;
    }

    pub fn set_version(&mut self, version: u8) -> Result<(), WireError> {
        if matches!(
            version,
            crate::protocol::PROTO_VERSION
                | crate::protocol::WEB_PROTO_VERSION
                | crate::protocol::WEB_PROTO_VERSION_3
        ) {
            self.version = version;
            Ok(())
        } else {
            Err(WireError::InvalidValue("protocol version", version))
        }
    }

    #[must_use]
    pub fn report_mut(&mut self) -> &mut MultiReport {
        &mut self.report
    }

    pub fn encode_authenticated(&self, key: &[u8]) -> Result<[u8; PACKET_SIZE], WireError> {
        let mut packet = *self;
        let auth = packet.authenticated_bytes();
        let digest = hmac_sha256(key, &auth);
        let mut tag = [0u8; HMAC_TAG_SIZE];
        tag.copy_from_slice(&digest[..HMAC_TAG_SIZE]);
        packet.set_hmac(tag);
        Ok(packet.encode())
    }

    pub fn decode_authenticated(bytes: &[u8], key: &[u8]) -> Result<Self, WireError> {
        if bytes.len() != PACKET_SIZE {
            return Err(WireError::InvalidLength {
                expected: PACKET_SIZE,
                actual: bytes.len(),
            });
        }
        if !hmac_verify(key, &bytes[..PACKET_AUTH_SIZE], &bytes[PACKET_AUTH_SIZE..]) {
            return Err(WireError::InvalidValue("packet hmac", 0));
        }
        let packet = Self::decode(bytes)?;
        if !matches!(
            packet.version(),
            crate::protocol::PROTO_VERSION
                | crate::protocol::WEB_PROTO_VERSION
                | crate::protocol::WEB_PROTO_VERSION_3
        ) {
            return Err(WireError::InvalidValue("protocol version", packet.version()));
        }
        Ok(packet)
    }
}

impl Default for RumblePacket {
    fn default() -> Self {
        Self::new(0, 0, 0, 0)
    }
}

impl RumblePacket {
    pub const WIRE_SIZE: usize = crate::protocol::RUMBLE_PACKET_SIZE;

    #[must_use]
    pub const fn subpad(&self) -> u8 {
        self.subpad
    }

    #[must_use]
    pub const fn amplitudes(&self) -> [u8; 2] {
        [self.low_freq, self.high_freq]
    }

    #[must_use]
    pub const fn duration_10ms(&self) -> u8 {
        self.duration_10ms
    }

    #[must_use]
    pub fn to_wire(&self) -> [u8; crate::protocol::RUMBLE_PACKET_SIZE] {
        self.encode()
    }

    pub fn from_wire(bytes: &[u8]) -> Result<Self, WireError> {
        Self::decode(bytes)
    }
}
