use ns_shared::protocol::{PrecisionRumblePacket, RumblePacket};

pub const RUMBLE_SLOT_COUNT: usize = 4;
const CLASSIC_SUPPRESSION_US: u64 = 20_000;
const DUPLICATE_REFRESH_US: u64 = 100_000;
const MIN_RUMBLE_DURATION_MS: u32 = 40;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RumbleState {
    packet: Option<RumblePacket>,
}

impl RumbleState {
    pub fn update(&mut self, packet: RumblePacket) {
        self.packet = Some(packet);
    }

    #[must_use]
    pub const fn current(&self) -> Option<RumblePacket> {
        self.packet
    }

    pub fn clear(&mut self) {
        self.packet = None;
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RumbleOutput {
    pub slot: u8,
    pub controller: i32,
    pub low: u8,
    pub high: u8,
    pub duration_ms: u32,
}

impl RumbleOutput {
    const fn new(
        slot: usize,
        controller: i32,
        low: u8,
        high: u8,
        duration_ms: u32,
    ) -> Self {
        Self {
            slot: slot as u8,
            controller,
            low,
            high,
            duration_ms,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct SlotState {
    low: u8,
    high: u8,
    until_us: u64,
    last_set_us: u64,
    suppress_classic_until_us: u64,
    last_controller: i32,
}

impl Default for SlotState {
    fn default() -> Self {
        Self {
            low: 0,
            high: 0,
            until_us: 0,
            last_set_us: 0,
            suppress_classic_until_us: 0,
            last_controller: -1,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RumbleManager {
    states: [SlotState; RUMBLE_SLOT_COUNT],
    enabled: bool,
}

impl Default for RumbleManager {
    fn default() -> Self {
        Self {
            states: [SlotState::default(); RUMBLE_SLOT_COUNT],
            enabled: true,
        }
    }
}

impl RumbleManager {
    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
    }

    #[must_use]
    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    #[must_use]
    pub fn apply_precision_packet_at(
        &mut self,
        packet: PrecisionRumblePacket,
        controller_for_slot: [i32; RUMBLE_SLOT_COUNT],
        now_us: u64,
    ) -> Vec<RumbleOutput> {
        let encoded = packet.encode();
        let subpad = encoded[4];
        let slot = usize::from(subpad);
        if slot >= RUMBLE_SLOT_COUNT {
            return Vec::new();
        }

        // Match the C++ client exactly: a precision packet must be allowed through
        // even if a previous precision packet opened the classic-packet suppression
        // window. The window is restored only after the precision fallback is applied.
        self.states[slot].suppress_classic_until_us = 0;
        let fallback = RumblePacket::new(subpad, encoded[5], encoded[6], encoded[7]);
        let outputs = self.apply_packet_at(fallback, controller_for_slot, now_us);
        self.states[slot].suppress_classic_until_us =
            now_us.saturating_add(CLASSIC_SUPPRESSION_US);
        outputs
    }

    #[must_use]
    pub fn apply_packet_at(
        &mut self,
        packet: RumblePacket,
        controller_for_slot: [i32; RUMBLE_SLOT_COUNT],
        now_us: u64,
    ) -> Vec<RumbleOutput> {
        let [subpad, low, high, duration_10ms] = packet.components();
        let slot = usize::from(subpad);
        if slot >= RUMBLE_SLOT_COUNT || !self.enabled {
            return Vec::new();
        }

        let state = &mut self.states[slot];
        if now_us < state.suppress_classic_until_us {
            return Vec::new();
        }

        let neutral = (low == 0 && high == 0) || duration_10ms == 0;
        let duration_ms = if neutral {
            0
        } else {
            u32::from(duration_10ms)
                .saturating_mul(10)
                .max(MIN_RUMBLE_DURATION_MS)
        };
        let duration_us = u64::from(duration_ms).saturating_mul(1_000);

        if !neutral
            && state.low == low
            && state.high == high
            && now_us.saturating_sub(state.last_set_us) < DUPLICATE_REFRESH_US
        {
            state.until_us = now_us.saturating_add(duration_us);
            return Vec::new();
        }

        state.low = low;
        state.high = high;
        state.until_us = if neutral {
            0
        } else {
            now_us.saturating_add(duration_us)
        };
        state.last_set_us = now_us;

        self.set_output(
            slot,
            if neutral { 0 } else { low },
            if neutral { 0 } else { high },
            duration_ms,
            controller_for_slot[slot],
        )
    }

    #[must_use]
    pub fn update_timeouts_at(
        &mut self,
        controller_for_slot: [i32; RUMBLE_SLOT_COUNT],
        now_us: u64,
    ) -> Vec<RumbleOutput> {
        let mut outputs = Vec::new();
        for (slot, controller) in controller_for_slot.into_iter().enumerate() {
            let timed_out = {
                let state = &mut self.states[slot];
                if state.until_us != 0 && now_us > state.until_us {
                    state.until_us = 0;
                    state.low = 0;
                    state.high = 0;
                    true
                } else {
                    false
                }
            };
            if timed_out {
                outputs.extend(self.set_output(slot, 0, 0, 0, controller));
            }
        }
        outputs
    }

    #[must_use]
    pub fn stop_all(&mut self) -> Vec<RumbleOutput> {
        let mut outputs = Vec::new();
        for slot in 0..RUMBLE_SLOT_COUNT {
            outputs.extend(self.set_output(slot, 0, 0, 0, -1));
        }
        outputs
    }

    #[must_use]
    pub fn active_until_us(&self, slot: usize) -> Option<u64> {
        self.states.get(slot).map(|state| state.until_us)
    }

    fn set_output(
        &mut self,
        slot: usize,
        low: u8,
        high: u8,
        duration_ms: u32,
        controller: i32,
    ) -> Vec<RumbleOutput> {
        let previous_controller = self.states[slot].last_controller;
        let mut outputs = Vec::with_capacity(2);
        if previous_controller != -1 && previous_controller != controller {
            outputs.push(RumbleOutput::new(slot, previous_controller, 0, 0, 0));
        }
        if controller >= 0 {
            outputs.push(RumbleOutput::new(
                slot,
                controller,
                low,
                high,
                if low != 0 || high != 0 {
                    duration_ms
                } else {
                    0
                },
            ));
        }
        self.states[slot].last_controller = controller;
        outputs
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CONTROLLERS: [i32; RUMBLE_SLOT_COUNT] = [10, 11, 12, 13];

    #[test]
    fn classic_packet_uses_cpp_minimum_duration() {
        let mut manager = RumbleManager::default();
        let outputs = manager.apply_packet_at(RumblePacket::new(1, 20, 40, 1), CONTROLLERS, 1_000);
        assert_eq!(
            outputs,
            vec![RumbleOutput::new(1, 11, 20, 40, MIN_RUMBLE_DURATION_MS)]
        );
        assert_eq!(manager.active_until_us(1), Some(41_000));
    }

    #[test]
    fn neutral_packet_stops_current_controller() {
        let mut manager = RumbleManager::default();
        let _ = manager.apply_packet_at(RumblePacket::new(0, 30, 60, 8), CONTROLLERS, 5_000);
        let outputs = manager.apply_packet_at(RumblePacket::new(0, 30, 60, 0), CONTROLLERS, 6_000);
        assert_eq!(outputs, vec![RumbleOutput::new(0, 10, 0, 0, 0)]);
        assert_eq!(manager.active_until_us(0), Some(0));
    }

    #[test]
    fn duplicate_packet_extends_timeout_without_restarting_output() {
        let mut manager = RumbleManager::default();
        let packet = RumblePacket::new(0, 50, 80, 5);
        let first = manager.apply_packet_at(packet, CONTROLLERS, 10_000);
        assert_eq!(first, vec![RumbleOutput::new(0, 10, 50, 80, 50)]);

        let duplicate = manager.apply_packet_at(packet, CONTROLLERS, 50_000);
        assert!(duplicate.is_empty());
        assert_eq!(manager.active_until_us(0), Some(100_000));
    }

    #[test]
    fn precision_packet_suppresses_only_following_classic_duplicate() {
        let mut manager = RumbleManager::default();
        let precision = PrecisionRumblePacket::new(2, 70, 90, 6, [1; 8]);
        let outputs = manager.apply_precision_packet_at(precision, CONTROLLERS, 100_000);
        assert_eq!(outputs, vec![RumbleOutput::new(2, 12, 70, 90, 60)]);

        let classic = manager.apply_packet_at(
            RumblePacket::new(2, 70, 90, 6),
            CONTROLLERS,
            110_000,
        );
        assert!(classic.is_empty());

        let after_window = manager.apply_packet_at(
            RumblePacket::new(2, 60, 80, 6),
            CONTROLLERS,
            121_000,
        );
        assert_eq!(after_window, vec![RumbleOutput::new(2, 12, 60, 80, 60)]);
    }

    #[test]
    fn consecutive_precision_packets_are_not_suppressed() {
        let mut manager = RumbleManager::default();
        let first = manager.apply_precision_packet_at(
            PrecisionRumblePacket::new(3, 10, 20, 5, [0; 8]),
            CONTROLLERS,
            200_000,
        );
        assert_eq!(first, vec![RumbleOutput::new(3, 13, 10, 20, 50)]);

        let second = manager.apply_precision_packet_at(
            PrecisionRumblePacket::new(3, 30, 40, 5, [0; 8]),
            CONTROLLERS,
            205_000,
        );
        assert_eq!(second, vec![RumbleOutput::new(3, 13, 30, 40, 50)]);
    }

    #[test]
    fn timeout_uses_strict_cpp_greater_than_boundary() {
        let mut manager = RumbleManager::default();
        let _ = manager.apply_packet_at(RumblePacket::new(0, 20, 40, 4), CONTROLLERS, 1_000);
        assert!(manager.update_timeouts_at(CONTROLLERS, 41_000).is_empty());
        assert_eq!(
            manager.update_timeouts_at(CONTROLLERS, 41_001),
            vec![RumbleOutput::new(0, 10, 0, 0, 0)]
        );
    }

    #[test]
    fn changing_controller_stops_old_controller_before_starting_new_one() {
        let mut manager = RumbleManager::default();
        let packet = RumblePacket::new(0, 20, 40, 5);
        let _ = manager.apply_packet_at(packet, CONTROLLERS, 1_000);
        let remapped = [21, 11, 12, 13];
        let outputs = manager.apply_packet_at(RumblePacket::new(0, 30, 50, 5), remapped, 101_000);
        assert_eq!(
            outputs,
            vec![
                RumbleOutput::new(0, 10, 0, 0, 0),
                RumbleOutput::new(0, 21, 30, 50, 50),
            ]
        );
    }

    #[test]
    fn disabled_manager_ignores_packets_without_mutating_timeout() {
        let mut manager = RumbleManager::default();
        manager.set_enabled(false);
        let outputs = manager.apply_packet_at(RumblePacket::new(0, 20, 40, 5), CONTROLLERS, 1_000);
        assert!(outputs.is_empty());
        assert_eq!(manager.active_until_us(0), Some(0));
    }

    #[test]
    fn invalid_slots_are_ignored() {
        let mut manager = RumbleManager::default();
        assert!(manager
            .apply_packet_at(RumblePacket::new(4, 20, 40, 5), CONTROLLERS, 1_000)
            .is_empty());
        assert!(manager
            .apply_precision_packet_at(
                PrecisionRumblePacket::new(4, 20, 40, 5, [0; 8]),
                CONTROLLERS,
                1_000,
            )
            .is_empty());
    }

    #[test]
    fn stop_all_stops_each_previous_controller_and_detaches_slots() {
        let mut manager = RumbleManager::default();
        for slot in 0..RUMBLE_SLOT_COUNT {
            let _ = manager.apply_packet_at(
                RumblePacket::new(slot as u8, 20, 40, 5),
                CONTROLLERS,
                1_000,
            );
        }
        assert_eq!(
            manager.stop_all(),
            vec![
                RumbleOutput::new(0, 10, 0, 0, 0),
                RumbleOutput::new(1, 11, 0, 0, 0),
                RumbleOutput::new(2, 12, 0, 0, 0),
                RumbleOutput::new(3, 13, 0, 0, 0),
            ]
        );
        assert!(manager.stop_all().is_empty());
    }
}
