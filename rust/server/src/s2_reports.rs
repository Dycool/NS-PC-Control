use crate::s2_motion_carrier::{MotionCarrierSample, MotionCarrierState, MOTION_CARRIER_SIZE};
use ns_shared::protocol::{
    Hat, HidReport, BTN_A, BTN_B, BTN_CAPTURE, BTN_HOME, BTN_L, BTN_LSTICK,
    BTN_MINUS, BTN_PLUS, BTN_R, BTN_RSTICK, BTN_X, BTN_Y, BTN_ZL, BTN_ZR,
    EXT_BUTTON_C, EXT_BUTTON_GL, EXT_BUTTON_GR, EXT_BUTTON_MASK, EXT_BUTTON_SL,
    EXT_BUTTON_SR, EXT_STATUS_BATTERY_CHARGING, EXT_STATUS_BATTERY_VALID,
};

pub const S2_REPORT_SIZE: usize = 64;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct JoyconMouseInput {
    dx: i16,
    dy: i16,
    scroll_y: i8,
    left_down: bool,
    right_down: bool,
    active: bool,
}

impl JoyconMouseInput {
    #[must_use]
    pub const fn new(
        dx: i16,
        dy: i16,
        scroll_y: i8,
        left_down: bool,
        right_down: bool,
        active: bool,
    ) -> Self {
        Self {
            dx,
            dy,
            scroll_y,
            left_down,
            right_down,
            active,
        }
    }

    #[must_use]
    pub const fn active(self) -> bool {
        self.active
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct S2ReportContext {
    pub nfc_state: u8,
    pub headset_state: u8,
    pub mouse: Option<JoyconMouseInput>,
}

#[derive(Clone, Copy)]
struct BuildParameters {
    timer: u8,
    imu_enabled: bool,
    fresh_motion: bool,
    now_us: u64,
    context: S2ReportContext,
}

#[derive(Clone, Copy)]
struct MotionParameters<'a> {
    source: &'a HidReport,
    imu_enabled: bool,
    fresh_motion: bool,
    now_us: u64,
    stationary_mouse: bool,
}

#[derive(Default)]
pub struct S2ReportBuilder {
    motion: MotionCarrierState,
    counter: u8,
}

impl S2ReportBuilder {
    pub fn reset(&mut self) {
        self.motion.reset();
        self.counter = 0;
    }

    #[must_use]
    pub fn build(
        &mut self,
        source: &HidReport,
        selected_report: u8,
        imu_enabled: bool,
        fresh_motion: bool,
        now_us: u64,
        context: S2ReportContext,
    ) -> [u8; S2_REPORT_SIZE] {
        let parameters = BuildParameters {
            timer: self.counter,
            imu_enabled,
            fresh_motion,
            now_us,
            context,
        };
        self.counter = self.counter.wrapping_add(1);
        match selected_report {
            0x07 => self.build_joycon(source, false, parameters),
            0x08 => self.build_joycon(source, true, parameters),
            _ => self.build_pro(source, parameters),
        }
    }

    fn write_motion(
        &mut self,
        output: &mut [u8; S2_REPORT_SIZE],
        length_index: usize,
        data_index: usize,
        parameters: MotionParameters<'_>,
    ) {
        if !parameters.imu_enabled || !parameters.source.has_motion() {
            self.motion.reset();
            return;
        }
        if parameters.fresh_motion || parameters.stationary_mouse {
            let samples = if parameters.stationary_mouse {
                [MotionCarrierSample::new([0, -4096, 0], [0; 3]); 3]
            } else {
                parameters
                    .source
                    .motion()
                    .map(|sample| MotionCarrierSample::new(sample.accel(), sample.gyro()))
            };
            let _ = self.motion.update(&samples, parameters.now_us);
        }
        if self.motion.carrier_valid() && data_index + MOTION_CARRIER_SIZE <= output.len() {
            output[length_index] = MOTION_CARRIER_SIZE as u8;
            output[data_index..data_index + MOTION_CARRIER_SIZE]
                .copy_from_slice(self.motion.carrier());
        }
    }

    fn build_pro(
        &mut self,
        source: &HidReport,
        parameters: BuildParameters,
    ) -> [u8; S2_REPORT_SIZE] {
        let input = source.input();
        let buttons = input.buttons();
        let extra = input.vendor() & EXT_BUTTON_MASK;
        let [lx, ly, rx, ry] = input.axes();
        let mut output = [0u8; S2_REPORT_SIZE];
        output[0] = 0x09;
        output[1] = parameters.timer;
        output[2] = power_info(source);
        output[3] = bit(buttons, BTN_RSTICK, 0x80)
            | bit(buttons, BTN_PLUS, 0x40)
            | bit(buttons, BTN_ZR, 0x20)
            | bit(buttons, BTN_R, 0x10)
            | bit(buttons, BTN_X, 0x08)
            | bit(buttons, BTN_Y, 0x04)
            | bit(buttons, BTN_A, 0x02)
            | bit(buttons, BTN_B, 0x01);
        output[4] = bit(buttons, BTN_LSTICK, 0x80)
            | bit(buttons, BTN_MINUS, 0x40)
            | bit(buttons, BTN_ZL, 0x20)
            | bit(buttons, BTN_L, 0x10)
            | dpad(input.hat());
        output[5] = bit(buttons, BTN_CAPTURE, 0x02)
            | bit(buttons, BTN_HOME, 0x01)
            | extra_bit(extra, EXT_BUTTON_GR, 0x04)
            | extra_bit(extra, EXT_BUTTON_GL, 0x08)
            | extra_bit(extra, EXT_BUTTON_C, 0x10);
        pack_stick(&mut output[6..9], lx, ly);
        pack_stick(&mut output[9..12], rx, ry);
        output[12] = 0x30;
        output[13] = parameters.context.nfc_state;
        output[14] = parameters.context.headset_state;
        self.write_motion(
            &mut output,
            15,
            16,
            MotionParameters {
                source,
                imu_enabled: parameters.imu_enabled,
                fresh_motion: parameters.fresh_motion,
                now_us: parameters.now_us,
                stationary_mouse: false,
            },
        );
        output
    }

    fn build_joycon(
        &mut self,
        source: &HidReport,
        right: bool,
        parameters: BuildParameters,
    ) -> [u8; S2_REPORT_SIZE] {
        let input = source.input();
        let mut buttons = input.buttons();
        let extra = input.vendor() & EXT_BUTTON_MASK;
        let [lx, ly, rx, ry] = input.axes();
        let mouse = parameters.context.mouse;
        if let Some(mouse) = mouse.filter(|mouse| mouse.active()) {
            if right {
                if mouse.left_down {
                    buttons |= BTN_R;
                }
                if mouse.right_down {
                    buttons |= BTN_ZR;
                }
            } else {
                if mouse.left_down {
                    buttons |= BTN_L;
                }
                if mouse.right_down {
                    buttons |= BTN_ZL;
                }
            }
        }

        let mut output = [0u8; S2_REPORT_SIZE];
        output[0] = if right { 0x08 } else { 0x07 };
        output[1] = parameters.timer;
        output[2] = power_info(source);
        if right {
            output[3] = bit(buttons, BTN_RSTICK, 0x80)
                | bit(buttons, BTN_PLUS, 0x40)
                | bit(buttons, BTN_ZR, 0x20)
                | bit(buttons, BTN_R, 0x10)
                | bit(buttons, BTN_X, 0x08)
                | bit(buttons, BTN_Y, 0x04)
                | bit(buttons, BTN_A, 0x02)
                | bit(buttons, BTN_B, 0x01);
            output[4] = bit(buttons, BTN_ZR, 0x80)
                | bit(buttons, BTN_R, 0x40)
                | extra_bit(extra, EXT_BUTTON_SL, 0x80)
                | extra_bit(extra, EXT_BUTTON_SR, 0x40)
                | extra_bit(extra, EXT_BUTTON_C, 0x10)
                | bit(buttons, BTN_HOME, 0x01);
            let scroll_y = mouse
                .filter(|mouse| mouse.active && mouse.scroll_y != 0)
                .map_or(ry, |mouse| if mouse.scroll_y > 0 { 0 } else { 255 });
            pack_stick(&mut output[6..9], rx, scroll_y);
        } else {
            output[3] = bit(buttons, BTN_LSTICK, 0x80)
                | bit(buttons, BTN_MINUS, 0x40)
                | bit(buttons, BTN_ZL, 0x20)
                | bit(buttons, BTN_L, 0x10)
                | dpad(input.hat());
            output[4] = bit(buttons, BTN_ZL, 0x80)
                | bit(buttons, BTN_L, 0x40)
                | extra_bit(extra, EXT_BUTTON_SL, 0x80)
                | extra_bit(extra, EXT_BUTTON_SR, 0x40)
                | bit(buttons, BTN_CAPTURE, 0x01);
            let scroll_y = mouse
                .filter(|mouse| mouse.active && mouse.scroll_y != 0)
                .map_or(ly, |mouse| if mouse.scroll_y > 0 { 0 } else { 255 });
            pack_stick(&mut output[6..9], lx, scroll_y);
        }
        output[5] = 0x07;
        output[9] = 0x38;

        let mouse_active = mouse.is_some_and(|mouse| mouse.active);
        self.write_motion(
            &mut output,
            16,
            17,
            MotionParameters {
                source,
                imu_enabled: parameters.imu_enabled,
                fresh_motion: parameters.fresh_motion,
                now_us: parameters.now_us,
                stationary_mouse: mouse_active,
            },
        );
        if let Some(mut mouse) = mouse {
            if mouse.active && mouse.scroll_y != 0 && mouse.dx == 0 && mouse.dy == 0 {
                mouse.dx = if parameters.timer & 1 != 0 { 1 } else { -1 };
            }
            let dx = mouse.dx.to_le_bytes();
            let dy = mouse.dy.to_le_bytes();
            output[0x0a..0x0c].copy_from_slice(&dx);
            output[0x0c..0x0e].copy_from_slice(&dy);
            output[0x0e] = if mouse.active { 0x17 } else { 0xff };
        }
        output[15] = if right {
            parameters.context.nfc_state
        } else {
            0
        };
        output
    }
}

fn power_info(source: &HidReport) -> u8 {
    let reserved = source.reserved();
    let level = if reserved[1] & EXT_STATUS_BATTERY_VALID != 0 {
        reserved[0].min(100) / 11
    } else {
        9
    }
    .min(9);
    (level << 2)
        | 0x01
        | if reserved[1] & EXT_STATUS_BATTERY_CHARGING != 0 {
            0x02
        } else {
            0
        }
}

fn bit(buttons: u16, mask: u16, output: u8) -> u8 {
    if buttons & mask != 0 {
        output
    } else {
        0
    }
}

fn extra_bit(buttons: u8, mask: u8, output: u8) -> u8 {
    if buttons & mask != 0 {
        output
    } else {
        0
    }
}

fn dpad(hat: Hat) -> u8 {
    match hat {
        Hat::North => 0x08,
        Hat::NorthEast => 0x08 | 0x02,
        Hat::East => 0x02,
        Hat::SouthEast => 0x01 | 0x02,
        Hat::South => 0x01,
        Hat::SouthWest => 0x01 | 0x04,
        Hat::West => 0x04,
        Hat::NorthWest => 0x08 | 0x04,
        Hat::Neutral => 0,
    }
}

fn axis8_to_12(value: u8) -> u16 {
    if value == 128 {
        return 0x800;
    }
    let delta = i32::from(value) - 128;
    (0x800 + (delta * 0x600) / if delta > 0 { 127 } else { 128 })
        .clamp(0x200, 0xe00) as u16
}

fn invert_axis(value: u8) -> u8 {
    if value == 128 {
        128
    } else {
        255 - value
    }
}

fn pack_stick(output: &mut [u8], x: u8, y: u8) {
    let x = axis8_to_12(x);
    let y = axis8_to_12(invert_axis(y));
    output[0] = x as u8;
    output[1] = ((x >> 8) as u8) | ((y << 4) as u8);
    output[2] = (y >> 4) as u8;
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::protocol::{HoriHidReport, MotionReport};

    #[test]
    fn neutral_pro2_report_matches_cpp_layout() {
        let source = HidReport::new(
            HoriHidReport::default(),
            [MotionReport::default(); 3],
            false,
            [0; 3],
        );
        let mut builder = S2ReportBuilder::default();
        let report = builder.build(
            &source,
            0x09,
            false,
            false,
            0,
            S2ReportContext::default(),
        );
        assert_eq!(report[0], 0x09);
        assert_eq!(report[2], 0x25);
        assert_eq!(&report[6..9], &[0x00, 0x08, 0x80]);
        assert_eq!(&report[9..12], &[0x00, 0x08, 0x80]);
        assert_eq!(report[12], 0x30);
        assert_eq!(report[15], 0);
    }

    #[test]
    fn joycon_mouse_block_preserves_cpp_offsets() {
        let source = HidReport::new(
            HoriHidReport::default(),
            [MotionReport::default(); 3],
            true,
            [0; 3],
        );
        let mut builder = S2ReportBuilder::default();
        let report = builder.build(
            &source,
            0x08,
            true,
            true,
            4_000,
            S2ReportContext {
                mouse: Some(JoyconMouseInput::new(12, -4, 0, true, false, true)),
                ..S2ReportContext::default()
            },
        );
        assert_eq!(report[0], 0x08);
        assert_eq!(&report[0x0a..0x0c], &12i16.to_le_bytes());
        assert_eq!(&report[0x0c..0x0e], &(-4i16).to_le_bytes());
        assert_eq!(report[0x0e], 0x17);
        assert_eq!(report[16], MOTION_CARRIER_SIZE as u8);
    }
}
