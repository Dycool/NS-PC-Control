use crate::app_state::UsbControllerFamily;
use crate::legacy_layout::{LegacySlot, LEGACY_PORTS};
use ns_shared::protocol::ControllerType;
use std::sync::{Mutex, OnceLock};

pub const S1_TYPE_REENUM_QUIET_US: u64 = 1_000_000;

#[derive(Clone, Debug)]
struct IdentityState {
    family: UsbControllerFamily,
    enumerated: [ControllerType; LEGACY_PORTS],
    live: [ControllerType; LEGACY_PORTS],
    change_us: u64,
}

impl Default for IdentityState {
    fn default() -> Self {
        Self::for_family(UsbControllerFamily::Switch1)
    }
}

impl IdentityState {
    fn for_family(family: UsbControllerFamily) -> Self {
        let idle = idle_profile(family);
        Self {
            family,
            enumerated: [idle; LEGACY_PORTS],
            live: [idle; LEGACY_PORTS],
            change_us: 0,
        }
    }

    fn observe(
        &mut self,
        family: UsbControllerFamily,
        slots: &[LegacySlot; LEGACY_PORTS],
        now_us: u64,
    ) {
        if self.family != family {
            *self = Self::for_family(family);
        }
        let next = std::array::from_fn(|port| profile_for_slot(slots[port], family));
        if next != self.live {
            self.live = next;
            self.change_us = if family != UsbControllerFamily::Hori && self.live != self.enumerated {
                now_us
            } else {
                0
            };
        } else if family != UsbControllerFamily::Hori
            && self.live != self.enumerated
            && self.change_us == 0
        {
            // The console may still have the previous identity latched even if
            // the writer was reset while the requested live identity stayed the
            // same. Schedule once without extending the quiet deadline at 250Hz.
            self.change_us = now_us;
        } else if self.live == self.enumerated {
            self.change_us = 0;
        }
    }

    fn due(&mut self, now_us: u64) -> bool {
        if self.family == UsbControllerFamily::Hori || self.change_us == 0 {
            return false;
        }
        if self.live == self.enumerated {
            self.change_us = 0;
            return false;
        }
        now_us.saturating_sub(self.change_us) >= S1_TYPE_REENUM_QUIET_US
    }

    fn mark_enumerated(&mut self) {
        self.enumerated = self.live;
        self.change_us = 0;
    }

    const fn live_profiles(&self) -> [ControllerType; LEGACY_PORTS] {
        self.live
    }

    const fn enumerated_profiles(&self) -> [ControllerType; LEGACY_PORTS] {
        self.enumerated
    }
}

fn state() -> &'static Mutex<IdentityState> {
    static STATE: OnceLock<Mutex<IdentityState>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(IdentityState::default()))
}

pub fn observe_legacy_identity(
    family: UsbControllerFamily,
    slots: &[LegacySlot; LEGACY_PORTS],
    now_us: u64,
) {
    let mut state = state().lock().unwrap_or_else(|poison| poison.into_inner());
    state.observe(family, slots, now_us);
}

#[must_use]
pub fn legacy_identity_reenumeration_due(now_us: u64) -> bool {
    state()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .due(now_us)
}

#[must_use]
pub fn legacy_identity_live_profiles() -> [ControllerType; LEGACY_PORTS] {
    state()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .live_profiles()
}

#[must_use]
pub fn legacy_identity_enumerated_profiles() -> [ControllerType; LEGACY_PORTS] {
    state()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .enumerated_profiles()
}

pub fn mark_legacy_identity_enumerated() {
    state()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .mark_enumerated();
}

pub fn reset_legacy_identity(family: UsbControllerFamily) {
    *state().lock().unwrap_or_else(|poison| poison.into_inner()) = IdentityState::for_family(family);
}

fn idle_profile(family: UsbControllerFamily) -> ControllerType {
    match family {
        UsbControllerFamily::Switch1 => ControllerType::Pro,
        UsbControllerFamily::Switch2 => ControllerType::ProS2,
        UsbControllerFamily::Hori => ControllerType::Hori,
    }
}

fn profile_for_slot(slot: LegacySlot, family: UsbControllerFamily) -> ControllerType {
    if slot.client_index().is_none() {
        return idle_profile(family);
    }
    match slot.virtual_type() {
        ControllerType::JoyconL | ControllerType::JoyconLS2 => {
            if family == UsbControllerFamily::Switch2 {
                ControllerType::JoyconLS2
            } else {
                ControllerType::JoyconL
            }
        }
        ControllerType::JoyconR | ControllerType::JoyconRS2 => {
            if family == UsbControllerFamily::Switch2 {
                ControllerType::JoyconRS2
            } else {
                ControllerType::JoyconR
            }
        }
        ControllerType::Hori => ControllerType::Hori,
        _ => idle_profile(family),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mismatch_requires_full_one_second_quiet_window() {
        let mut state = IdentityState::for_family(UsbControllerFamily::Switch1);
        state.live[0] = ControllerType::JoyconL;
        state.change_us = 10;
        assert!(!state.due(10 + S1_TYPE_REENUM_QUIET_US - 1));
        assert!(state.due(10 + S1_TYPE_REENUM_QUIET_US));
    }

    #[test]
    fn reverting_to_enumerated_identity_cancels_reconnect() {
        let mut state = IdentityState::for_family(UsbControllerFamily::Switch1);
        state.live[0] = ControllerType::JoyconR;
        state.change_us = 100;
        state.live = state.enumerated;
        assert!(!state.due(2_000_000));
        assert_eq!(state.change_us, 0);
    }

    #[test]
    fn mark_enumerated_latches_current_live_identity() {
        let mut state = IdentityState::for_family(UsbControllerFamily::Switch1);
        state.live[0] = ControllerType::JoyconL;
        state.change_us = 1;
        state.mark_enumerated();
        assert_eq!(state.enumerated[0], ControllerType::JoyconL);
        assert_eq!(state.change_us, 0);
    }

    #[test]
    fn profile_snapshots_preserve_live_and_console_latched_identity() {
        let mut state = IdentityState::for_family(UsbControllerFamily::Switch1);
        assert_eq!(state.live_profiles(), [ControllerType::Pro; LEGACY_PORTS]);
        assert_eq!(
            state.enumerated_profiles(),
            [ControllerType::Pro; LEGACY_PORTS]
        );

        state.live[0] = ControllerType::JoyconR;
        assert_eq!(state.live_profiles()[0], ControllerType::JoyconR);
        assert_eq!(state.enumerated_profiles()[0], ControllerType::Pro);

        state.mark_enumerated();
        assert_eq!(state.enumerated_profiles(), state.live_profiles());
    }

    #[test]
    fn family_reset_exposes_family_native_idle_identity() {
        let s1 = IdentityState::for_family(UsbControllerFamily::Switch1);
        let s2 = IdentityState::for_family(UsbControllerFamily::Switch2);
        let hori = IdentityState::for_family(UsbControllerFamily::Hori);
        assert_eq!(s1.live_profiles(), [ControllerType::Pro; LEGACY_PORTS]);
        assert_eq!(s2.live_profiles(), [ControllerType::ProS2; LEGACY_PORTS]);
        assert_eq!(hori.live_profiles(), [ControllerType::Hori; LEGACY_PORTS]);
    }

    #[test]
    fn hori_never_requests_identity_reenumeration() {
        let mut state = IdentityState::for_family(UsbControllerFamily::Hori);
        state.live[0] = ControllerType::JoyconL;
        state.change_us = 1;
        assert!(!state.due(u64::MAX));
    }
}
