use ns_shared::protocol::{S2_AUDIO_CAP_MICROPHONE, S2_AUDIO_CAP_PLAYBACK};

pub const CAPS_INTERVAL_US: u64 = 500_000;
pub const AUDIO_RETRY_US: u64 = 5_000_000;
pub const PLAYBACK_IDLE_RESET_US: u64 = 500_000;
const NEVER_ANNOUNCED_FLAGS: u8 = 0xff;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AudioConfig<'a> {
    pub switch2_mode: bool,
    pub playback_requested: bool,
    pub microphone_requested: bool,
    pub playback_device: &'a str,
    pub microphone_device: &'a str,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AudioUpdatePlan {
    pub desired_flags: u8,
    pub reconfigure: bool,
    pub send_zero_if_ineligible: bool,
    pub reset_playback: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AudioLifecycle {
    requested_flags: u8,
    active_flags: u8,
    requested_playback_device: String,
    requested_microphone_device: String,
    next_open_retry_us: u64,
    last_capabilities_send_us: u64,
    last_announced_flags: u8,
    playback_started: bool,
    last_playback_receive_us: u64,
}

impl Default for AudioLifecycle {
    fn default() -> Self {
        Self {
            requested_flags: 0,
            active_flags: 0,
            requested_playback_device: String::new(),
            requested_microphone_device: String::new(),
            next_open_retry_us: 0,
            last_capabilities_send_us: 0,
            last_announced_flags: NEVER_ANNOUNCED_FLAGS,
            playback_started: false,
            last_playback_receive_us: 0,
        }
    }
}

impl AudioLifecycle {
    #[must_use]
    pub const fn requested_flags(&self) -> u8 {
        self.requested_flags
    }

    #[must_use]
    pub const fn active_flags(&self) -> u8 {
        self.active_flags
    }

    #[must_use]
    pub const fn next_open_retry_us(&self) -> u64 {
        self.next_open_retry_us
    }

    #[must_use]
    pub const fn playback_started(&self) -> bool {
        self.playback_started
    }

    #[must_use]
    pub fn desired_flags(config: &AudioConfig<'_>) -> u8 {
        let mut desired = 0;
        if config.switch2_mode && config.playback_requested {
            desired |= S2_AUDIO_CAP_PLAYBACK;
            if config.microphone_requested {
                desired |= S2_AUDIO_CAP_MICROPHONE;
            }
        }
        desired
    }

    #[must_use]
    pub fn update(&mut self, now_us: u64, config: &AudioConfig<'_>) -> AudioUpdatePlan {
        let had_announced_capabilities = self.last_announced_flags != NEVER_ANNOUNCED_FLAGS
            && self.last_announced_flags != 0;
        let desired = Self::desired_flags(config);
        let device_changed = config.playback_device != self.requested_playback_device
            || config.microphone_device != self.requested_microphone_device;
        let retry_due = desired != 0
            && self.active_flags != desired
            && self.next_open_retry_us != 0
            && now_us >= self.next_open_retry_us;
        let reconfigure = desired != self.requested_flags || device_changed || retry_due;

        let reset_playback = self.playback_started
            && self.last_playback_receive_us != 0
            && now_us.wrapping_sub(self.last_playback_receive_us) > PLAYBACK_IDLE_RESET_US;
        if reset_playback {
            self.reset_playback();
        }

        AudioUpdatePlan {
            desired_flags: desired,
            reconfigure,
            send_zero_if_ineligible: !config.switch2_mode && had_announced_capabilities,
            reset_playback,
        }
    }

    pub fn begin_reconfigure(
        &mut self,
        desired_flags: u8,
        playback_device: &str,
        microphone_device: &str,
    ) {
        self.requested_flags = desired_flags;
        self.requested_playback_device.clear();
        self.requested_playback_device.push_str(playback_device);
        self.requested_microphone_device.clear();
        self.requested_microphone_device.push_str(microphone_device);
        self.active_flags = 0;
        self.next_open_retry_us = 0;
        self.playback_started = false;
        self.last_playback_receive_us = 0;
        self.last_announced_flags = NEVER_ANNOUNCED_FLAGS;
    }

    pub fn finish_reconfigure(&mut self, now_us: u64, active_flags: u8) {
        self.active_flags = active_flags;
        self.next_open_retry_us = if active_flags == self.requested_flags || self.requested_flags == 0 {
            0
        } else {
            now_us.wrapping_add(AUDIO_RETRY_US)
        };
        self.last_announced_flags = NEVER_ANNOUNCED_FLAGS;
    }

    #[must_use]
    pub fn capabilities_due(&self, now_us: u64, force: bool) -> Option<u8> {
        if !force
            && self.active_flags == self.last_announced_flags
            && now_us.wrapping_sub(self.last_capabilities_send_us) < CAPS_INTERVAL_US
        {
            None
        } else {
            Some(self.active_flags)
        }
    }

    pub fn mark_capabilities_sent(&mut self, now_us: u64, flags: u8) {
        self.last_capabilities_send_us = now_us;
        self.last_announced_flags = flags;
    }

    pub fn mark_playback_packet(&mut self, now_us: u64) {
        self.playback_started = true;
        self.last_playback_receive_us = now_us;
    }

    pub fn reset_playback(&mut self) {
        self.playback_started = false;
        self.last_playback_receive_us = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config<'a>(
        switch2_mode: bool,
        playback_requested: bool,
        microphone_requested: bool,
        playback_device: &'a str,
        microphone_device: &'a str,
    ) -> AudioConfig<'a> {
        AudioConfig {
            switch2_mode,
            playback_requested,
            microphone_requested,
            playback_device,
            microphone_device,
        }
    }

    #[test]
    fn microphone_is_never_advertised_without_playback() {
        assert_eq!(AudioLifecycle::desired_flags(&config(true, false, true, "", "")), 0);
        assert_eq!(
            AudioLifecycle::desired_flags(&config(true, true, true, "", "")),
            S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE
        );
        assert_eq!(AudioLifecycle::desired_flags(&config(false, true, true, "", "")), 0);
    }

    #[test]
    fn configuration_change_and_failed_open_retry_match_cpp_timing() {
        let mut lifecycle = AudioLifecycle::default();
        let cfg = config(true, true, true, "speakers", "mic");
        let plan = lifecycle.update(1_000, &cfg);
        assert!(plan.reconfigure);
        assert_eq!(plan.desired_flags, S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE);

        lifecycle.begin_reconfigure(plan.desired_flags, cfg.playback_device, cfg.microphone_device);
        lifecycle.finish_reconfigure(1_000, S2_AUDIO_CAP_PLAYBACK);
        assert_eq!(lifecycle.next_open_retry_us(), 1_000 + AUDIO_RETRY_US);

        assert!(!lifecycle.update(1_000 + AUDIO_RETRY_US - 1, &cfg).reconfigure);
        assert!(lifecycle.update(1_000 + AUDIO_RETRY_US, &cfg).reconfigure);
    }

    #[test]
    fn device_name_change_forces_reconfigure_even_when_flags_match() {
        let mut lifecycle = AudioLifecycle::default();
        let original = config(true, true, false, "first", "@default");
        lifecycle.begin_reconfigure(AudioLifecycle::desired_flags(&original), "first", "@default");
        lifecycle.finish_reconfigure(0, S2_AUDIO_CAP_PLAYBACK);
        lifecycle.mark_capabilities_sent(0, S2_AUDIO_CAP_PLAYBACK);

        let changed = config(true, true, false, "second", "@default");
        assert!(lifecycle.update(1, &changed).reconfigure);
    }

    #[test]
    fn capability_announcement_is_immediate_then_throttled_to_half_second() {
        let mut lifecycle = AudioLifecycle::default();
        let cfg = config(true, true, false, "@default", "@default");
        let desired = AudioLifecycle::desired_flags(&cfg);
        lifecycle.begin_reconfigure(desired, cfg.playback_device, cfg.microphone_device);
        lifecycle.finish_reconfigure(10, desired);

        assert_eq!(lifecycle.capabilities_due(10, false), Some(S2_AUDIO_CAP_PLAYBACK));
        lifecycle.mark_capabilities_sent(10, S2_AUDIO_CAP_PLAYBACK);
        assert_eq!(lifecycle.capabilities_due(10 + CAPS_INTERVAL_US - 1, false), None);
        assert_eq!(
            lifecycle.capabilities_due(10 + CAPS_INTERVAL_US, false),
            Some(S2_AUDIO_CAP_PLAYBACK)
        );
    }

    #[test]
    fn leaving_switch2_mode_requests_one_zero_capability_packet() {
        let mut lifecycle = AudioLifecycle::default();
        lifecycle.begin_reconfigure(S2_AUDIO_CAP_PLAYBACK, "@default", "@default");
        lifecycle.finish_reconfigure(0, S2_AUDIO_CAP_PLAYBACK);
        lifecycle.mark_capabilities_sent(100, S2_AUDIO_CAP_PLAYBACK);

        let disabled = config(false, true, false, "@default", "@default");
        let first = lifecycle.update(101, &disabled);
        assert!(first.reconfigure);
        assert!(first.send_zero_if_ineligible);

        lifecycle.begin_reconfigure(0, "@default", "@default");
        lifecycle.finish_reconfigure(101, 0);
        lifecycle.mark_capabilities_sent(101, 0);
        assert!(!lifecycle.update(102, &disabled).send_zero_if_ineligible);
    }

    #[test]
    fn playback_idle_reset_uses_strict_greater_than_half_second_boundary() {
        let mut lifecycle = AudioLifecycle::default();
        lifecycle.mark_playback_packet(1_000);
        let cfg = config(true, true, false, "", "");
        assert!(!lifecycle.update(1_000 + PLAYBACK_IDLE_RESET_US, &cfg).reset_playback);
        assert!(lifecycle.update(1_000 + PLAYBACK_IDLE_RESET_US + 1, &cfg).reset_playback);
        assert!(!lifecycle.playback_started());
    }

    #[test]
    fn successful_reconfigure_cancels_retry() {
        let mut lifecycle = AudioLifecycle::default();
        lifecycle.begin_reconfigure(S2_AUDIO_CAP_PLAYBACK, "", "");
        lifecycle.finish_reconfigure(50, S2_AUDIO_CAP_PLAYBACK);
        assert_eq!(lifecycle.active_flags(), S2_AUDIO_CAP_PLAYBACK);
        assert_eq!(lifecycle.next_open_retry_us(), 0);
    }
}
