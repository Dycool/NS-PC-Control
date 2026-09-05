use crate::s2_enumeration::{EnumerationTracker, RecoveryDecision};
use crate::s2_native_command::{validate_streaming_command, StreamingCommandStatus};
use crate::s2_nfc_codec::{NfcError, S2NfcRuntime, Signature};
use ns_shared::aes::aes128_encrypt_block;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

const FACTORY_BASE: u32 = 0x13000;
const FACTORY_SIZE: usize = 0x160;
const USER_MOTION_CAL_BASE: u32 = 0x1fc000;
const USER_MOTION_CAL_SIZE: usize = 0x80;
const LEGACY_PORT_COUNT: usize = 4;
const USER_MOTION_CAL_MAGIC: [u8; 8] = *b"NS2CAL\x02\0";
const FEATURE_BUTTONS: u32 = 0x01;
const FEATURE_STICKS: u32 = 0x02;
const FEATURE_IMU: u32 = 0x04;
const FEATURE_MOUSE: u32 = 0x10;
const FEATURE_RUMBLE: u32 = 0x20;
const FEATURE_MAG: u32 = 0x80;
const DEFAULT_FEATURE_MASK: u32 = FEATURE_BUTTONS | FEATURE_STICKS | FEATURE_IMU | FEATURE_RUMBLE;
const DEVICE_KEY_B1: [u8; 16] = [
    0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05, 0xe1,
    0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10,
];
const EMULATED_FIRMWARE_VERSION: [u8; 12] = [
    0x02, 0x09, 0x63, 0x02, 0x0c, 0x09, 0x09, 0x00, 0x00, 0x09, 0x09, 0x00,
];

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct NativeRuntimeFlags {
    full_report_enabled: bool,
    input_report_mode: u8,
    imu_enabled: bool,
    vibration_enabled: bool,
    player_leds: u8,
}

impl NativeRuntimeFlags {
    #[must_use]
    pub const fn full_report_enabled(self) -> bool { self.full_report_enabled }
    #[must_use]
    pub const fn input_report_mode(self) -> u8 { self.input_report_mode }
    #[must_use]
    pub const fn imu_enabled(self) -> bool { self.imu_enabled }
    #[must_use]
    pub const fn vibration_enabled(self) -> bool { self.vibration_enabled }
    #[must_use]
    pub const fn player_leds(self) -> u8 { self.player_leds }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum NativeCommandError {
    ShortOptionalPacket,
    TruncatedStreamingCommand,
    UnsupportedReportId(u8),
    FirmwareUpdateRejected(u8),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Ep0Reply {
    Data(Vec<u8>),
    StatusOnly,
}

struct NativeState {
    streaming: bool,
    selected_report: u8,
    feature_mask: u32,
    enabled_features: u32,
    stage: u8,
    ltk: [u8; 16],
    factory: [u8; FACTORY_SIZE],
    user_motion_cal: [u8; USER_MOTION_CAL_SIZE],
    identity: [u8; 64],
    ctrl_info: [u8; 16],
    pairing_info: [u8; 9],
    runtime: NativeRuntimeFlags,
    nfc: S2NfcRuntime,
}

impl NativeState {
    fn new(port: usize) -> Self {
        let mut state = Self {
            streaming: false,
            selected_report: 0x09,
            feature_mask: DEFAULT_FEATURE_MASK,
            enabled_features: 0,
            stage: 0,
            ltk: [0; 16],
            factory: [0xff; FACTORY_SIZE],
            user_motion_cal: [0xff; USER_MOTION_CAL_SIZE],
            identity: [0xff; 64],
            ctrl_info: [0; 16],
            pairing_info: [0; 9],
            runtime: NativeRuntimeFlags::default(),
            nfc: S2NfcRuntime::default(),
        };
        state.build_factory(port);
        state
    }

    fn build_factory(&mut self, port: usize) {
        self.factory.fill(0xff);
        let block: [u8; 40] = [
            0x01,0xad,0xd9,0x9a,0x55,0x56,0x65,0xa0,0x00,0x0a,0xa0,0x00,0x0a,0xe2,0x20,0x0e,
            0xe2,0x20,0x0e,0x9a,0xad,0xd9,0x9a,0xad,0xd9,0x0a,0xa5,0x50,0x0a,0xa5,0x50,0x2f,
            0xf6,0x62,0x2f,0xf6,0x62,0x0a,0xff,0xff,
        ];
        let suffix = 0x30u8.wrapping_add((port % 10) as u8);
        let mac_last = 0x3cu8.wrapping_add(port as u8);
        let serial = [
            0x48,0x45,0x4a,0x37,0x31,0x30,0x30,0x31,
            0x31,0x32,0x31,0x32,0x34,suffix,0x00,0x00,
        ];
        self.ctrl_info = [0x01,0x01,0x05,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x9e,0x2b,0xab,0xab,0xa9,mac_last];
        self.pairing_info = [0x01,0x04,0x01,0x9e,0x2b,0xab,0xab,0xa9,mac_last];
        self.fac(0x13000, &[0x01, 0x00]);
        self.fac(0x13002, &serial);
        self.fac(0x13012, &[0x7e, 0x05]);
        self.fac(0x13014, &[0x69, 0x20]);
        self.fac(0x13016, &[0x01, 0x06, 0x01]);
        self.fac(0x13019, &[0x23, 0x23, 0x23]);
        self.fac(0x1301c, &[0xa0, 0xa0, 0xa0]);
        self.fac(0x1301f, &[0xe6, 0xe6, 0xe6]);
        self.fac(0x13022, &[0x32, 0x32, 0x32]);
        self.fac(0x13040, &[0x16,0xf4,0xd3,0x41,0x48,0xce,0x85,0xba,0xf1,0x05,0x71,0xba,0x1f,0x27,0xcb,0x3b]);
        self.fac(0x13080, &block);
        self.fac(0x130a8, &[0xa3,0xa7,0x87,0x35,0x06,0x5f,0x1c,0xc6,0x5c]);
        self.fac(0x130c0, &block);
        self.fac(0x130e8, &[0xb1,0xa8,0x83,0xb6,0x35,0x5e,0x27,0x26,0x64]);
        self.fac(0x13100, &[0,0,0,0,0,0,0,0,0,0,0,0,0x2d,0x10,0xa7,0x3d,0xe7,0x49,0x35,0x3c,0xa4,0x2d,0x20,0x41]);
        self.fac(0x13140, &[0x00,0xd7,0xa3,0xbc,0x41,0xd7,0xa3,0xbc,0x41]);
        self.refresh_identity();
    }

    fn fac(&mut self, address: u32, data: &[u8]) {
        let Some(offset) = address.checked_sub(FACTORY_BASE).map(|value| value as usize) else { return; };
        if offset <= self.factory.len() && data.len() <= self.factory.len() - offset {
            self.factory[offset..offset + data.len()].copy_from_slice(data);
        }
    }

    fn refresh_identity(&mut self) {
        let prefix: [u8; 0x25] = self.factory[..0x25]
            .try_into()
            .expect("factory identity prefix is 37 bytes");
        self.identity[..0x25].copy_from_slice(&prefix);
    }

    fn is_joycon(&self) -> bool {
        matches!(self.selected_report, 0x07 | 0x08) || matches!(self.factory[0x14], 0x66 | 0x67)
    }

    fn update_stage(&mut self, id: u8, sub: u8) {
        let stage = match (id, sub) {
            (0x15, 0x02) => 5,
            (0x15, 0x03) => 6,
            (0x02, 0x04 | 0x01) => 7,
            (0x11, _) => 8,
            (0x0c, 0x06 | 0x04) => 9,
            (0x03, 0x0a) => 10,
            _ => 4,
        };
        self.stage = self.stage.max(stage);
    }

    fn set_feature_state(&mut self, sub: u8, mask: u32) {
        match sub {
            0x02 => self.feature_mask = mask,
            0x03 => {
                self.feature_mask &= !mask;
                self.enabled_features &= !mask;
            }
            0x04 => self.enabled_features |= mask & self.feature_mask,
            0x05 => self.enabled_features &= !mask,
            _ => {}
        }
        self.runtime.imu_enabled = self.enabled_features & FEATURE_IMU != 0;
        self.runtime.vibration_enabled = self.enabled_features & FEATURE_RUMBLE != 0;
    }

    fn read_memory(&self, address: u32, length: usize) -> Vec<u8> {
        (0..length)
            .map(|index| {
                let address = address.saturating_add(index as u32);
                if (FACTORY_BASE..FACTORY_BASE + FACTORY_SIZE as u32).contains(&address) {
                    self.factory[(address - FACTORY_BASE) as usize]
                } else if (USER_MOTION_CAL_BASE..USER_MOTION_CAL_BASE + USER_MOTION_CAL_SIZE as u32).contains(&address) {
                    self.user_motion_cal[(address - USER_MOTION_CAL_BASE) as usize]
                } else if address == 0x1fa000 {
                    0
                } else {
                    0xff
                }
            })
            .collect()
    }

    fn write_motion_calibration(&mut self, address: u32, data: &[u8]) -> bool {
        let mut changed = false;
        for (index, value) in data.iter().copied().enumerate() {
            let address = u64::from(address).saturating_add(index as u64);
            if (u64::from(USER_MOTION_CAL_BASE)..u64::from(USER_MOTION_CAL_BASE) + USER_MOTION_CAL_SIZE as u64).contains(&address) {
                let offset = (address - u64::from(USER_MOTION_CAL_BASE)) as usize;
                if self.user_motion_cal[offset] != value {
                    self.user_motion_cal[offset] = value;
                    changed = true;
                }
            }
        }
        changed
    }

    fn set_ltk(&mut self, a1_wire: &[u8]) {
        if a1_wire.len() < 16 { return; }
        for index in 0..16 {
            self.ltk[index] = a1_wire[15 - index] ^ DEVICE_KEY_B1[15 - index];
        }
    }

    fn answer_challenge(&self, a2_wire: &[u8]) -> [u8; 16] {
        let mut reversed = [0u8; 16];
        if a2_wire.len() >= 16 {
            for index in 0..16 { reversed[index] = a2_wire[15 - index]; }
        }
        aes128_encrypt_block(&self.ltk, &reversed)
    }
}

pub struct NativeController {
    state: Mutex<NativeState>,
    enumeration: EnumerationTracker,
    calibration_path: PathBuf,
}

impl Default for NativeController {
    fn default() -> Self { Self::new("/var/lib/ns-pc-control/switch2_motion_calibration.bin") }
}

impl NativeController {
    #[must_use]
    pub fn new(calibration_path: impl Into<PathBuf>) -> Self {
        let calibration_path = calibration_path.into();
        let mut state = NativeState::new(0);
        let _ = load_calibration(&calibration_path, &mut state.user_motion_cal);
        Self { state: Mutex::new(state), enumeration: EnumerationTracker::default(), calibration_path }
    }

    #[must_use]
    pub fn enumeration(&self) -> &EnumerationTracker { &self.enumeration }

    pub fn reset(&self) {
        let mut state = self.state.lock().unwrap_or_else(|poison| poison.into_inner());
        let calibration = state.user_motion_cal;
        *state = NativeState::new(0);
        state.user_motion_cal = calibration;
        self.enumeration.bus_reset();
    }

    pub fn set_pid(&self, pid_low: u8) {
        let mut state = self.state.lock().unwrap_or_else(|poison| poison.into_inner());
        state.factory[0x14] = pid_low;
        state.factory[0x15] = 0x20;
        state.refresh_identity();
        state.selected_report = match pid_low { 0x67 => 0x07, 0x66 => 0x08, _ => 0x09 };
        if matches!(pid_low, 0x66 | 0x67) {
            state.feature_mask |= FEATURE_MOUSE;
        } else {
            state.feature_mask &= !FEATURE_MOUSE;
            state.enabled_features &= !FEATURE_MOUSE;
        }
    }

    #[must_use]
    pub fn streaming_enabled(&self) -> bool {
        bench_stream_enabled() || self.state.lock().unwrap_or_else(|poison| poison.into_inner()).streaming
    }

    #[must_use]
    pub fn selected_report(&self) -> u8 {
        self.state.lock().unwrap_or_else(|poison| poison.into_inner()).selected_report
    }

    #[must_use]
    pub fn enabled_features(&self) -> u32 {
        if bench_stream_enabled() { DEFAULT_FEATURE_MASK } else { self.state.lock().unwrap_or_else(|poison| poison.into_inner()).enabled_features }
    }

    #[must_use]
    pub fn runtime_flags(&self) -> NativeRuntimeFlags {
        self.state.lock().unwrap_or_else(|poison| poison.into_inner()).runtime
    }

    pub fn set_amiibo_data(&self, data: &[u8], has_real_signature: bool, signature: Signature) -> Result<(), NfcError> {
        self.state.lock().unwrap_or_else(|poison| poison.into_inner()).nfc.set_tag_data(data, has_real_signature, signature)
    }

    pub fn clear_amiibo(&self) {
        self.state.lock().unwrap_or_else(|poison| poison.into_inner()).nfc.clear();
    }

    #[must_use]
    pub fn take_modified_amiibo(&self) -> Option<Vec<u8>> {
        let mut state = self.state.lock().unwrap_or_else(|poison| poison.into_inner());
        if !state.nfc.is_modified() {
            return None;
        }
        let image = state.nfc.image().to_vec();
        state.nfc.clear_modified();
        Some(image)
    }

    pub fn handle_ep0(&self, request_type: u8, request: u8, length: u16) -> Option<Ep0Reply> {
        if request_type & 0x60 != 0x40 { return None; }
        let mut state = self.state.lock().unwrap_or_else(|poison| poison.into_inner());
        state.stage = state.stage.max(4);
        match (request_type & 0x80 != 0, request) {
            (true, 0x03) => Some(Ep0Reply::Data(state.identity[..usize::from(length).min(state.identity.len())].to_vec())),
            (true, 0x02) => Some(Ep0Reply::Data(state.ctrl_info[..usize::from(length).min(state.ctrl_info.len())].to_vec())),
            (false, 0x04) => Some(Ep0Reply::StatusOnly),
            _ => None,
        }
    }

    pub fn handle_vendor_command(&self, command: &[u8], now_ms: u64) -> Result<Vec<u8>, NativeCommandError> {
        let streaming = validate_streaming_command(command);
        match streaming.status() {
            StreamingCommandStatus::Truncated => {
                let _ = self.enumeration.request_reenumeration("truncated mandatory native streaming command 0x03/0x0A");
                return Err(NativeCommandError::TruncatedStreamingCommand);
            }
            StreamingCommandStatus::UnsupportedReportId => {
                let _ = self.enumeration.request_reenumeration("invalid report ID in mandatory native streaming command 0x03/0x0A");
                return Err(NativeCommandError::UnsupportedReportId(streaming.report_id()));
            }
            _ => {}
        }
        if command.len() < 8 { return Err(NativeCommandError::ShortOptionalPacket); }
        let id = command[0];
        let transport = command[2];
        let sub = command[3];
        if id == 0x0d && sub != 0x01 { return Err(NativeCommandError::FirmwareUpdateRejected(sub)); }

        let mut state = self.state.lock().unwrap_or_else(|poison| poison.into_inner());
        self.enumeration.native_handshake();
        state.update_stage(id, sub);
        let mut response = vec![id, 0x01, transport, sub, 0x00, 0xf8, 0x00, 0x00];
        let mut payload = Vec::new();
        match id {
            0x03 => match sub {
                0x0d | 0x03 => payload = vec![0x01, 0, 0, 0],
                0x0a => {
                    state.selected_report = streaming.report_id();
                    state.streaming = true;
                    state.runtime.full_report_enabled = true;
                    state.runtime.input_report_mode = state.selected_report;
                    self.enumeration.streaming_validated(state.selected_report);
                }
                _ => {}
            },
            0x07 => payload.push(0),
            0x16 => payload.resize(24, 0),
            0x15 => match sub {
                0x01 => payload.extend_from_slice(&state.pairing_info),
                0x02 => {
                    payload.push(1);
                    if command.len() >= 25 { payload.extend_from_slice(&state.answer_challenge(&command[9..25])); } else { payload.resize(17, 0); }
                }
                0x03 => payload.push(1),
                0x04 => {
                    payload.push(1);
                    if command.len() >= 25 { state.set_ltk(&command[9..25]); }
                    payload.extend_from_slice(&DEVICE_KEY_B1);
                }
                _ => {}
            },
            0x09 => {
                if sub == 0x07 && command.len() > 8 { state.runtime.player_leds = command[8]; }
            }
            0x0c => {
                response[4] = 0x10;
                response[5] = 0x78;
                if sub == 0x01 {
                    let features = u32::from(command.get(8).copied().unwrap_or(0));
                    let joycon = state.is_joycon();
                    payload.resize(12, 0);
                    payload[4] = if features & FEATURE_BUTTONS != 0 { 0x07 } else { 0 };
                    payload[5] = if features & FEATURE_STICKS != 0 { 0x07 } else { 0 };
                    payload[6] = if features & FEATURE_IMU != 0 { if joycon { 0x03 } else { 0x01 } } else { 0 };
                    payload[7] = if features & FEATURE_MAG != 0 { if joycon { 0x03 } else { 0x01 } } else { 0 };
                    payload[8] = if features & FEATURE_MOUSE != 0 { if joycon { 0x03 } else { 0x01 } } else { 0 };
                    payload[9] = if features & FEATURE_RUMBLE != 0 { 0x03 } else { 0 };
                } else if sub == 0x06 {
                    payload.resize(40, 0);
                    if command.len() > 12 { payload[4] = command[12]; }
                } else {
                    let mask = read_le32(command, 8);
                    state.set_feature_state(sub, mask);
                    payload.resize(4, 0);
                }
            }
            0x02 => {
                let address = read_le32(command, 12);
                match sub {
                    0x04 => {
                        let length = usize::from(command.get(8).copied().unwrap_or(0).min(0x50));
                        payload.resize(8, 0);
                        payload[0] = length as u8;
                        if command.len() >= 16 { payload[4..8].copy_from_slice(&command[12..16]); }
                        payload.extend_from_slice(&state.read_memory(address, length));
                    }
                    0x01 => {
                        payload.resize(8, 0);
                        payload[0] = 0x40;
                        if command.len() >= 16 { payload[4..8].copy_from_slice(&command[12..16]); }
                        payload.extend_from_slice(&state.read_memory(address, 0x40));
                    }
                    0x05 => {
                        payload.resize(8, 0);
                        if command.len() >= 16 { payload[4..8].copy_from_slice(&command[12..16]); }
                        if command.len() > 16 {
                            let declared = usize::from(command.get(5).copied().unwrap_or(0).saturating_sub(8));
                            let length = declared.min(command.len() - 16);
                            if length != 0 && state.write_motion_calibration(address, &command[16..16 + length]) {
                                let _ = save_calibration(&self.calibration_path, &state.user_motion_cal);
                            }
                        }
                    }
                    _ => {}
                }
            }
            0x10 => payload.extend_from_slice(&EMULATED_FIRMWARE_VERSION),
            0x0d => {}
            0x0b => match sub {
                0x03 => payload.extend_from_slice(&[0xa5,0x0e,0,0]),
                0x04 => payload.extend_from_slice(&[0x34,0,0x83,0]),
                _ => {}
            },
            0x11 => match sub {
                0x01 => payload.extend_from_slice(&[0x03,0,0,0]),
                0x03 => payload.extend_from_slice(&[0x01,0x20,0x03,0x00,0x00,0x0a,0xe8,0x1c,0x3b,0x79,0x7d,0x8b,0x3a,0x0a,0xe8,0x9c,0x42,0x58,0xa0,0x0b,0x42,0x0a,0xe8,0x9c,0x41,0x58,0xa0,0x0b,0x41]),
                _ => {}
            },
            0x01 => {
                if sub == 0x0c {
                    payload.extend_from_slice(&[0x61,0x12,0x50,0x10]);
                } else if matches!(sub, 0x03 | 0x04 | 0x05 | 0x06 | 0x08 | 0x14 | 0x15 | 0x1e | 0x20 | 0x21) {
                    let reply = state.nfc.step(now_ms, sub, &command[8..]);
                    response[1] = reply.direction();
                    payload.extend_from_slice(reply.payload());
                }
            }
            0x18 => match sub {
                0x01 => payload.extend_from_slice(&[0,0,0x40,0xf0,0,0,0x60,0]),
                0x03 => payload.push(command.get(8).copied().unwrap_or(0)),
                _ => {}
            },
            _ => {}
        }
        response.extend_from_slice(&payload);
        Ok(response)
    }

    #[must_use]
    pub fn recovery_decision(&self, reason: &str) -> RecoveryDecision { self.enumeration.request_reenumeration(reason) }
}

fn bench_stream_enabled() -> bool {
    std::env::var_os("NS_S2_BENCH_STREAM")
        .is_some_and(|value| value.to_string_lossy() != "0" && !value.is_empty())
}

fn read_le32(bytes: &[u8], offset: usize) -> u32 {
    bytes.get(offset..offset + 4).and_then(|value| value.try_into().ok()).map(u32::from_le_bytes).unwrap_or(0)
}

fn load_calibration(path: &Path, output: &mut [u8; USER_MOTION_CAL_SIZE]) -> io::Result<()> {
    let bytes = fs::read(path)?;
    if bytes.len() < USER_MOTION_CAL_MAGIC.len() + USER_MOTION_CAL_SIZE || bytes[..8] != USER_MOTION_CAL_MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid Switch 2 calibration file"));
    }
    let payload = bytes.len() - USER_MOTION_CAL_MAGIC.len();
    if payload != USER_MOTION_CAL_SIZE && payload != USER_MOTION_CAL_SIZE * LEGACY_PORT_COUNT {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "unsupported Switch 2 calibration file size"));
    }
    output.copy_from_slice(&bytes[8..8 + USER_MOTION_CAL_SIZE]);
    Ok(())
}

fn save_calibration(path: &Path, data: &[u8; USER_MOTION_CAL_SIZE]) -> io::Result<()> {
    let parent = path.parent().ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "calibration path has no parent"))?;
    fs::create_dir_all(parent)?;
    let temporary = path.with_extension("bin.tmp");
    let mut bytes = vec![0xff; USER_MOTION_CAL_MAGIC.len() + USER_MOTION_CAL_SIZE * LEGACY_PORT_COUNT];
    bytes[..8].copy_from_slice(&USER_MOTION_CAL_MAGIC);
    bytes[8..8 + USER_MOTION_CAL_SIZE].copy_from_slice(data);
    fs::write(&temporary, bytes)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&temporary, fs::Permissions::from_mode(0o600))?;
    }
    fs::rename(temporary, path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn controller() -> NativeController {
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH).expect("time").as_nanos();
        NativeController::new(std::env::temp_dir().join(format!("nspc-s2-native-{nonce}.bin")))
    }

    #[test]
    fn ep0_identity_and_factory_pid_match_cpp() {
        let controller = controller();
        let Some(Ep0Reply::Data(identity)) = controller.handle_ep0(0xc0, 0x03, 64) else { panic!("identity"); };
        assert_eq!(identity[0x14..0x16], [0x69, 0x20]);
        controller.set_pid(0x67);
        assert_eq!(controller.selected_report(), 0x07);
        let Some(Ep0Reply::Data(identity)) = controller.handle_ep0(0xc0, 0x03, 64) else { panic!("identity"); };
        assert_eq!(identity[0x14..0x16], [0x67, 0x20]);
    }

    #[test]
    fn streaming_and_feature_negotiation_match_cpp_contract() {
        let controller = controller();
        let command = [0x03,0,0,0x0a,0,0,0,0,0x09];
        let response = controller.handle_vendor_command(&command, 0).expect("stream");
        assert_eq!(response, [0x03,0x01,0x00,0x0a,0x00,0xf8,0x00,0x00]);
        assert!(controller.streaming_enabled());
        assert_eq!(controller.selected_report(), 0x09);
        let mut enable = [0u8; 12];
        enable[0] = 0x0c;
        enable[3] = 0x04;
        enable[8..12].copy_from_slice(&(FEATURE_IMU | FEATURE_RUMBLE).to_le_bytes());
        let response = controller.handle_vendor_command(&enable, 0).expect("features");
        assert_eq!(&response[4..6], &[0x10, 0x78]);
        assert_eq!(controller.enabled_features() & (FEATURE_IMU | FEATURE_RUMBLE), FEATURE_IMU | FEATURE_RUMBLE);
    }

    #[test]
    fn memory_reads_preserve_erased_ff_regions() {
        let controller = controller();
        let mut read = [0u8; 16];
        read[0] = 0x02;
        read[3] = 0x04;
        read[8] = 0x10;
        read[12..16].copy_from_slice(&0x13060u32.to_le_bytes());
        let response = controller.handle_vendor_command(&read, 0).expect("memory");
        assert_eq!(&response[16..32], &[0xff; 16]);
    }

    #[test]
    fn pairing_challenge_uses_safe_aes_path() {
        let controller = controller();
        let mut key_command = vec![0u8; 25];
        key_command[0] = 0x15;
        key_command[3] = 0x04;
        for (index, value) in key_command[9..25].iter_mut().enumerate() { *value = index as u8; }
        let response = controller.handle_vendor_command(&key_command, 0).expect("key");
        assert_eq!(&response[9..25], &DEVICE_KEY_B1);
        let mut challenge = vec![0u8; 25];
        challenge[0] = 0x15;
        challenge[3] = 0x02;
        challenge[9..25].fill(0x55);
        let response = controller.handle_vendor_command(&challenge, 0).expect("challenge");
        assert_eq!(response.len(), 25);
        assert_ne!(&response[9..25], &[0; 16]);
    }

    #[test]
    fn calibration_loader_accepts_cpp_four_port_file() {
        let path = std::env::temp_dir().join("nspc-s2-calibration-compat.bin");
        let mut bytes = vec![0xff; 8 + USER_MOTION_CAL_SIZE * LEGACY_PORT_COUNT];
        bytes[..8].copy_from_slice(&USER_MOTION_CAL_MAGIC);
        bytes[8] = 0x42;
        fs::write(&path, bytes).expect("write");
        let mut data = [0u8; USER_MOTION_CAL_SIZE];
        load_calibration(&path, &mut data).expect("load");
        assert_eq!(data[0], 0x42);
        let _ = fs::remove_file(path);
    }
}
