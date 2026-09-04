//! Byte-compatible encoding for the C++ `shared/protocol.hpp` wire protocol.
//!
//! The original implementation used packed C++ structs. This port intentionally
//! serializes every integer explicitly in little-endian order, so no unaligned
//! references, raw pointers, transmutation, or layout assumptions are required.

use core::fmt;

pub const PROTO_MAGIC: u32 = 0x4e53_5743;
pub const PROTO_VERSION: u8 = 4;
pub const WEB_PROTO_VERSION: u8 = 5;
pub const WEB_PROTO_VERSION_3: u8 = 6;
pub const DEFAULT_PORT: u16 = 7331;
pub const LEGACY_UDP_HZ: u16 = 250;
pub const PRO_UDP_HZ: u16 = 250;
pub const LEGACY_UDP_INTERVAL_MS: u16 = 4;
pub const PRO_UDP_INTERVAL_MS: u16 = 4;
pub const DEFAULT_SECRET: &str = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
pub const HMAC_TAG_SIZE: usize = 16;

pub const RUMBLE_MAGIC: u32 = 0x4e53_5652;
pub const PRECISION_RUMBLE_MAGIC: u32 = 0x4e53_5648;
pub const CONTROLLER_STATUS_MAGIC: u32 = 0x4e53_4353;
pub const CLIENT_ASSIGNMENT_MAGIC: u32 = 0x4e53_4341;
pub const SERVER_INFO_MAGIC: u32 = 0x4e53_5349;
pub const CLIENT_NAMES_MAGIC: u32 = 0x4e53_434e;
pub const ROSTER_MAGIC: u32 = 0x4e53_524f;
pub const JOYCON_MOUSE_MAGIC: u32 = 0x4e53_4a4d;
pub const GADGET_MODE_MAGIC: u32 = 0x4e53_4d44;
pub const S2_AUDIO_CAPS_MAGIC: u32 = 0x4e53_4143;
pub const S2_AUDIO_PCM_MAGIC: u32 = 0x4e53_4155;
pub const AMIIBO_REQUEST_MAGIC: u32 = 0x4e53_4152;
pub const AMIIBO_DATA_MAGIC: u32 = 0x4e53_4144;
pub const AMIIBO_LIBRARY_MAGIC: u32 = 0x4e53_414c;
pub const AMIIBO_LIBRARY_RESULT_MAGIC: u32 = 0x4e53_4c52;

pub const SERVER_INFO_VERSION: u8 = 1;
pub const CLIENT_ASSIGNMENT_VERSION: u8 = 1;
pub const JOYCON_MOUSE_VERSION: u8 = 3;
pub const GADGET_MODE_VERSION: u8 = 1;
pub const S2_AUDIO_VERSION: u8 = 1;
pub const AMIIBO_LIBRARY_VERSION: u8 = 1;

pub const S2_AUDIO_CAP_PLAYBACK: u8 = 1 << 0;
pub const S2_AUDIO_CAP_MICROPHONE: u8 = 1 << 1;
pub const S2_AUDIO_DIR_CONSOLE_TO_CLIENT: u8 = 0;
pub const S2_AUDIO_DIR_CLIENT_TO_CONSOLE: u8 = 1;
pub const S2_AUDIO_SAMPLE_RATE: u32 = 48_000;
pub const S2_AUDIO_CHANNELS: u8 = 2;
pub const S2_AUDIO_SAMPLE_BYTES: u8 = 2;
pub const S2_AUDIO_USB_FRAME_BYTES: usize = 192;
pub const S2_AUDIO_UDP_FRAMES: usize = 5;
pub const S2_AUDIO_PCM_BYTES: usize = S2_AUDIO_USB_FRAME_BYTES * S2_AUDIO_UDP_FRAMES;
pub const S2_AUDIO_PORT_OFFSET: u16 = 1;

pub const SERVER_INFO_FLAG_SWITCH_ASLEEP: u8 = 1 << 0;
pub const SERVER_INFO_FLAG_SERVER_FULL: u8 = 1 << 1;
pub const SERVER_INFO_FLAG_SWITCH2_MODE: u8 = 1 << 2;
pub const SERVER_INFO_FLAG_HORI_MODE: u8 = 1 << 3;
pub const SERVER_INFO_FLAG_S2_AUDIO: u8 = 1 << 4;
pub const SERVER_INFO_FLAG_SESSION_TERMINATED: u8 = 1 << 5;

pub const FLAG_NONE: u8 = 0;
pub const FLAG_RESET: u8 = 0x01;
pub const FLAG_AUTOFIRE: u8 = 0x02;
pub const FLAG_SINGLE_PAD: u8 = 0x04;
pub const FLAG_DISCONNECT: u8 = 0x08;

pub const EXT_PAD_PRESENT: u8 = 0x01;
pub const EXT_BUTTON_C: u8 = 0x02;
pub const EXT_BUTTON_GL: u8 = 0x04;
pub const EXT_BUTTON_GR: u8 = 0x08;
pub const EXT_BUTTON_SL: u8 = 0x10;
pub const EXT_BUTTON_SR: u8 = 0x20;
pub const EXT_BUTTON_MASK: u8 =
    EXT_BUTTON_C | EXT_BUTTON_GL | EXT_BUTTON_GR | EXT_BUTTON_SL | EXT_BUTTON_SR;
pub const EXT_STATUS_BATTERY_VALID: u8 = 0x01;
pub const EXT_STATUS_BATTERY_CHARGING: u8 = 0x02;
pub const EXT_STATUS_MOTION_FRESH: u8 = 0x04;
pub const EXT_STATUS_MOTION_FRESH_VALID: u8 = 0x08;
pub const EXT_STATUS_BATTERY_PERCENT_UNKNOWN: u8 = 0xff;

pub const CONTROLLER_PLAYER_INDEX_UNKNOWN: u8 = 0xff;
pub const CONTROLLER_CONSOLE_PORT_NONE: u8 = 0xff;
pub const CONTROLLER_STATUS_FLAG_BODY_RGB_VALID: u8 = 0x01;
pub const CLIENT_ASSIGNMENT_FLAG_ACCEPTED: u8 = 0x01;
pub const CLIENT_ASSIGNMENT_FLAG_SERVER_FULL: u8 = 0x02;
pub const CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP: u8 = 0x04;
pub const CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID: u8 = 0x08;
pub const CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED: u8 = 0x10;
pub const CLIENT_ASSIGNMENT_FLAG_SESSION_TERMINATED: u8 = 0x20;
pub const JOYCON_MOUSE_FLAG_ACTIVE: u8 = 0x01;
pub const JOYCON_MOUSE_FLAG_LEFT_BUTTON: u8 = 0x02;
pub const JOYCON_MOUSE_FLAG_RIGHT_BUTTON: u8 = 0x04;

pub const ROSTER_NAME_CAP: usize = 48;
pub const AMIIBO_TAGMO_DUMP_SIZE: usize = 532;
pub const AMIIBO_RAW_DUMP_SIZE: usize = 540;
pub const AMIIBO_SIGNATURE_SIZE: usize = 32;
pub const AMIIBO_EXTENDED_DUMP_SIZE: usize = AMIIBO_RAW_DUMP_SIZE + AMIIBO_SIGNATURE_SIZE;
pub const AMIIBO_V3_DUMP_SIZE: usize = 2048;
pub const AMIIBO_MAX_DUMP_SIZE: usize = AMIIBO_V3_DUMP_SIZE;

pub const HORI_REPORT_SIZE: usize = 8;
pub const MOTION_REPORT_SIZE: usize = 12;
pub const HID_REPORT_SIZE: usize = 48;
pub const MULTI_REPORT_SIZE: usize = 192;
pub const PACKET_SIZE: usize = 228;
pub const PACKET_AUTH_SIZE: usize = PACKET_SIZE - HMAC_TAG_SIZE;
pub const WEB_PACKET_SIZE: usize = PACKET_AUTH_SIZE;
pub const RUMBLE_PACKET_SIZE: usize = 8;
pub const PRECISION_RUMBLE_PACKET_SIZE: usize = 20;
pub const SERVER_INFO_PROBE_SIZE: usize = 8;
pub const SERVER_INFO_REPLY_SIZE: usize = 16;
pub const GADGET_MODE_REQUEST_SIZE: usize = 28;
pub const GADGET_MODE_REQUEST_AUTH_SIZE: usize = 12;
pub const GADGET_MODE_REPLY_SIZE: usize = 12;
pub const CLIENT_ASSIGNMENT_PACKET_SIZE: usize = 16;
pub const CONTROLLER_STATUS_PACKET_SIZE: usize = 12;
pub const ROSTER_ENTRY_SIZE: usize = 50;
pub const CLIENT_NAMES_PACKET_SIZE: usize = 224;
pub const CLIENT_NAMES_AUTH_SIZE: usize = CLIENT_NAMES_PACKET_SIZE - HMAC_TAG_SIZE;
pub const ROSTER_PACKET_SIZE: usize = 208;
pub const JOYCON_MOUSE_PACKET_SIZE: usize = 47;
pub const JOYCON_MOUSE_AUTH_SIZE: usize = JOYCON_MOUSE_PACKET_SIZE - HMAC_TAG_SIZE;
pub const S2_AUDIO_CAPS_PACKET_SIZE: usize = 36;
pub const S2_AUDIO_CAPS_AUTH_SIZE: usize = S2_AUDIO_CAPS_PACKET_SIZE - HMAC_TAG_SIZE;
pub const S2_AUDIO_PCM_PACKET_SIZE: usize = 36 + S2_AUDIO_PCM_BYTES;
pub const S2_AUDIO_PCM_AUTH_SIZE: usize = S2_AUDIO_PCM_PACKET_SIZE - HMAC_TAG_SIZE;
pub const AMIIBO_REQUEST_PACKET_SIZE: usize = 8;
pub const AMIIBO_DATA_PACKET_SIZE: usize = 4 + 1 + 2 + AMIIBO_MAX_DUMP_SIZE;
pub const AMIIBO_LIBRARY_PACKET_SIZE: usize = 32;
pub const AMIIBO_LIBRARY_AUTH_SIZE: usize = 16;
pub const AMIIBO_LIBRARY_RESULT_PACKET_SIZE: usize = 20;

pub const BTN_Y: u16 = 1 << 0;
pub const BTN_B: u16 = 1 << 1;
pub const BTN_A: u16 = 1 << 2;
pub const BTN_X: u16 = 1 << 3;
pub const BTN_L: u16 = 1 << 4;
pub const BTN_R: u16 = 1 << 5;
pub const BTN_ZL: u16 = 1 << 6;
pub const BTN_ZR: u16 = 1 << 7;
pub const BTN_MINUS: u16 = 1 << 8;
pub const BTN_PLUS: u16 = 1 << 9;
pub const BTN_LSTICK: u16 = 1 << 10;
pub const BTN_RSTICK: u16 = 1 << 11;
pub const BTN_HOME: u16 = 1 << 12;
pub const BTN_CAPTURE: u16 = 1 << 13;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum GadgetFamily {
    #[default]
    Switch1 = 0,
    Switch2 = 1,
    Hori = 2,
}

impl GadgetFamily {
    fn from_wire(value: u8) -> Result<Self, WireError> {
        match value {
            0 => Ok(Self::Switch1),
            1 => Ok(Self::Switch2),
            2 => Ok(Self::Hori),
            other => Err(WireError::InvalidValue("gadget family", other)),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum GadgetModeResult {
    #[default]
    Restarting = 0,
    Unchanged = 1,
    ServerFull = 2,
}

impl GadgetModeResult {
    fn from_wire(value: u8) -> Result<Self, WireError> {
        match value {
            0 => Ok(Self::Restarting),
            1 => Ok(Self::Unchanged),
            2 => Ok(Self::ServerFull),
            other => Err(WireError::InvalidValue("gadget mode result", other)),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ServerBackend {
    #[default]
    Unknown = 0,
    Legacy = 1,
    Pro = 2,
}

impl ServerBackend {
    fn from_wire(value: u8) -> Result<Self, WireError> {
        match value {
            0 => Ok(Self::Unknown),
            1 => Ok(Self::Legacy),
            2 => Ok(Self::Pro),
            other => Err(WireError::InvalidValue("server backend", other)),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ControllerType {
    #[default]
    Default = 0,
    JoyconL = 1,
    JoyconR = 2,
    Pro = 3,
    JoyconPair = 4,
    Hori = 5,
    ProS2 = 6,
    JoyconLS2 = 7,
    JoyconRS2 = 8,
    JoyconPairS2 = 9,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum Hat {
    North = 0,
    NorthEast = 1,
    East = 2,
    SouthEast = 3,
    South = 4,
    SouthWest = 5,
    West = 6,
    NorthWest = 7,
    #[default]
    Neutral = 8,
}

impl Hat {
    fn from_wire(value: u8) -> Result<Self, WireError> {
        match value {
            0 => Ok(Self::North),
            1 => Ok(Self::NorthEast),
            2 => Ok(Self::East),
            3 => Ok(Self::SouthEast),
            4 => Ok(Self::South),
            5 => Ok(Self::SouthWest),
            6 => Ok(Self::West),
            7 => Ok(Self::NorthWest),
            8 => Ok(Self::Neutral),
            other => Err(WireError::InvalidValue("hat", other)),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WireError {
    InvalidLength { expected: usize, actual: usize },
    InvalidMagic { expected: u32, actual: u32 },
    InvalidValue(&'static str, u8),
}

impl fmt::Display for WireError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidLength { expected, actual } => {
                write!(formatter, "invalid wire length: expected {expected}, got {actual}")
            }
            Self::InvalidMagic { expected, actual } => {
                write!(formatter, "invalid magic: expected {expected:#010x}, got {actual:#010x}")
            }
            Self::InvalidValue(name, value) => write!(formatter, "invalid {name} value: {value}"),
        }
    }
}

impl std::error::Error for WireError {}

fn exact<const N: usize>(bytes: &[u8]) -> Result<&[u8; N], WireError> {
    bytes.try_into().map_err(|_| WireError::InvalidLength {
        expected: N,
        actual: bytes.len(),
    })
}

fn u16_le(bytes: &[u8]) -> u16 {
    u16::from_le_bytes(bytes.try_into().expect("wire slice has two bytes"))
}

fn i16_le(bytes: &[u8]) -> i16 {
    i16::from_le_bytes(bytes.try_into().expect("wire slice has two bytes"))
}

fn u32_le(bytes: &[u8]) -> u32 {
    u32::from_le_bytes(bytes.try_into().expect("wire slice has four bytes"))
}

fn i32_le(bytes: &[u8]) -> i32 {
    i32::from_le_bytes(bytes.try_into().expect("wire slice has four bytes"))
}

fn u64_le(bytes: &[u8]) -> u64 {
    u64::from_le_bytes(bytes.try_into().expect("wire slice has eight bytes"))
}

fn ensure_magic(expected: u32, actual: u32) -> Result<(), WireError> {
    if expected == actual {
        Ok(())
    } else {
        Err(WireError::InvalidMagic { expected, actual })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HoriHidReport {
    pub(crate) buttons: u16,
    pub(crate) hat: Hat,
    pub(crate) lx: u8,
    pub(crate) ly: u8,
    pub(crate) rx: u8,
    pub(crate) ry: u8,
    pub(crate) vendor: u8,
}

impl Default for HoriHidReport {
    fn default() -> Self {
        Self {
            buttons: 0,
            hat: Hat::Neutral,
            lx: 128,
            ly: 128,
            rx: 128,
            ry: 128,
            vendor: 0,
        }
    }
}

impl HoriHidReport {
    #[must_use]
    pub const fn new(buttons: u16, hat: Hat, axes: [u8; 4], vendor: u8) -> Self {
        Self {
            buttons,
            hat,
            lx: axes[0],
            ly: axes[1],
            rx: axes[2],
            ry: axes[3],
            vendor,
        }
    }

    #[must_use]
    pub const fn buttons(&self) -> u16 {
        self.buttons
    }

    #[must_use]
    pub const fn hat(&self) -> Hat {
        self.hat
    }

    #[must_use]
    pub const fn axes(&self) -> [u8; 4] {
        [self.lx, self.ly, self.rx, self.ry]
    }

    #[must_use]
    pub const fn vendor(&self) -> u8 {
        self.vendor
    }

    #[must_use]
    pub const fn is_pad_present(&self) -> bool {
        self.vendor & EXT_PAD_PRESENT != 0
    }

    pub fn set_pad_present(&mut self, present: bool) {
        if present {
            self.vendor |= EXT_PAD_PRESENT;
        } else {
            self.vendor &= !EXT_PAD_PRESENT;
        }
    }

    #[must_use]
    pub fn encode(&self) -> [u8; HORI_REPORT_SIZE] {
        let mut out = [0u8; HORI_REPORT_SIZE];
        out[..2].copy_from_slice(&self.buttons.to_le_bytes());
        out[2] = self.hat as u8;
        out[3] = self.lx;
        out[4] = self.ly;
        out[5] = self.rx;
        out[6] = self.ry;
        out[7] = self.vendor;
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<HORI_REPORT_SIZE>(bytes)?;
        Ok(Self {
            buttons: u16_le(&bytes[..2]),
            hat: Hat::from_wire(bytes[2])?,
            lx: bytes[3],
            ly: bytes[4],
            rx: bytes[5],
            ry: bytes[6],
            vendor: bytes[7],
        })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MotionReport {
    pub(crate) ax: i16,
    pub(crate) ay: i16,
    pub(crate) az: i16,
    pub(crate) gx: i16,
    pub(crate) gy: i16,
    pub(crate) gz: i16,
}

impl MotionReport {
    #[must_use]
    pub const fn new(accel: [i16; 3], gyro: [i16; 3]) -> Self {
        Self {
            ax: accel[0],
            ay: accel[1],
            az: accel[2],
            gx: gyro[0],
            gy: gyro[1],
            gz: gyro[2],
        }
    }

    #[must_use]
    pub const fn accel(&self) -> [i16; 3] {
        [self.ax, self.ay, self.az]
    }

    #[must_use]
    pub const fn gyro(&self) -> [i16; 3] {
        [self.gx, self.gy, self.gz]
    }

    #[must_use]
    pub fn encode(&self) -> [u8; MOTION_REPORT_SIZE] {
        let values = [self.ax, self.ay, self.az, self.gx, self.gy, self.gz];
        let mut out = [0u8; MOTION_REPORT_SIZE];
        for (chunk, value) in out.chunks_exact_mut(2).zip(values) {
            chunk.copy_from_slice(&value.to_le_bytes());
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<MOTION_REPORT_SIZE>(bytes)?;
        Ok(Self::new(
            [i16_le(&bytes[0..2]), i16_le(&bytes[2..4]), i16_le(&bytes[4..6])],
            [i16_le(&bytes[6..8]), i16_le(&bytes[8..10]), i16_le(&bytes[10..12])],
        ))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HidReport {
    pub(crate) input: HoriHidReport,
    pub(crate) motion: [MotionReport; 3],
    pub(crate) has_motion: bool,
    pub(crate) reserved: [u8; 3],
}

impl Default for HidReport {
    fn default() -> Self {
        Self {
            input: HoriHidReport::default(),
            motion: [MotionReport::default(); 3],
            has_motion: false,
            reserved: [0; 3],
        }
    }
}

impl HidReport {
    #[must_use]
    pub const fn new(
        input: HoriHidReport,
        motion: [MotionReport; 3],
        has_motion: bool,
        reserved: [u8; 3],
    ) -> Self {
        Self {
            input,
            motion,
            has_motion,
            reserved,
        }
    }

    #[must_use]
    pub const fn input(&self) -> &HoriHidReport {
        &self.input
    }

    #[must_use]
    pub const fn motion(&self) -> &[MotionReport; 3] {
        &self.motion
    }

    #[must_use]
    pub const fn has_motion(&self) -> bool {
        self.has_motion
    }

    #[must_use]
    pub const fn reserved(&self) -> [u8; 3] {
        self.reserved
    }

    #[must_use]
    pub fn encode(&self) -> [u8; HID_REPORT_SIZE] {
        let mut out = [0u8; HID_REPORT_SIZE];
        out[..HORI_REPORT_SIZE].copy_from_slice(&self.input.encode());
        for (index, sample) in self.motion.iter().enumerate() {
            let start = HORI_REPORT_SIZE + index * MOTION_REPORT_SIZE;
            out[start..start + MOTION_REPORT_SIZE].copy_from_slice(&sample.encode());
        }
        out[44] = u8::from(self.has_motion);
        out[45..48].copy_from_slice(&self.reserved);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<HID_REPORT_SIZE>(bytes)?;
        let input = HoriHidReport::decode(&bytes[..8])?;
        let motion = [
            MotionReport::decode(&bytes[8..20])?,
            MotionReport::decode(&bytes[20..32])?,
            MotionReport::decode(&bytes[32..44])?,
        ];
        Ok(Self::new(input, motion, bytes[44] != 0, [bytes[45], bytes[46], bytes[47]]))
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MultiReport {
    pub(crate) pads: [HidReport; 4],
}

impl MultiReport {
    #[must_use]
    pub const fn new(pads: [HidReport; 4]) -> Self {
        Self { pads }
    }

    #[must_use]
    pub const fn pads(&self) -> &[HidReport; 4] {
        &self.pads
    }

    #[must_use]
    pub fn encode(&self) -> [u8; MULTI_REPORT_SIZE] {
        let mut out = [0u8; MULTI_REPORT_SIZE];
        for (index, pad) in self.pads.iter().enumerate() {
            let start = index * HID_REPORT_SIZE;
            out[start..start + HID_REPORT_SIZE].copy_from_slice(&pad.encode());
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<MULTI_REPORT_SIZE>(bytes)?;
        Ok(Self::new([
            HidReport::decode(&bytes[0..48])?,
            HidReport::decode(&bytes[48..96])?,
            HidReport::decode(&bytes[96..144])?,
            HidReport::decode(&bytes[144..192])?,
        ]))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Packet {
    pub(crate) version: u8,
    pub(crate) flags: u8,
    pub(crate) reserved: u16,
    pub(crate) seq: u32,
    pub(crate) ts_us: u64,
    pub(crate) report: MultiReport,
    pub(crate) hmac: [u8; HMAC_TAG_SIZE],
}

impl Default for Packet {
    fn default() -> Self {
        Self {
            version: WEB_PROTO_VERSION,
            flags: FLAG_NONE,
            reserved: 0,
            seq: 0,
            ts_us: 0,
            report: MultiReport::default(),
            hmac: [0; HMAC_TAG_SIZE],
        }
    }
}

impl Packet {
    #[must_use]
    pub const fn new(version: u8, flags: u8, seq: u32, ts_us: u64, report: MultiReport) -> Self {
        Self {
            version,
            flags,
            reserved: 0,
            seq,
            ts_us,
            report,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn version(&self) -> u8 {
        self.version
    }

    #[must_use]
    pub const fn flags(&self) -> u8 {
        self.flags
    }

    #[must_use]
    pub const fn sequence(&self) -> u32 {
        self.seq
    }

    #[must_use]
    pub const fn timestamp_us(&self) -> u64 {
        self.ts_us
    }

    #[must_use]
    pub const fn report(&self) -> &MultiReport {
        &self.report
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, tag: [u8; HMAC_TAG_SIZE]) {
        self.hmac = tag;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; PACKET_SIZE] {
        let mut out = [0u8; PACKET_SIZE];
        out[..4].copy_from_slice(&PROTO_MAGIC.to_le_bytes());
        out[4] = self.version;
        out[5] = self.flags;
        out[6..8].copy_from_slice(&self.reserved.to_le_bytes());
        out[8..12].copy_from_slice(&self.seq.to_le_bytes());
        out[12..20].copy_from_slice(&self.ts_us.to_le_bytes());
        out[20..212].copy_from_slice(&self.report.encode());
        out[PACKET_AUTH_SIZE..].copy_from_slice(&self.hmac);
        out
    }

    #[must_use]
    pub fn authenticated_bytes(&self) -> [u8; PACKET_AUTH_SIZE] {
        let encoded = self.encode();
        let mut out = [0u8; PACKET_AUTH_SIZE];
        out.copy_from_slice(&encoded[..PACKET_AUTH_SIZE]);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<PACKET_SIZE>(bytes)?;
        ensure_magic(PROTO_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            version: bytes[4],
            flags: bytes[5],
            reserved: u16_le(&bytes[6..8]),
            seq: u32_le(&bytes[8..12]),
            ts_us: u64_le(&bytes[12..20]),
            report: MultiReport::decode(&bytes[20..212])?,
            hmac: bytes[212..228]
                .try_into()
                .expect("packet HMAC is exactly 16 bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RumblePacket {
    pub(crate) subpad: u8,
    pub(crate) low_freq: u8,
    pub(crate) high_freq: u8,
    pub(crate) duration_10ms: u8,
}

impl RumblePacket {
    #[must_use]
    pub const fn new(subpad: u8, low_freq: u8, high_freq: u8, duration_10ms: u8) -> Self {
        Self {
            subpad,
            low_freq,
            high_freq,
            duration_10ms,
        }
    }

    #[must_use]
    pub const fn components(&self) -> [u8; 4] {
        [self.subpad, self.low_freq, self.high_freq, self.duration_10ms]
    }

    #[must_use]
    pub fn encode(&self) -> [u8; RUMBLE_PACKET_SIZE] {
        let mut out = [0u8; RUMBLE_PACKET_SIZE];
        out[..4].copy_from_slice(&RUMBLE_MAGIC.to_le_bytes());
        out[4..8].copy_from_slice(&self.components());
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<RUMBLE_PACKET_SIZE>(bytes)?;
        ensure_magic(RUMBLE_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self::new(bytes[4], bytes[5], bytes[6], bytes[7]))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PrecisionRumblePacket {
    pub(crate) subpad: u8,
    pub(crate) low_freq: u8,
    pub(crate) high_freq: u8,
    pub(crate) duration_10ms: u8,
    pub(crate) precision: [u8; 8],
    pub(crate) reserved: [u8; 4],
}

impl PrecisionRumblePacket {
    #[must_use]
    pub const fn new(
        subpad: u8,
        low_freq: u8,
        high_freq: u8,
        duration_10ms: u8,
        precision: [u8; 8],
    ) -> Self {
        Self {
            subpad,
            low_freq,
            high_freq,
            duration_10ms,
            precision,
            reserved: [0; 4],
        }
    }

    #[must_use]
    pub fn encode(&self) -> [u8; PRECISION_RUMBLE_PACKET_SIZE] {
        let mut out = [0u8; PRECISION_RUMBLE_PACKET_SIZE];
        out[..4].copy_from_slice(&PRECISION_RUMBLE_MAGIC.to_le_bytes());
        out[4] = self.subpad;
        out[5] = self.low_freq;
        out[6] = self.high_freq;
        out[7] = self.duration_10ms;
        out[8..16].copy_from_slice(&self.precision);
        out[16..20].copy_from_slice(&self.reserved);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<PRECISION_RUMBLE_PACKET_SIZE>(bytes)?;
        ensure_magic(PRECISION_RUMBLE_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            subpad: bytes[4],
            low_freq: bytes[5],
            high_freq: bytes[6],
            duration_10ms: bytes[7],
            precision: bytes[8..16].try_into().expect("precision field is eight bytes"),
            reserved: bytes[16..20].try_into().expect("reserved field is four bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ServerInfoProbe;

impl ServerInfoProbe {
    #[must_use]
    pub fn encode() -> [u8; SERVER_INFO_PROBE_SIZE] {
        let mut out = [0u8; SERVER_INFO_PROBE_SIZE];
        out[..4].copy_from_slice(&SERVER_INFO_MAGIC.to_le_bytes());
        out[4] = SERVER_INFO_VERSION;
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<SERVER_INFO_PROBE_SIZE>(bytes)?;
        ensure_magic(SERVER_INFO_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ServerInfoReply {
    pub(crate) backend: ServerBackend,
    pub(crate) udp_interval_ms: u16,
    pub(crate) udp_hz: u16,
    pub(crate) reserved: [u8; 6],
}

impl ServerInfoReply {
    #[must_use]
    pub const fn new(backend: ServerBackend, udp_interval_ms: u16, udp_hz: u16) -> Self {
        Self {
            backend,
            udp_interval_ms,
            udp_hz,
            reserved: [0; 6],
        }
    }

    #[must_use]
    pub const fn backend(&self) -> ServerBackend {
        self.backend
    }

    #[must_use]
    pub const fn cadence(&self) -> (u16, u16) {
        (self.udp_interval_ms, self.udp_hz)
    }

    #[must_use]
    pub fn encode(&self) -> [u8; SERVER_INFO_REPLY_SIZE] {
        let mut out = [0u8; SERVER_INFO_REPLY_SIZE];
        out[..4].copy_from_slice(&SERVER_INFO_MAGIC.to_le_bytes());
        out[4] = SERVER_INFO_VERSION;
        out[5] = self.backend as u8;
        out[6..8].copy_from_slice(&self.udp_interval_ms.to_le_bytes());
        out[8..10].copy_from_slice(&self.udp_hz.to_le_bytes());
        out[10..16].copy_from_slice(&self.reserved);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<SERVER_INFO_REPLY_SIZE>(bytes)?;
        ensure_magic(SERVER_INFO_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            backend: ServerBackend::from_wire(bytes[5])?,
            udp_interval_ms: u16_le(&bytes[6..8]),
            udp_hz: u16_le(&bytes[8..10]),
            reserved: bytes[10..16].try_into().expect("reserved field is six bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GadgetModeRequest {
    pub(crate) family: GadgetFamily,
    pub(crate) seq: u32,
    pub(crate) hmac: [u8; HMAC_TAG_SIZE],
}

impl GadgetModeRequest {
    #[must_use]
    pub const fn new(family: GadgetFamily, seq: u32) -> Self {
        Self {
            family,
            seq,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn family(&self) -> GadgetFamily {
        self.family
    }

    #[must_use]
    pub const fn sequence(&self) -> u32 {
        self.seq
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, tag: [u8; HMAC_TAG_SIZE]) {
        self.hmac = tag;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; GADGET_MODE_REQUEST_SIZE] {
        let mut out = [0u8; GADGET_MODE_REQUEST_SIZE];
        out[..4].copy_from_slice(&GADGET_MODE_MAGIC.to_le_bytes());
        out[4] = GADGET_MODE_VERSION;
        out[5] = self.family as u8;
        out[8..12].copy_from_slice(&self.seq.to_le_bytes());
        out[12..].copy_from_slice(&self.hmac);
        out
    }

    #[must_use]
    pub fn authenticated_bytes(&self) -> [u8; GADGET_MODE_REQUEST_AUTH_SIZE] {
        let encoded = self.encode();
        encoded[..GADGET_MODE_REQUEST_AUTH_SIZE]
            .try_into()
            .expect("authenticated request prefix is 12 bytes")
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<GADGET_MODE_REQUEST_SIZE>(bytes)?;
        ensure_magic(GADGET_MODE_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            family: GadgetFamily::from_wire(bytes[5])?,
            seq: u32_le(&bytes[8..12]),
            hmac: bytes[12..28].try_into().expect("HMAC field is 16 bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GadgetModeReply {
    pub(crate) result: GadgetModeResult,
    pub(crate) active_family: GadgetFamily,
    pub(crate) active_clients: u8,
    pub(crate) reserved: u32,
}

impl GadgetModeReply {
    #[must_use]
    pub const fn new(
        result: GadgetModeResult,
        active_family: GadgetFamily,
        active_clients: u8,
    ) -> Self {
        Self {
            result,
            active_family,
            active_clients,
            reserved: 0,
        }
    }

    #[must_use]
    pub const fn result(&self) -> GadgetModeResult {
        self.result
    }

    #[must_use]
    pub const fn active_family(&self) -> GadgetFamily {
        self.active_family
    }

    #[must_use]
    pub const fn active_clients(&self) -> u8 {
        self.active_clients
    }

    #[must_use]
    pub fn encode(&self) -> [u8; GADGET_MODE_REPLY_SIZE] {
        let mut out = [0u8; GADGET_MODE_REPLY_SIZE];
        out[..4].copy_from_slice(&GADGET_MODE_MAGIC.to_le_bytes());
        out[4] = GADGET_MODE_VERSION;
        out[5] = self.result as u8;
        out[6] = self.active_family as u8;
        out[7] = self.active_clients;
        out[8..12].copy_from_slice(&self.reserved.to_le_bytes());
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<GADGET_MODE_REPLY_SIZE>(bytes)?;
        ensure_magic(GADGET_MODE_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            result: GadgetModeResult::from_wire(bytes[5])?,
            active_family: GadgetFamily::from_wire(bytes[6])?,
            active_clients: bytes[7],
            reserved: u32_le(&bytes[8..12]),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct JoyconMousePacket {
    pub(crate) flags: u8,
    pub(crate) subpad: u8,
    pub(crate) seq: u32,
    pub(crate) delta_x: i32,
    pub(crate) delta_y: i32,
    pub(crate) scroll_y: i32,
    pub(crate) ts_us: u64,
    pub(crate) hmac: [u8; HMAC_TAG_SIZE],
}

impl JoyconMousePacket {
    #[must_use]
    pub const fn new(
        flags: u8,
        subpad: u8,
        seq: u32,
        deltas: [i32; 3],
        ts_us: u64,
    ) -> Self {
        Self {
            flags,
            subpad,
            seq,
            delta_x: deltas[0],
            delta_y: deltas[1],
            scroll_y: deltas[2],
            ts_us,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn flags(&self) -> u8 {
        self.flags
    }

    #[must_use]
    pub const fn subpad(&self) -> u8 {
        self.subpad
    }

    #[must_use]
    pub const fn sequence(&self) -> u32 {
        self.seq
    }

    #[must_use]
    pub const fn deltas(&self) -> [i32; 3] {
        [self.delta_x, self.delta_y, self.scroll_y]
    }

    #[must_use]
    pub const fn timestamp_us(&self) -> u64 {
        self.ts_us
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, tag: [u8; HMAC_TAG_SIZE]) {
        self.hmac = tag;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; JOYCON_MOUSE_PACKET_SIZE] {
        let mut out = [0u8; JOYCON_MOUSE_PACKET_SIZE];
        out[..4].copy_from_slice(&JOYCON_MOUSE_MAGIC.to_le_bytes());
        out[4] = JOYCON_MOUSE_VERSION;
        out[5] = self.flags;
        out[6] = self.subpad;
        out[7..11].copy_from_slice(&self.seq.to_le_bytes());
        out[11..15].copy_from_slice(&self.delta_x.to_le_bytes());
        out[15..19].copy_from_slice(&self.delta_y.to_le_bytes());
        out[19..23].copy_from_slice(&self.scroll_y.to_le_bytes());
        out[23..31].copy_from_slice(&self.ts_us.to_le_bytes());
        out[31..47].copy_from_slice(&self.hmac);
        out
    }

    #[must_use]
    pub fn authenticated_bytes(&self) -> [u8; JOYCON_MOUSE_AUTH_SIZE] {
        let encoded = self.encode();
        encoded[..JOYCON_MOUSE_AUTH_SIZE]
            .try_into()
            .expect("authenticated mouse prefix is 31 bytes")
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<JOYCON_MOUSE_PACKET_SIZE>(bytes)?;
        ensure_magic(JOYCON_MOUSE_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            flags: bytes[5],
            subpad: bytes[6],
            seq: u32_le(&bytes[7..11]),
            delta_x: i32_le(&bytes[11..15]),
            delta_y: i32_le(&bytes[15..19]),
            scroll_y: i32_le(&bytes[19..23]),
            ts_us: u64_le(&bytes[23..31]),
            hmac: bytes[31..47].try_into().expect("HMAC field is 16 bytes"),
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct S2AudioPcmPacket {
    pub(crate) direction: u8,
    pub(crate) payload_bytes: u16,
    pub(crate) seq: u32,
    pub(crate) ts_us: u64,
    pub(crate) pcm: [u8; S2_AUDIO_PCM_BYTES],
    pub(crate) hmac: [u8; HMAC_TAG_SIZE],
}

impl S2AudioPcmPacket {
    #[must_use]
    pub fn new(direction: u8, seq: u32, ts_us: u64, pcm: [u8; S2_AUDIO_PCM_BYTES]) -> Self {
        Self {
            direction,
            payload_bytes: S2_AUDIO_PCM_BYTES as u16,
            seq,
            ts_us,
            pcm,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn direction(&self) -> u8 {
        self.direction
    }

    #[must_use]
    pub const fn sequence(&self) -> u32 {
        self.seq
    }

    #[must_use]
    pub const fn timestamp_us(&self) -> u64 {
        self.ts_us
    }

    #[must_use]
    pub const fn pcm(&self) -> &[u8; S2_AUDIO_PCM_BYTES] {
        &self.pcm
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, tag: [u8; HMAC_TAG_SIZE]) {
        self.hmac = tag;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; S2_AUDIO_PCM_PACKET_SIZE] {
        let mut out = [0u8; S2_AUDIO_PCM_PACKET_SIZE];
        out[..4].copy_from_slice(&S2_AUDIO_PCM_MAGIC.to_le_bytes());
        out[4] = S2_AUDIO_VERSION;
        out[5] = self.direction;
        out[6..8].copy_from_slice(&self.payload_bytes.to_le_bytes());
        out[8..12].copy_from_slice(&self.seq.to_le_bytes());
        out[12..20].copy_from_slice(&self.ts_us.to_le_bytes());
        out[20..20 + S2_AUDIO_PCM_BYTES].copy_from_slice(&self.pcm);
        out[S2_AUDIO_PCM_AUTH_SIZE..].copy_from_slice(&self.hmac);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<S2_AUDIO_PCM_PACKET_SIZE>(bytes)?;
        ensure_magic(S2_AUDIO_PCM_MAGIC, u32_le(&bytes[..4]))?;
        let payload_bytes = u16_le(&bytes[6..8]);
        if usize::from(payload_bytes) > S2_AUDIO_PCM_BYTES {
            return Err(WireError::InvalidValue("S2 audio payload size", bytes[6]));
        }
        Ok(Self {
            direction: bytes[5],
            payload_bytes,
            seq: u32_le(&bytes[8..12]),
            ts_us: u64_le(&bytes[12..20]),
            pcm: bytes[20..20 + S2_AUDIO_PCM_BYTES]
                .try_into()
                .expect("PCM field has the fixed protocol size"),
            hmac: bytes[S2_AUDIO_PCM_AUTH_SIZE..]
                .try_into()
                .expect("HMAC field is 16 bytes"),
        })
    }
}

#[must_use]
pub const fn is_supported_amiibo_dump_size(size: usize) -> bool {
    size == AMIIBO_TAGMO_DUMP_SIZE
        || size == AMIIBO_RAW_DUMP_SIZE
        || size == AMIIBO_EXTENDED_DUMP_SIZE
        || size == AMIIBO_V3_DUMP_SIZE
}

#[cfg(test)]
mod tests {
    use super::{
        GadgetFamily, GadgetModeRequest, Hat, HidReport, HoriHidReport, JoyconMousePacket,
        MotionReport, MultiReport, Packet, PACKET_AUTH_SIZE, PACKET_SIZE, WEB_PROTO_VERSION,
    };

    #[test]
    fn legacy_hori_layout_is_exact() {
        let report = HoriHidReport::new(0x1234, Hat::SouthWest, [1, 2, 3, 4], 0xa5);
        assert_eq!(report.encode(), [0x34, 0x12, 5, 1, 2, 3, 4, 0xa5]);
        assert_eq!(HoriHidReport::decode(&report.encode()), Ok(report));
    }

    #[test]
    fn motion_layout_is_little_endian() {
        let motion = MotionReport::new([1, -2, 0x1234], [-1, 0x2345, -0x3456]);
        assert_eq!(MotionReport::decode(&motion.encode()), Ok(motion));
    }

    #[test]
    fn unified_packet_stays_228_bytes() {
        let input = HoriHidReport::new(0x55aa, Hat::NorthEast, [0, 127, 128, 255], 1);
        let sample = MotionReport::new([1, 2, 3], [4, 5, 6]);
        let hid = HidReport::new(input, [sample; 3], true, [90, 0x0f, 6]);
        let report = MultiReport::new([hid, HidReport::default(), HidReport::default(), hid]);
        let mut packet = Packet::new(WEB_PROTO_VERSION, 3, 0x1234_5678, 0x1020_3040_5060_7080, report);
        packet.set_hmac([0xa5; 16]);
        let encoded = packet.encode();
        assert_eq!(encoded.len(), PACKET_SIZE);
        assert_eq!(packet.authenticated_bytes().len(), PACKET_AUTH_SIZE);
        assert_eq!(Packet::decode(&encoded), Ok(packet));
    }

    #[test]
    fn authenticated_control_prefixes_match_cpp_sizes() {
        let request = GadgetModeRequest::new(GadgetFamily::Switch2, 0x4433_2211);
        assert_eq!(request.authenticated_bytes().len(), 12);
        assert_eq!(request.encode().len(), 28);

        let mouse = JoyconMousePacket::new(7, 2, 4, [-100, 200, -1], 99);
        assert_eq!(mouse.authenticated_bytes().len(), 31);
        assert_eq!(mouse.encode().len(), 47);
    }

    #[test]
    fn malformed_wire_data_fails_closed() {
        assert!(Packet::decode(&[0; PACKET_SIZE]).is_err());
        assert!(Packet::decode(&[0; PACKET_SIZE - 1]).is_err());
        let mut invalid_hat = HoriHidReport::default().encode();
        invalid_hat[2] = 9;
        assert!(HoriHidReport::decode(&invalid_hat).is_err());
    }
}
