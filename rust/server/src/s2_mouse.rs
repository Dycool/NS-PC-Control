use ns_shared::joycon_mouse::JoyconMousePacket;

pub const JOYCON_MOUSE_TIMEOUT_US: u64 = 250_000;
const MAX_PENDING: i64 = 1_i64 << 30;
const USB_FRAME_US: u64 = 4_000;
const MAX_SMOOTHING_FRAMES: u64 = 4;
const SCROLL_NOTCH_UNITS: u64 = 120;
const SCROLL_NOTCH_HOLD_US: u64 = 40_000;
const MAX_SCROLL_NOTCHES: u64 = 8;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MouseSample {
    pub dx: i16,
    pub dy: i16,
    pub scroll_y: i8,
    pub left_down: bool,
    pub right_down: bool,
    pub active: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MouseStreamState {
    active: bool,
    first_packet: bool,
    last_sequence: u32,
    last_rx_us: u64,
    last_client_timestamp_us: u64,
    pending_x: i64,
    pending_y: i64,
    smoothing_frames: u8,
    left_down: bool,
    right_down: bool,
    scroll_direction: i8,
    scroll_until_us: u64,
}

impl Default for MouseStreamState {
    fn default() -> Self {
        Self {
            active: false,
            first_packet: true,
            last_sequence: 0,
            last_rx_us: 0,
            last_client_timestamp_us: 0,
            pending_x: 0,
            pending_y: 0,
            smoothing_frames: 0,
            left_down: false,
            right_down: false,
            scroll_direction: 0,
            scroll_until_us: 0,
        }
    }
}

impl MouseStreamState {
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    #[must_use]
    pub fn update(&mut self, packet: JoyconMousePacket, now_us: u64) -> bool {
        if !self.first_packet && !sequence_is_newer(packet.sequence(), self.last_sequence) {
            return false;
        }
        self.first_packet = false;
        self.last_sequence = packet.sequence();
        self.last_rx_us = now_us;

        self.active = packet.active();
        self.left_down = packet.left_down();
        self.right_down = packet.right_down();
        if !self.active {
            self.last_client_timestamp_us = 0;
            self.pending_x = 0;
            self.pending_y = 0;
            self.smoothing_frames = 0;
            self.scroll_direction = 0;
            self.scroll_until_us = 0;
            return true;
        }

        self.pending_x = self
            .pending_x
            .saturating_add(i64::from(packet.delta_x()))
            .clamp(-MAX_PENDING, MAX_PENDING);
        self.pending_y = self
            .pending_y
            .saturating_add(i64::from(packet.delta_y()))
            .clamp(-MAX_PENDING, MAX_PENDING);
        if packet.delta_x() != 0 || packet.delta_y() != 0 {
            let mut frame_count = 1_u64;
            if self.last_client_timestamp_us != 0
                && packet.timestamp_us() > self.last_client_timestamp_us
            {
                frame_count = packet
                    .timestamp_us()
                    .saturating_sub(self.last_client_timestamp_us)
                    .saturating_add(USB_FRAME_US - 1)
                    / USB_FRAME_US;
                frame_count = frame_count.clamp(1, MAX_SMOOTHING_FRAMES);
            }
            self.smoothing_frames = self
                .smoothing_frames
                .max(u8::try_from(frame_count).unwrap_or(MAX_SMOOTHING_FRAMES as u8));
        }
        self.last_client_timestamp_us = packet.timestamp_us();

        if packet.scroll_y() != 0 {
            let direction = if packet.scroll_y() > 0 { 1 } else { -1 };
            let units = u64::from(packet.scroll_y().unsigned_abs()).min(
                MAX_SCROLL_NOTCHES.saturating_mul(SCROLL_NOTCH_UNITS),
            );
            let notches = units
                .saturating_add(SCROLL_NOTCH_UNITS - 1)
                .checked_div(SCROLL_NOTCH_UNITS)
                .unwrap_or(1)
                .clamp(1, MAX_SCROLL_NOTCHES);
            let base = if self.scroll_direction == direction && self.scroll_until_us > now_us {
                self.scroll_until_us
            } else {
                now_us
            };
            self.scroll_direction = direction;
            self.scroll_until_us = base.saturating_add(
                notches.saturating_mul(SCROLL_NOTCH_HOLD_US),
            );
        }
        true
    }

    #[must_use]
    pub fn consume(&mut self, now_us: u64, feature_enabled: bool) -> MouseSample {
        if !self.active
            || self.last_rx_us == 0
            || now_us.saturating_sub(self.last_rx_us) > JOYCON_MOUSE_TIMEOUT_US
        {
            self.deactivate_stream();
            return MouseSample::default();
        }

        if !feature_enabled {
            self.pending_x = 0;
            self.pending_y = 0;
            self.smoothing_frames = 0;
            self.scroll_direction = 0;
            self.scroll_until_us = 0;
            return MouseSample::default();
        }

        let smoothing_frames = i64::from(self.smoothing_frames.max(1));
        let dx = smoothed_axis(self.pending_x, smoothing_frames);
        let dy = smoothed_axis(self.pending_y, smoothing_frames);
        self.pending_x -= dx;
        self.pending_y -= dy;
        self.smoothing_frames = self.smoothing_frames.saturating_sub(1);

        let scroll_y = if now_us < self.scroll_until_us {
            self.scroll_direction
        } else {
            self.scroll_direction = 0;
            self.scroll_until_us = 0;
            0
        };

        MouseSample {
            dx: i16::try_from(dx).expect("smoothed mouse X is clamped to i16"),
            dy: i16::try_from(dy).expect("smoothed mouse Y is clamped to i16"),
            scroll_y,
            left_down: self.left_down,
            right_down: self.right_down,
            active: true,
        }
    }

    fn deactivate_stream(&mut self) {
        self.active = false;
        self.last_client_timestamp_us = 0;
        self.pending_x = 0;
        self.pending_y = 0;
        self.smoothing_frames = 0;
        self.left_down = false;
        self.right_down = false;
        self.scroll_direction = 0;
        self.scroll_until_us = 0;
    }
}

fn smoothed_axis(pending: i64, smoothing_frames: i64) -> i64 {
    let mut value = pending / smoothing_frames;
    if value == 0 && pending != 0 {
        value = if pending > 0 { 1 } else { -1 };
    }
    value.clamp(i64::from(i16::MIN), i64::from(i16::MAX))
}

#[must_use]
const fn sequence_is_newer(sequence: u32, previous: u32) -> bool {
    let delta = sequence.wrapping_sub(previous);
    delta != 0 && delta < 0x8000_0000
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::joycon_mouse::{
        JOYCON_MOUSE_FLAG_ACTIVE, JOYCON_MOUSE_FLAG_LEFT_BUTTON,
    };

    fn packet(sequence: u32, dx: i32, dy: i32, scroll: i32, timestamp_us: u64) -> JoyconMousePacket {
        JoyconMousePacket::new(
            JOYCON_MOUSE_FLAG_ACTIVE | JOYCON_MOUSE_FLAG_LEFT_BUTTON,
            0,
            sequence,
            dx,
            dy,
            scroll,
            timestamp_us,
        )
    }

    #[test]
    fn sequence_is_wrap_aware_and_drops_duplicates() {
        let mut state = MouseStreamState::default();
        assert!(state.update(packet(u32::MAX, 1, 0, 0, 1), 1));
        assert!(state.update(packet(0, 1, 0, 0, 2), 2));
        assert!(!state.update(packet(0, 99, 0, 0, 3), 3));
        assert!(!state.update(packet(u32::MAX, 99, 0, 0, 4), 4));
    }

    #[test]
    fn timestamp_gap_smooths_motion_across_up_to_four_frames() {
        let mut state = MouseStreamState::default();
        assert!(state.update(packet(1, 0, 0, 0, 1_000), 1_000));
        assert!(state.update(packet(2, 40, -20, 0, 17_000), 2_000));
        let first = state.consume(2_000, true);
        assert_eq!((first.dx, first.dy), (10, -5));
        let second = state.consume(6_000, true);
        assert_eq!((second.dx, second.dy), (10, -5));
        let third = state.consume(10_000, true);
        assert_eq!((third.dx, third.dy), (10, -5));
        let fourth = state.consume(14_000, true);
        assert_eq!((fourth.dx, fourth.dy), (10, -5));
    }

    #[test]
    fn disabled_feature_flushes_motion_before_reenable() {
        let mut state = MouseStreamState::default();
        assert!(state.update(packet(1, 100, 50, 0, 1), 1));
        assert_eq!(state.consume(2, false), MouseSample::default());
        let enabled = state.consume(3, true);
        assert!(enabled.active);
        assert_eq!((enabled.dx, enabled.dy), (0, 0));
        assert!(enabled.left_down);
    }

    #[test]
    fn wheel_notches_hold_for_40ms_each_and_extend_same_direction() {
        let mut state = MouseStreamState::default();
        assert!(state.update(packet(1, 0, 0, 240, 1), 10));
        assert_eq!(state.consume(79_999, true).scroll_y, 1);
        assert_eq!(state.consume(80_010, true).scroll_y, 0);

        assert!(state.update(packet(2, 0, 0, -120, 2), 100_000));
        assert!(state.update(packet(3, 0, 0, -120, 3), 120_000));
        assert_eq!(state.consume(179_999, true).scroll_y, -1);
        assert_eq!(state.consume(180_001, true).scroll_y, 0);
    }

    #[test]
    fn stream_times_out_after_250ms() {
        let mut state = MouseStreamState::default();
        assert!(state.update(packet(1, 10, 10, 0, 1), 5_000));
        assert!(state.consume(255_000, true).active);
        assert_eq!(state.consume(255_001, true), MouseSample::default());
    }
}
