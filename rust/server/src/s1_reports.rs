use ns_shared::protocol::{
    ControllerType, Hat, HidReport, HoriHidReport, MotionReport, BTN_A, BTN_B, BTN_CAPTURE,
    BTN_HOME, BTN_L, BTN_LSTICK, BTN_MINUS, BTN_PLUS, BTN_R, BTN_RSTICK, BTN_X, BTN_Y,
    BTN_ZL, BTN_ZR, EXT_BUTTON_SL, EXT_BUTTON_SR, EXT_STATUS_BATTERY_CHARGING,
    EXT_STATUS_BATTERY_VALID,
};

pub const PRO_REPORT_SIZE: usize = 64;
pub const RID_INPUT_STANDARD: u8 = 0x30;
pub const RID_INPUT_SUBCMD: u8 = 0x21;
pub const RID_OUTPUT_RUMBLE: u8 = 0x10;
pub const RID_OUTPUT_CMD: u8 = 0x01;
pub const PRO_BAT_CON: u8 = 0x81;
pub const PRO_VIBRATOR_REPORT: u8 = 0x0b;

#[must_use]
pub const fn pro_timer_from_us(time_us: u64) -> u8 {
    ((time_us / 5_000) & 0xff) as u8
}

#[must_use]
pub fn pro_conn_info_from_hid(source: &HidReport) -> u8 {
    let reserved = source.reserved();
    if reserved[1] & EXT_STATUS_BATTERY_VALID == 0 || reserved[0] > 100 {
        return PRO_BAT_CON;
    }
    let level = match reserved[0] {
        90..=100 => 4,
        70..=89 => 3,
        45..=69 => 2,
        20..=44 => 1,
        _ => 0,
    };
    let mut connection = (level << 5) | (PRO_BAT_CON & 0x01);
    if reserved[1] & EXT_STATUS_BATTERY_CHARGING != 0 {
        connection |= 0x10;
    }
    connection
}

#[must_use]
pub fn axis8_to_12(value: u8) -> u16 {
    if value == 128 {
        return 0x800;
    }
    let delta = i32::from(value) - 128;
    let divisor = if delta > 0 { 127 } else { 128 };
    (0x800_i32 + (delta * 0x600) / divisor).clamp(0x200, 0xe00) as u16
}

#[must_use]
pub const fn invert_axis8_centered(value: u8) -> u8 {
    if value == 128 { 128 } else { 255 - value }
}

#[must_use]
pub fn pack_stick_12(x8: u8, y8: u8) -> [u8; 3] {
    let x = axis8_to_12(x8);
    let y = axis8_to_12(invert_axis8_centered(y8));
    [x as u8, ((x >> 8) as u8) | ((y << 4) as u8), (y >> 4) as u8]
}

#[must_use]
pub fn map_buttons(buttons: u16, hat: Hat) -> [u8; 3] {
    let mut output = [0_u8; 3];
    output[0] = if buttons & BTN_Y != 0 { 0x01 } else { 0 }
        | if buttons & BTN_X != 0 { 0x02 } else { 0 }
        | if buttons & BTN_B != 0 { 0x04 } else { 0 }
        | if buttons & BTN_A != 0 { 0x08 } else { 0 }
        | if buttons & BTN_R != 0 { 0x40 } else { 0 }
        | if buttons & BTN_ZR != 0 { 0x80 } else { 0 };
    output[1] = 0x80
        | if buttons & BTN_MINUS != 0 { 0x01 } else { 0 }
        | if buttons & BTN_PLUS != 0 { 0x02 } else { 0 }
        | if buttons & BTN_RSTICK != 0 { 0x04 } else { 0 }
        | if buttons & BTN_LSTICK != 0 { 0x08 } else { 0 }
        | if buttons & BTN_HOME != 0 { 0x10 } else { 0 }
        | if buttons & BTN_CAPTURE != 0 { 0x20 } else { 0 };
    output[2] = if buttons & BTN_L != 0 { 0x40 } else { 0 }
        | if buttons & BTN_ZL != 0 { 0x80 } else { 0 };
    output[2] |= match hat {
        Hat::North => 0x02,
        Hat::NorthEast => 0x06,
        Hat::East => 0x04,
        Hat::SouthEast => 0x05,
        Hat::South => 0x01,
        Hat::SouthWest => 0x09,
        Hat::West => 0x08,
        Hat::NorthWest => 0x0a,
        Hat::Neutral => 0,
    };
    output
}

#[must_use]
pub fn shape_controller_input(
    input: HoriHidReport,
    virtual_type: ControllerType,
    pair_member: bool,
) -> HoriHidReport {
    match virtual_type {
        ControllerType::JoyconR | ControllerType::JoyconRS2 => {
            let [lx, ly, mut rx, mut ry] = input.axes();
            if !pair_member && rx == 128 && ry == 128 {
                rx = lx;
                ry = ly;
            }
            HoriHidReport::new(
                input.buttons()
                    & (BTN_Y | BTN_B | BTN_A | BTN_X | BTN_R | BTN_ZR | BTN_PLUS | BTN_RSTICK | BTN_HOME),
                Hat::Neutral,
                [128, 128, rx, ry],
                input.vendor(),
            )
        }
        ControllerType::JoyconL | ControllerType::JoyconLS2 => {
            let [lx, ly, _, _] = input.axes();
            HoriHidReport::new(
                input.buttons() & (BTN_L | BTN_ZL | BTN_MINUS | BTN_LSTICK | BTN_CAPTURE),
                input.hat(),
                [lx, ly, 128, 128],
                input.vendor(),
            )
        }
        _ => input,
    }
}

#[must_use]
pub fn build_standard_report(
    source: &HidReport,
    virtual_type: ControllerType,
    pair_member: bool,
    imu_enabled: bool,
    timer: u8,
) -> [u8; PRO_REPORT_SIZE] {
    let shaped = shape_controller_input(*source.input(), virtual_type, pair_member);
    let mut output = [0_u8; PRO_REPORT_SIZE];
    output[0] = RID_INPUT_STANDARD;
    output[1] = timer;
    output[2] = pro_conn_info_from_hid(source);
    output[3..6].copy_from_slice(&map_buttons(shaped.buttons(), shaped.hat()));
    output[6..9].copy_from_slice(&pack_stick_12(shaped.axes()[0], shaped.axes()[1]));
    output[9..12].copy_from_slice(&pack_stick_12(shaped.axes()[2], shaped.axes()[3]));
    output[12] = PRO_VIBRATOR_REPORT;

    let motion = if imu_enabled && source.has_motion() {
        *source.motion()
    } else {
        [MotionReport::default(); 3]
    };
    for (index, sample) in motion.iter().enumerate() {
        let accel = sample.accel();
        let gyro = sample.gyro();
        let values = [accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2]];
        let base = 13 + index * 12;
        for (word, value) in values.into_iter().enumerate() {
            let start = base + word * 2;
            output[start..start + 2].copy_from_slice(&value.to_le_bytes());
        }
    }

    apply_controller_type_report(virtual_type, shaped.vendor(), &mut output);
    output
}

#[must_use]
pub fn neutral_standard_report(timer: u8) -> [u8; PRO_REPORT_SIZE] {
    build_standard_report(
        &HidReport::default(),
        ControllerType::Pro,
        false,
        false,
        timer,
    )
}

fn apply_controller_type_report(
    virtual_type: ControllerType,
    extra_buttons: u8,
    output: &mut [u8; PRO_REPORT_SIZE],
) {
    let side = match virtual_type {
        ControllerType::JoyconR | ControllerType::JoyconRS2 => 3,
        ControllerType::JoyconL | ControllerType::JoyconLS2 => 5,
        _ => return,
    };
    output[2] = (output[2] & 0xf0) | 0x0f;
    if output[side] & 0x40 != 0 {
        output[side] |= 0x10;
    }
    if output[side] & 0x80 != 0 {
        output[side] |= 0x20;
    }
    if extra_buttons & EXT_BUTTON_SR != 0 {
        output[side] |= 0x10;
    }
    if extra_buttons & EXT_BUTTON_SL != 0 {
        output[side] |= 0x20;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::protocol::{EXT_STATUS_BATTERY_PERCENT_UNKNOWN, MotionReport};

    #[test]
    fn neutral_report_matches_cpp_wire_defaults() {
        let report = neutral_standard_report(0x12);
        assert_eq!(report.len(), 64);
        assert_eq!(&report[..13], &[0x30, 0x12, 0x81, 0, 0x80, 0, 0, 0x08, 0x80, 0, 0x08, 0x80, 0x0b]);
        assert!(report[13..].iter().all(|byte| *byte == 0));
    }

    #[test]
    fn axis_mapping_matches_cpp_endpoints_and_center() {
        assert_eq!(axis8_to_12(0), 0x200);
        assert_eq!(axis8_to_12(128), 0x800);
        assert_eq!(axis8_to_12(255), 0xe00);
        assert_eq!(pack_stick_12(128, 128), [0x00, 0x08, 0x80]);
    }

    #[test]
    fn joycon_right_moves_single_source_stick_and_filters_buttons() {
        let input = HoriHidReport::new(
            BTN_A | BTN_L | BTN_R | BTN_HOME,
            Hat::South,
            [200, 60, 128, 128],
            0,
        );
        let shaped = shape_controller_input(input, ControllerType::JoyconR, false);
        assert_eq!(shaped.axes(), [128, 128, 200, 60]);
        assert_eq!(shaped.hat(), Hat::Neutral);
        assert_eq!(shaped.buttons(), BTN_A | BTN_R | BTN_HOME);
    }

    #[test]
    fn motion_words_follow_cpp_axis_order_and_little_endian() {
        let motion = MotionReport::new([1, 2, 3], [4, 5, 6]);
        let source = HidReport::new(
            HoriHidReport::default(),
            [motion, MotionReport::default(), MotionReport::default()],
            true,
            [EXT_STATUS_BATTERY_PERCENT_UNKNOWN, 0, 0],
        );
        let report = build_standard_report(&source, ControllerType::Pro, false, true, 0);
        assert_eq!(&report[13..25], &[1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0]);
    }

    #[test]
    fn timer_uses_cpp_five_millisecond_counter() {
        assert_eq!(pro_timer_from_us(0), 0);
        assert_eq!(pro_timer_from_us(4_999), 0);
        assert_eq!(pro_timer_from_us(5_000), 1);
        assert_eq!(pro_timer_from_us(1_280_000), 0);
    }
}
