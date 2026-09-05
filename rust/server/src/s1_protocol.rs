use crate::s1_reports::{
    map_buttons, pack_stick_12, pro_conn_info_from_hid, pro_timer_from_us, shape_controller_input,
    PRO_REPORT_SIZE, PRO_VIBRATOR_REPORT, RID_INPUT_SUBCMD, RID_OUTPUT_CMD, RID_OUTPUT_RUMBLE,
};
use crate::virtual_controller::{CTRL_MAC_BE, VIRTUAL_BODY_RGB};
use ns_shared::protocol::{ControllerType, HidReport, RumblePacket};

const SPI_FLASH_SIZE: usize = 0x20_0000;
const CMD_BT_MANUAL_PAIRING: u8 = 0x01;
const CMD_GET_DEVICE_INFO: u8 = 0x02;
const CMD_SET_DATA_FORMAT: u8 = 0x03;
const CMD_TRIGGER_BUTTONS: u8 = 0x04;
const CMD_SET_SHIP_MODE: u8 = 0x08;
const CMD_SPI_FLASH_READ: u8 = 0x10;
const CMD_SET_PLAYER_LIGHTS: u8 = 0x30;
const CMD_ENABLE_IMU: u8 = 0x40;
const CMD_SET_IMU_SENS: u8 = 0x41;
const CMD_ENABLE_VIBRATION: u8 = 0x48;
const RUMBLE_GAIN_PERCENT: i32 = 40;

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct S1OutputEffects {
    pub immediate_report: Option<[u8; PRO_REPORT_SIZE]>,
    pub rumble: Option<RumblePacket>,
    pub player_lights: Option<u8>,
}

#[derive(Clone, Debug)]
struct PendingReply {
    ack: u8,
    subcommand: u8,
    data: [u8; 49],
}

#[derive(Clone, Debug)]
pub struct S1PortRuntime {
    port: usize,
    profile: ControllerType,
    full_report_enabled: bool,
    input_report_mode: u8,
    imu_enabled: bool,
    vibration_enabled: bool,
    usb_seen_mac: bool,
    usb_handshake_done: bool,
    usb_baudrate_set: bool,
    usb_timeout_disabled: bool,
    pending_reply: Option<PendingReply>,
    rumble_active: bool,
    spi_flash: Vec<u8>,
}

impl S1PortRuntime {
    #[must_use]
    pub fn new(port: usize, profile: ControllerType) -> Self {
        let mut runtime = Self {
            port: port.min(3),
            profile,
            full_report_enabled: false,
            input_report_mode: 0x30,
            imu_enabled: false,
            vibration_enabled: false,
            usb_seen_mac: false,
            usb_handshake_done: false,
            usb_baudrate_set: false,
            usb_timeout_disabled: false,
            pending_reply: None,
            rumble_active: false,
            spi_flash: vec![0xff; SPI_FLASH_SIZE],
        };
        runtime.initialize_spi();
        runtime
    }

    pub fn reset_transport(&mut self) {
        self.full_report_enabled = false;
        self.input_report_mode = 0x30;
        self.imu_enabled = false;
        self.vibration_enabled = false;
        self.usb_seen_mac = false;
        self.usb_handshake_done = false;
        self.usb_baudrate_set = false;
        self.usb_timeout_disabled = false;
        self.pending_reply = None;
        self.rumble_active = false;
    }

    pub fn set_profile(&mut self, profile: ControllerType) {
        if self.profile == profile {
            return;
        }
        self.profile = profile;
        self.spi_flash.fill(0xff);
        self.initialize_spi();
    }

    #[must_use]
    pub const fn profile(&self) -> ControllerType {
        self.profile
    }

    #[must_use]
    pub const fn full_report_enabled(&self) -> bool {
        self.full_report_enabled
    }

    #[must_use]
    pub const fn imu_enabled(&self) -> bool {
        self.imu_enabled
    }

    #[must_use]
    pub const fn vibration_enabled(&self) -> bool {
        self.vibration_enabled
    }

    #[must_use]
    pub const fn input_report_mode(&self) -> u8 {
        self.input_report_mode
    }

    pub fn process_output(&mut self, packet: &[u8], subpad: usize) -> S1OutputEffects {
        if packet.len() < 2 || packet[..2] == [0, 0] {
            return S1OutputEffects::default();
        }
        match packet[0] {
            0x80 => S1OutputEffects {
                immediate_report: Some(self.process_usb_handshake(packet[1])),
                ..S1OutputEffects::default()
            },
            RID_OUTPUT_CMD if packet.len() > 10 => {
                let rumble = self.decode_rumble(packet, subpad, false);
                let subcommand = packet[10];
                let command_data = packet.get(11..).unwrap_or_default();
                let player_lights = (subcommand == CMD_SET_PLAYER_LIGHTS || subcommand == 0x33)
                    .then(|| command_data.first().copied())
                    .flatten();
                self.handle_subcommand(subcommand, command_data);
                S1OutputEffects {
                    immediate_report: None,
                    rumble,
                    player_lights,
                }
            }
            RID_OUTPUT_RUMBLE => S1OutputEffects {
                rumble: self.decode_rumble(packet, subpad, true),
                ..S1OutputEffects::default()
            },
            _ => S1OutputEffects::default(),
        }
    }

    #[must_use]
    pub fn take_subcommand_reply(
        &mut self,
        source: &HidReport,
        virtual_type: ControllerType,
        pair_member: bool,
        now_us: u64,
    ) -> Option<[u8; PRO_REPORT_SIZE]> {
        let pending = self.pending_reply.take()?;
        let shaped = shape_controller_input(*source.input(), virtual_type, pair_member);
        let mut output = [0_u8; PRO_REPORT_SIZE];
        output[0] = RID_INPUT_SUBCMD;
        output[1] = pro_timer_from_us(now_us);
        output[2] = pro_conn_info_from_hid(source);
        output[3..6].copy_from_slice(&map_buttons(shaped.buttons(), shaped.hat()));
        output[6..9].copy_from_slice(&pack_stick_12(shaped.axes()[0], shaped.axes()[1]));
        output[9..12].copy_from_slice(&pack_stick_12(shaped.axes()[2], shaped.axes()[3]));
        output[12] = PRO_VIBRATOR_REPORT;
        output[13] = pending.ack;
        output[14] = pending.subcommand;
        output[15..64].copy_from_slice(&pending.data);
        Some(output)
    }

    fn process_usb_handshake(&mut self, subtype: u8) -> [u8; PRO_REPORT_SIZE] {
        let mut output = [0_u8; PRO_REPORT_SIZE];
        output[0] = 0x81;
        output[1] = subtype;
        if subtype == 0x01 {
            output[3] = ControllerType::Pro as u8;
            let mac = CTRL_MAC_BE[self.port];
            output[4..10].copy_from_slice(&[mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]]);
        }
        match subtype {
            0x01 => self.usb_seen_mac = true,
            0x02 => self.usb_handshake_done = true,
            0x03 => self.usb_baudrate_set = true,
            0x04 => self.usb_timeout_disabled = true,
            0x05 => self.usb_timeout_disabled = false,
            _ => {}
        }
        output
    }

    fn handle_subcommand(&mut self, subcommand: u8, command_data: &[u8]) {
        let mut pending = PendingReply {
            ack: 0x80,
            subcommand,
            data: [0; 49],
        };
        match subcommand {
            CMD_BT_MANUAL_PAIRING => {
                pending.ack = 0x81;
                if !matches!(self.profile, ControllerType::Pro | ControllerType::ProS2) {
                    pending.data[..2].copy_from_slice(&[0x03, 0x01]);
                } else if command_data.first().is_some_and(|value| matches!(value, 0x02 | 0x03)) {
                    pending.data[..16].fill(0);
                }
            }
            CMD_TRIGGER_BUTTONS => {
                pending.ack = 0x83;
                pending.data[0] = 0;
            }
            CMD_SET_SHIP_MODE | CMD_SET_IMU_SENS => {}
            CMD_GET_DEVICE_INFO => {
                pending.ack = 0x82;
                let info = self.device_info();
                pending.data[..info.len()].copy_from_slice(&info);
            }
            CMD_SET_DATA_FORMAT => {
                self.full_report_enabled = true;
                self.input_report_mode = 0x30;
            }
            CMD_SPI_FLASH_READ => self.handle_spi_read(command_data, &mut pending),
            CMD_SET_PLAYER_LIGHTS | 0x33 => {}
            CMD_ENABLE_IMU => {
                self.imu_enabled = command_data.first().is_none_or(|value| *value != 0);
            }
            CMD_ENABLE_VIBRATION => {
                self.vibration_enabled = command_data.first().is_none_or(|value| *value != 0);
            }
            _ => {}
        }
        self.pending_reply = Some(pending);
    }

    fn handle_spi_read(&self, command_data: &[u8], pending: &mut PendingReply) {
        if command_data.len() < 5 {
            pending.ack = 0;
            return;
        }
        let address = u32::from_le_bytes(command_data[..4].try_into().expect("four byte SPI address"));
        let size = usize::from(command_data[4]).min(44);
        pending.ack = 0x90;
        pending.data[..5].copy_from_slice(&command_data[..5]);
        let start = usize::try_from(address).unwrap_or(usize::MAX);
        for offset in 0..size {
            pending.data[5 + offset] = self.spi_flash.get(start.saturating_add(offset)).copied().unwrap_or(0xff);
        }
    }

    fn device_info(&self) -> [u8; 36] {
        let mut output = [0_u8; 36];
        output[0] = 0x03;
        output[1] = 0x49;
        output[2] = logical_device_type(self.profile);
        output[3] = 0x02;
        let mac = CTRL_MAC_BE[self.port];
        output[4..10].copy_from_slice(&[mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]]);
        output[10] = 0x01;
        output[11] = 0x02;
        output
    }

    fn initialize_spi(&mut self) {
        self.spi_flash.fill(0xff);
        self.spi_flash[0x6012] = logical_device_type(self.profile);
        self.spi_flash[0x6013] = 0xa0;
        self.spi_flash[0x601b] = 0x02;

        pack12_into(&mut self.spi_flash, 0x603d, 0x600, 0x600);
        pack12_into(&mut self.spi_flash, 0x6040, 0x800, 0x800);
        pack12_into(&mut self.spi_flash, 0x6043, 0x600, 0x600);
        pack12_into(&mut self.spi_flash, 0x6046, 0x800, 0x800);
        pack12_into(&mut self.spi_flash, 0x6049, 0x600, 0x600);
        pack12_into(&mut self.spi_flash, 0x604c, 0x600, 0x600);

        let imu = [0_i16, 0, 0, 0x4000, 0x4000, 0x4000, 0, 0, 0, 0x343b, 0x343b, 0x343b];
        for (index, value) in imu.into_iter().enumerate() {
            put_i16(&mut self.spi_flash, 0x6020 + index * 2, value);
        }
        put_i16(&mut self.spi_flash, 0x6080, 0);
        put_i16(&mut self.spi_flash, 0x6082, 0);
        put_i16(&mut self.spi_flash, 0x6084, 0);
        pack12_into(&mut self.spi_flash, 0x6089, 0x0a0, 0x100);
        for index in 6..0x24 {
            self.spi_flash[0x6086 + index] = if index & 1 == 0 { 0x0f } else { 0x30 };
        }
        self.spi_flash[0x6050..0x6053].copy_from_slice(&VIRTUAL_BODY_RGB[self.port]);
        self.spi_flash[0x6053..0x6056].fill(0xff);
        self.spi_flash[0x6056..0x6059].copy_from_slice(&VIRTUAL_BODY_RGB[self.port]);
        self.spi_flash[0x6059..0x605c].fill(0xff);
        self.spi_flash[0x605c] = 0;
    }

    fn decode_rumble(&mut self, packet: &[u8], subpad: usize, publish_neutral: bool) -> Option<RumblePacket> {
        if packet.len() < 10 {
            return None;
        }
        let rumble = &packet[2..10];
        let left = decode_precision_half(&rumble[..4]);
        let right = decode_precision_half(&rumble[4..]);
        let mut low = left.0.max(right.0);
        let mut high = left.1.max(right.1);
        if low == 0
            && high == 0
            && !is_neutral_half(&rumble[..4])
            && !is_neutral_half(&rumble[4..])
        {
            low = decode_legacy_half(&rumble[..4]);
            high = decode_legacy_half(&rumble[4..]);
        }
        let neutral = low == 0 && high == 0;
        if neutral && (!publish_neutral || !self.rumble_active) {
            return None;
        }
        self.rumble_active = !neutral;
        Some(RumblePacket::new(
            u8::try_from(subpad).unwrap_or(0),
            if neutral { 0 } else { low },
            if neutral { 0 } else { high },
            if neutral { 0 } else { 1 },
        ))
    }
}

fn logical_device_type(profile: ControllerType) -> u8 {
    match profile {
        ControllerType::JoyconL | ControllerType::JoyconLS2 => 0x01,
        ControllerType::JoyconR | ControllerType::JoyconRS2 => 0x02,
        ControllerType::Hori => 0x04,
        _ => 0x03,
    }
}

fn put_i16(flash: &mut [u8], address: usize, value: i16) {
    flash[address..address + 2].copy_from_slice(&value.to_le_bytes());
}

fn pack12_into(flash: &mut [u8], address: usize, x: u16, y: u16) {
    flash[address] = x as u8;
    flash[address + 1] = ((x >> 8) as u8) | ((y << 4) as u8);
    flash[address + 2] = (y >> 4) as u8;
}

fn is_neutral_half(bytes: &[u8]) -> bool {
    bytes == [0, 0, 0, 0] || bytes == [0x00, 0x01, 0x40, 0x40]
}

fn scale_rumble(value: i32) -> u8 {
    let scaled = value.saturating_mul(RUMBLE_GAIN_PERCENT) / 100;
    if scaled > 0 {
        scaled.clamp(1, 255) as u8
    } else {
        0
    }
}

fn decode_precision_half(bytes: &[u8]) -> (u8, u8) {
    if is_neutral_half(bytes) {
        return (0, 0);
    }
    let high_delta = (i32::from(bytes[1] & 0x7f) - 0x01).abs();
    let low_delta = (i32::from(bytes[3] & 0x7f) - 0x40).abs();
    let high = scale_rumble(high_delta * 3 + i32::from(bytes[0]).abs() / 3);
    let low = scale_rumble(
        low_delta * 3 + (i32::from(bytes[2] & 0x7f) - 0x40).abs() / 2,
    );
    (low, high)
}

fn decode_legacy_half(bytes: &[u8]) -> u8 {
    if is_neutral_half(bytes) {
        return 0;
    }
    let neutral = [0x00_u8, 0x01, 0x40, 0x40];
    let mut max_diff = 0_i32;
    let mut sum_diff = 0_i32;
    for (value, reference) in bytes.iter().zip(neutral) {
        let difference = (i32::from(*value) - i32::from(reference)).abs();
        max_diff = max_diff.max(difference);
        sum_diff += difference;
    }
    scale_rumble(max_diff * 2 + sum_diff / 4)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::protocol::{HoriHidReport, MotionReport};

    #[test]
    fn usb_80_01_returns_cpp_identity_shape() {
        let mut runtime = S1PortRuntime::new(0, ControllerType::JoyconR);
        let effects = runtime.process_output(&[0x80, 0x01], 0);
        let reply = effects.immediate_report.expect("reply");
        assert_eq!(&reply[..4], &[0x81, 0x01, 0x00, 0x03]);
        assert_eq!(&reply[4..10], &[0xa0, 0x06, 0x26, 0x53, 0x4e, 0x02]);
    }

    #[test]
    fn device_info_reports_logical_joycon_identity() {
        let mut runtime = S1PortRuntime::new(1, ControllerType::JoyconL);
        let mut packet = [0_u8; 11];
        packet[0] = 0x01;
        packet[10] = CMD_GET_DEVICE_INFO;
        let _ = runtime.process_output(&packet, 0);
        let reply = runtime
            .take_subcommand_reply(&HidReport::default(), ControllerType::JoyconL, false, 0)
            .expect("reply");
        assert_eq!(reply[0], 0x21);
        assert_eq!(reply[13], 0x82);
        assert_eq!(reply[14], CMD_GET_DEVICE_INFO);
        assert_eq!(&reply[15..19], &[0x03, 0x49, 0x01, 0x02]);
    }

    #[test]
    fn spi_calibration_bytes_match_cpp_seed() {
        let mut runtime = S1PortRuntime::new(0, ControllerType::Pro);
        let mut packet = [0_u8; 16];
        packet[0] = 0x01;
        packet[10] = CMD_SPI_FLASH_READ;
        packet[11..15].copy_from_slice(&0x603d_u32.to_le_bytes());
        packet[15] = 6;
        let _ = runtime.process_output(&packet, 0);
        let reply = runtime
            .take_subcommand_reply(&HidReport::default(), ControllerType::Pro, false, 0)
            .expect("reply");
        assert_eq!(reply[13], 0x90);
        assert_eq!(&reply[15..20], &packet[11..16]);
        assert_eq!(&reply[20..26], &[0x00, 0x06, 0x60, 0x00, 0x08, 0x80]);
    }

    #[test]
    fn data_format_and_imu_commands_update_runtime() {
        let mut runtime = S1PortRuntime::new(0, ControllerType::Pro);
        let mut format = [0_u8; 12];
        format[0] = 0x01;
        format[10] = CMD_SET_DATA_FORMAT;
        format[11] = 0x30;
        let _ = runtime.process_output(&format, 0);
        assert!(runtime.full_report_enabled());
        assert_eq!(runtime.input_report_mode(), 0x30);

        let mut imu = [0_u8; 12];
        imu[0] = 0x01;
        imu[10] = CMD_ENABLE_IMU;
        imu[11] = 1;
        let _ = runtime.process_output(&imu, 0);
        assert!(runtime.imu_enabled());
    }

    #[test]
    fn neutral_rumble_is_suppressed_until_active() {
        let mut runtime = S1PortRuntime::new(0, ControllerType::Pro);
        let mut packet = [0_u8; 10];
        packet[0] = RID_OUTPUT_RUMBLE;
        packet[2..6].copy_from_slice(&[0, 1, 0x40, 0x40]);
        packet[6..10].copy_from_slice(&[0, 1, 0x40, 0x40]);
        assert_eq!(runtime.process_output(&packet, 0), S1OutputEffects::default());
    }

    #[test]
    fn command_can_publish_lights_and_rumble_together() {
        let mut runtime = S1PortRuntime::new(0, ControllerType::Pro);
        let mut packet = [0_u8; 12];
        packet[0] = RID_OUTPUT_CMD;
        packet[2..6].copy_from_slice(&[0x10, 0x10, 0x50, 0x50]);
        packet[6..10].copy_from_slice(&[0x10, 0x10, 0x50, 0x50]);
        packet[10] = CMD_SET_PLAYER_LIGHTS;
        packet[11] = 0x03;
        let effects = runtime.process_output(&packet, 2);
        assert_eq!(effects.player_lights, Some(0x03));
        assert!(effects.rumble.is_some());
    }

    #[test]
    fn subcommand_reply_carries_current_controls() {
        let mut runtime = S1PortRuntime::new(0, ControllerType::Pro);
        let mut packet = [0_u8; 11];
        packet[0] = RID_OUTPUT_CMD;
        packet[10] = CMD_TRIGGER_BUTTONS;
        let _ = runtime.process_output(&packet, 0);
        let source = HidReport::new(
            HoriHidReport::default(),
            [ns_shared::protocol::MotionReport::default(); 3],
            false,
            [0; 3],
        );
        let reply = runtime
            .take_subcommand_reply(&source, ControllerType::Pro, false, 25_000)
            .expect("reply");
        assert_eq!(reply[0], 0x21);
        assert_eq!(reply[1], 5);
        assert_eq!(reply[13..16], [0x83, CMD_TRIGGER_BUTTONS, 0]);
    }
}
