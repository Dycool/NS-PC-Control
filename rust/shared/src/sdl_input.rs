//! Deterministic SDL input normalization shared by the Rust clients and server.
//!
//! These helpers mirror the pure, backend-independent portion of the C++
//! `shared/sdl_input.cpp` implementation. Keeping them separate from an SDL FFI
//! lets first-party Rust remain safe while preserving the exact input policy.

use crate::protocol::{Hat, HoriHidReport};

pub const SDL_DIGITAL_RELEASE_GRACE_US: u64 = 35_000;
const GYRO_DEADZONE: i16 = 32;

/// Smooths one-poll digital releases exactly like the C++ SDL input path.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DigitalReleaseFilter {
    last_buttons: u16,
    button_until: [u64; 16],
    last_hat: Hat,
    hat_until: u64,
}

impl DigitalReleaseFilter {
    pub fn reset(&mut self) {
        self.last_buttons = 0;
        self.button_until.fill(0);
        self.last_hat = Hat::Neutral;
        self.hat_until = 0;
    }

    /// Applies the C++ 35 ms inclusive digital-release grace window.
    pub fn apply(&mut self, report: &mut HoriHidReport, now_us: u64) {
        for index in 0..16 {
            let bit = 1_u16 << index;
            if report.buttons & bit != 0 {
                self.last_buttons |= bit;
                self.button_until[index] = now_us.wrapping_add(SDL_DIGITAL_RELEASE_GRACE_US);
            } else if self.last_buttons & bit != 0
                && self.button_until[index] != 0
                && now_us <= self.button_until[index]
            {
                report.buttons |= bit;
            } else {
                self.last_buttons &= !bit;
                self.button_until[index] = 0;
            }
        }

        if report.hat != Hat::Neutral {
            self.last_hat = report.hat;
            self.hat_until = now_us.wrapping_add(SDL_DIGITAL_RELEASE_GRACE_US);
        } else if self.hat_until != 0 && now_us <= self.hat_until {
            report.hat = self.last_hat;
        } else {
            self.last_hat = Hat::Neutral;
            self.hat_until = 0;
        }
    }
}

/// Converts one signed SDL axis to the legacy 0..=255 report range.
#[must_use]
pub fn sdl_axis_to_byte(value: i16, invert: bool, deadzone: i32) -> u8 {
    let value = i32::from(value);
    if value > -deadzone && value < deadzone {
        return 128;
    }

    let scaled = if value >= deadzone {
        128 + ((value - deadzone) * 127) / (32_767 - deadzone)
    } else {
        128 - ((-value - deadzone) * 128) / (32_768 - deadzone)
    };
    let scaled = scaled.clamp(0, 255);
    if invert {
        (255 - scaled) as u8
    } else {
        scaled as u8
    }
}

/// Applies the C++ radial stick deadzone while preserving the original angle.
#[must_use]
pub fn sdl_stick_to_bytes(raw_x: i16, raw_y: i16, deadzone: i32) -> (u8, u8) {
    let x = f32::from(raw_x);
    let y = f32::from(raw_y);
    let magnitude = (x * x + y * y).sqrt();
    let deadzone = deadzone as f32;
    if magnitude <= deadzone {
        return (128, 128);
    }

    const MAX_MAGNITUDE: f32 = 32_767.0;
    let clamped = magnitude.min(MAX_MAGNITUDE);
    let scaled = (clamped - deadzone) / (MAX_MAGNITUDE - deadzone);
    let unit_x = x / magnitude;
    let unit_y = y / magnitude;

    fn to_byte(axis: f32) -> u8 {
        let value = 128 + (axis * 127.0).round() as i32;
        value.clamp(0, 255) as u8
    }

    (to_byte(unit_x * scaled), to_byte(unit_y * scaled))
}

/// Matches `std::lround` plus the C++ i16 saturation boundaries.
#[must_use]
pub fn clamp_motion_i16(value: f32) -> i16 {
    if value > 32_767.0 {
        32_767
    } else if value < -32_768.0 {
        -32_768
    } else {
        value.round() as i16
    }
}

/// Zeros the same inclusive ±32 gyro noise band as the C++ implementation.
#[must_use]
pub fn gyro_deadzone_i16(value: i16) -> i16 {
    if i32::from(value).abs() <= i32::from(GYRO_DEADZONE) {
        0
    } else {
        value
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::BTN_A;

    #[test]
    fn digital_button_release_grace_is_inclusive() {
        let mut filter = DigitalReleaseFilter::default();
        let mut pressed = HoriHidReport::default();
        pressed.buttons = BTN_A;
        filter.apply(&mut pressed, 100);

        let deadline = 100 + SDL_DIGITAL_RELEASE_GRACE_US;
        let mut at_deadline = HoriHidReport::default();
        filter.apply(&mut at_deadline, deadline);
        assert_eq!(at_deadline.buttons & BTN_A, BTN_A);

        let mut after_deadline = HoriHidReport::default();
        filter.apply(&mut after_deadline, deadline + 1);
        assert_eq!(after_deadline.buttons & BTN_A, 0);
    }

    #[test]
    fn repeated_press_extends_button_grace_window() {
        let mut filter = DigitalReleaseFilter::default();
        let mut report = HoriHidReport::default();
        report.buttons = BTN_A;
        filter.apply(&mut report, 10);
        filter.apply(&mut report, 20_000);

        let mut released = HoriHidReport::default();
        filter.apply(&mut released, 55_000);
        assert_eq!(released.buttons & BTN_A, BTN_A);

        let mut expired = HoriHidReport::default();
        filter.apply(&mut expired, 55_001);
        assert_eq!(expired.buttons & BTN_A, 0);
    }

    #[test]
    fn hat_release_grace_matches_button_boundary() {
        let mut filter = DigitalReleaseFilter::default();
        let mut report = HoriHidReport::default();
        report.hat = Hat::NorthEast;
        filter.apply(&mut report, 50);

        let mut at_deadline = HoriHidReport::default();
        filter.apply(&mut at_deadline, 50 + SDL_DIGITAL_RELEASE_GRACE_US);
        assert_eq!(at_deadline.hat, Hat::NorthEast);

        let mut expired = HoriHidReport::default();
        filter.apply(&mut expired, 51 + SDL_DIGITAL_RELEASE_GRACE_US);
        assert_eq!(expired.hat, Hat::Neutral);
    }

    #[test]
    fn reset_forgets_all_release_history() {
        let mut filter = DigitalReleaseFilter::default();
        let mut report = HoriHidReport::default();
        report.buttons = BTN_A;
        report.hat = Hat::South;
        filter.apply(&mut report, 1_000);
        filter.reset();

        let mut neutral = HoriHidReport::default();
        filter.apply(&mut neutral, 1_001);
        assert_eq!(neutral.buttons, 0);
        assert_eq!(neutral.hat, Hat::Neutral);
    }

    #[test]
    fn scalar_axis_preserves_cpp_strict_deadzone_and_endpoints() {
        const DEADZONE: i32 = 8_000;
        assert_eq!(sdl_axis_to_byte(7_999, false, DEADZONE), 128);
        assert_eq!(sdl_axis_to_byte(-7_999, false, DEADZONE), 128);
        assert_eq!(sdl_axis_to_byte(8_000, false, DEADZONE), 128);
        assert_eq!(sdl_axis_to_byte(-8_000, false, DEADZONE), 128);
        assert_eq!(sdl_axis_to_byte(32_767, false, DEADZONE), 255);
        assert_eq!(sdl_axis_to_byte(-32_768, false, DEADZONE), 0);
        assert_eq!(sdl_axis_to_byte(32_767, true, DEADZONE), 0);
        assert_eq!(sdl_axis_to_byte(-32_768, true, DEADZONE), 255);
    }

    #[test]
    fn radial_stick_deadzone_treats_cardinals_and_diagonals_circularly() {
        const DEADZONE: i32 = 8_000;
        assert_eq!(sdl_stick_to_bytes(8_000, 0, DEADZONE), (128, 128));
        assert_eq!(sdl_stick_to_bytes(0, -8_000, DEADZONE), (128, 128));
        assert_eq!(sdl_stick_to_bytes(32_767, 0, DEADZONE), (255, 128));

        let diagonal = sdl_stick_to_bytes(23_170, 23_170, DEADZONE);
        assert_eq!(diagonal.0, diagonal.1);
        assert!(diagonal.0 > 128);
    }

    #[test]
    fn motion_clamp_uses_lround_semantics_and_saturates() {
        assert_eq!(clamp_motion_i16(1.49), 1);
        assert_eq!(clamp_motion_i16(1.5), 2);
        assert_eq!(clamp_motion_i16(-1.5), -2);
        assert_eq!(clamp_motion_i16(40_000.0), 32_767);
        assert_eq!(clamp_motion_i16(-40_000.0), -32_768);
    }

    #[test]
    fn gyro_deadzone_is_inclusive_at_32() {
        for value in [-32, -1, 0, 1, 32] {
            assert_eq!(gyro_deadzone_i16(value), 0);
        }
        assert_eq!(gyro_deadzone_i16(-33), -33);
        assert_eq!(gyro_deadzone_i16(33), 33);
    }
}
