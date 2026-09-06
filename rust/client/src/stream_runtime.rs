use crate::input_settings::KEYBOARD_MODE_SINGLE;
use crate::udp_protocol::UdpClient;
use ns_shared::protocol::{ControllerType, MultiReport, DEFAULT_PORT};
use std::io;
use std::time::{Duration, Instant};

pub struct StreamRuntime {
    interval: Duration,
    next_deadline: Instant,
}

impl StreamRuntime {
    #[must_use]
    pub fn new(hz: u32) -> Self {
        let hz = hz.max(1);
        Self {
            interval: Duration::from_nanos(1_000_000_000_u64 / u64::from(hz)),
            next_deadline: Instant::now(),
        }
    }

    pub fn tick(&mut self, client: &mut UdpClient, report: MultiReport) -> io::Result<()> {
        let now = Instant::now();
        if now < self.next_deadline {
            std::thread::sleep(self.next_deadline - now);
        }
        client.send_report(report, 0)?;
        self.next_deadline += self.interval;
        let after = Instant::now();
        if self.next_deadline < after {
            self.next_deadline = after;
        }
        Ok(())
    }
}

impl Default for StreamRuntime {
    fn default() -> Self {
        Self::new(250)
    }
}

/// Mirrors the C++ `parse_host_port` connection-target parser.
///
/// The legacy parser intentionally treats the first colon as the port separator,
/// uses `atoi`-style prefix parsing for the port, preserves the default port when
/// the parsed value is outside 1..=65535, and trims the host after removing the
/// suffix. Keeping those details matters for CLI/settings compatibility.
#[must_use]
pub fn parse_host_port(input: &str) -> Option<(String, u16)> {
    let mut value = input.trim();
    if value.is_empty() {
        return None;
    }

    let mut port = DEFAULT_PORT;
    if let Some(colon) = value.find(':') {
        let parsed = atoi_prefix(&value[colon + 1..]);
        if (1..=65_535).contains(&parsed) {
            port = parsed as u16;
        }
        value = &value[..colon];
    }

    let host = value.trim();
    if host.is_empty() {
        None
    } else {
        Some((host.to_string(), port))
    }
}

fn atoi_prefix(input: &str) -> i32 {
    let bytes = input.as_bytes();
    let mut index = 0;
    while index < bytes.len() && bytes[index].is_ascii_whitespace() {
        index += 1;
    }

    let mut negative = false;
    if let Some(byte) = bytes.get(index) {
        match byte {
            b'+' => index += 1,
            b'-' => {
                negative = true;
                index += 1;
            }
            _ => {}
        }
    }

    let start = index;
    let mut magnitude: i64 = 0;
    while let Some(byte) = bytes.get(index) {
        if !byte.is_ascii_digit() {
            break;
        }
        magnitude = (magnitude * 10 + i64::from(byte - b'0')).min(i64::from(i32::MAX) + 1);
        index += 1;
    }
    if index == start {
        return 0;
    }

    let signed = if negative { -magnitude } else { magnitude };
    signed.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32
}

/// Inputs used by the C++ `requested_controller_profile_for_frame` decision.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RequestedControllerProfile {
    pub hori_mode: bool,
    pub controller_type: i32,
    pub switch2_mode: bool,
    pub keyboard_mode: i32,
    pub joycon_mouse_mode: bool,
}

/// Returns the controller identity that must be advertised for the next frame.
///
/// This preserves the C++ ordering: HORI wins absolutely; Switch 2 single-keyboard
/// mode promotes the default/full controller to Pro Controller 2 unless native
/// Joy-Con mouse is active or a lone Joy-Con was explicitly selected; normal S2
/// mode maps legacy controller identities to their S2 counterparts; unknown raw
/// values fail closed to the legacy Pro identity.
#[must_use]
pub fn requested_controller_profile(input: RequestedControllerProfile) -> ControllerType {
    if input.hori_mode {
        return ControllerType::Hori;
    }

    let single_joycon = matches!(input.controller_type, 1 | 2);
    if input.switch2_mode
        && input.keyboard_mode == KEYBOARD_MODE_SINGLE
        && !input.joycon_mouse_mode
        && !single_joycon
    {
        return ControllerType::ProS2;
    }

    let raw = if input.switch2_mode {
        match input.controller_type {
            3 => 6,
            1 => 7,
            2 => 8,
            4 => 9,
            other => other,
        }
    } else {
        input.controller_type
    };

    match raw {
        1 => ControllerType::JoyconL,
        2 => ControllerType::JoyconR,
        3 => ControllerType::Pro,
        4 => ControllerType::JoyconPair,
        6 => ControllerType::ProS2,
        7 => ControllerType::JoyconLS2,
        8 => ControllerType::JoyconRS2,
        9 => ControllerType::JoyconPairS2,
        _ => ControllerType::Pro,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input_settings::{KEYBOARD_MODE_OFF, KEYBOARD_MODE_OVERRIDE};

    #[test]
    fn host_port_defaults_and_trims_like_cpp() {
        assert_eq!(
            parse_host_port(" 192.168.1.20 "),
            Some(("192.168.1.20".to_string(), DEFAULT_PORT))
        );
        assert_eq!(parse_host_port("   "), None);
        assert_eq!(parse_host_port(":7331"), None);
    }

    #[test]
    fn host_port_uses_first_colon_and_atoi_prefix() {
        assert_eq!(
            parse_host_port("host:1234junk"),
            Some(("host".to_string(), 1234))
        );
        assert_eq!(
            parse_host_port("host: +42 trailing"),
            Some(("host".to_string(), 42))
        );
        assert_eq!(
            parse_host_port("host:0"),
            Some(("host".to_string(), DEFAULT_PORT))
        );
        assert_eq!(
            parse_host_port("host:65536"),
            Some(("host".to_string(), DEFAULT_PORT))
        );
        assert_eq!(
            parse_host_port("host:not-a-port"),
            Some(("host".to_string(), DEFAULT_PORT))
        );
        assert_eq!(
            parse_host_port("2001:db8::1"),
            Some(("2001".to_string(), DEFAULT_PORT))
        );
    }

    fn profile(controller_type: i32) -> RequestedControllerProfile {
        RequestedControllerProfile {
            hori_mode: false,
            controller_type,
            switch2_mode: false,
            keyboard_mode: KEYBOARD_MODE_OFF,
            joycon_mouse_mode: false,
        }
    }

    #[test]
    fn hori_override_wins_over_every_other_profile_input() {
        let mut input = profile(9);
        input.hori_mode = true;
        input.switch2_mode = true;
        input.keyboard_mode = KEYBOARD_MODE_SINGLE;
        assert_eq!(requested_controller_profile(input), ControllerType::Hori);
    }

    #[test]
    fn switch2_maps_all_legacy_profiles() {
        for (raw, expected) in [
            (1, ControllerType::JoyconLS2),
            (2, ControllerType::JoyconRS2),
            (3, ControllerType::ProS2),
            (4, ControllerType::JoyconPairS2),
        ] {
            let mut input = profile(raw);
            input.switch2_mode = true;
            assert_eq!(requested_controller_profile(input), expected);
        }
    }

    #[test]
    fn switch2_single_keyboard_forces_full_pro2_except_cpp_exclusions() {
        for raw in [0, 3, 4, 5, 99] {
            let mut input = profile(raw);
            input.switch2_mode = true;
            input.keyboard_mode = KEYBOARD_MODE_SINGLE;
            assert_eq!(requested_controller_profile(input), ControllerType::ProS2);
        }

        for (raw, expected) in [(1, ControllerType::JoyconLS2), (2, ControllerType::JoyconRS2)] {
            let mut input = profile(raw);
            input.switch2_mode = true;
            input.keyboard_mode = KEYBOARD_MODE_SINGLE;
            assert_eq!(requested_controller_profile(input), expected);
        }

        let mut mouse = profile(3);
        mouse.switch2_mode = true;
        mouse.keyboard_mode = KEYBOARD_MODE_SINGLE;
        mouse.joycon_mouse_mode = true;
        assert_eq!(requested_controller_profile(mouse), ControllerType::ProS2);
    }

    #[test]
    fn non_single_keyboard_modes_do_not_trigger_single_keyboard_override() {
        for keyboard_mode in [KEYBOARD_MODE_OFF, KEYBOARD_MODE_OVERRIDE, 99] {
            let mut input = profile(4);
            input.switch2_mode = true;
            input.keyboard_mode = keyboard_mode;
            assert_eq!(requested_controller_profile(input), ControllerType::JoyconPairS2);
        }
    }

    #[test]
    fn valid_native_s2_profiles_survive_and_unknown_values_fall_back_to_pro() {
        for (raw, expected) in [
            (6, ControllerType::ProS2),
            (7, ControllerType::JoyconLS2),
            (8, ControllerType::JoyconRS2),
            (9, ControllerType::JoyconPairS2),
        ] {
            assert_eq!(requested_controller_profile(profile(raw)), expected);
        }
        assert_eq!(requested_controller_profile(profile(0)), ControllerType::Pro);
        assert_eq!(requested_controller_profile(profile(5)), ControllerType::Pro);
        assert_eq!(requested_controller_profile(profile(1234)), ControllerType::Pro);
    }
}
