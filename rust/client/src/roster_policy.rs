//! Safe Rust port of the C++ client-side roster construction policy.
//!
//! The desktop runtime periodically advertises the four local input names to the
//! server. This module keeps that externally visible slot/name/gyro policy pure
//! and deterministic so the eventual SDL backend only has to provide snapshots.

use crate::input_settings::{KEYBOARD_MODE_OFF, KEYBOARD_MODE_OVERRIDE, KEYBOARD_MODE_SINGLE};

pub const ROSTER_NAME_CAP: usize = 48;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RosterSource {
    pub connected: bool,
    pub has_motion: bool,
    pub name: String,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LocalRosterEntry {
    pub present: bool,
    pub has_gyro: bool,
    pub name: [u8; ROSTER_NAME_CAP],
}

impl LocalRosterEntry {
    fn set(&mut self, has_gyro: bool, name: &str) {
        self.present = true;
        self.has_gyro = has_gyro;
        let bytes = name.as_bytes();
        let len = bytes.len().min(ROSTER_NAME_CAP - 1);
        self.name[..len].copy_from_slice(&bytes[..len]);
        self.name[len] = 0;
    }

    #[must_use]
    pub fn name_bytes(&self) -> &[u8] {
        let end = self
            .name
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(ROSTER_NAME_CAP);
        &self.name[..end]
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RosterPolicy {
    pub switch2_mode: bool,
    pub hori_mode: bool,
    pub joycon_pair_mode: bool,
    pub keyboard_mode: i32,
}

/// Mirrors C++ `build_local_roster_entries` exactly for slot presence, names and gyro flags.
#[must_use]
pub fn build_local_roster_entries(
    sources: &[RosterSource; 4],
    policy: RosterPolicy,
) -> [LocalRosterEntry; 4] {
    let mut out = [LocalRosterEntry::default(); 4];

    if policy.switch2_mode {
        if policy.keyboard_mode == KEYBOARD_MODE_SINGLE {
            out[0].set(false, "Keyboard");
            return out;
        }

        let source = sources.iter().position(|pad| pad.connected);
        if policy.keyboard_mode == KEYBOARD_MODE_OVERRIDE {
            if let Some(index) = source {
                let base = if sources[index].name.is_empty() {
                    "Controller"
                } else {
                    &sources[index].name
                };
                out[0].set(sources[index].has_motion, &format!("{base} + Keyboard"));
            } else {
                out[0].set(false, "Keyboard");
            }
        } else if let Some(index) = source {
            let name = if sources[index].name.is_empty() {
                "Controller"
            } else {
                &sources[index].name
            };
            out[0].set(sources[index].has_motion, name);
        }
        return out;
    }

    let source_slots = if !policy.hori_mode && policy.joycon_pair_mode {
        2
    } else {
        4
    };

    let shifted_p1_target = if policy.keyboard_mode == KEYBOARD_MODE_SINGLE && sources[0].connected {
        (1..source_slots).find(|index| !sources[*index].connected)
    } else {
        None
    };

    for index in 0..source_slots {
        if index == 0 && policy.keyboard_mode != KEYBOARD_MODE_OFF {
            if policy.keyboard_mode == KEYBOARD_MODE_SINGLE {
                out[0].set(false, "Keyboard");
            } else if sources[0].connected {
                let base = if sources[0].name.is_empty() {
                    "Controller"
                } else {
                    &sources[0].name
                };
                out[0].set(sources[0].has_motion, &format!("{base} + Keyboard"));
            } else {
                out[0].set(false, "Keyboard");
            }
        } else if Some(index) == shifted_p1_target {
            let name = if sources[0].name.is_empty() {
                "Controller"
            } else {
                &sources[0].name
            };
            out[index].set(sources[0].has_motion, name);
        } else if sources[index].connected {
            let name = if sources[index].name.is_empty() {
                "Controller"
            } else {
                &sources[index].name
            };
            out[index].set(sources[index].has_motion, name);
        }
    }

    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn source(name: &str, has_motion: bool) -> RosterSource {
        RosterSource {
            connected: true,
            has_motion,
            name: name.to_string(),
        }
    }

    fn entry_name(entry: &LocalRosterEntry) -> &str {
        std::str::from_utf8(entry.name_bytes()).expect("test names are UTF-8")
    }

    #[test]
    fn switch2_single_keyboard_owns_p1_and_ignores_physical_controllers() {
        let sources = [
            source("Pad A", true),
            source("Pad B", true),
            RosterSource::default(),
            RosterSource::default(),
        ];
        let out = build_local_roster_entries(
            &sources,
            RosterPolicy {
                switch2_mode: true,
                keyboard_mode: KEYBOARD_MODE_SINGLE,
                ..RosterPolicy::default()
            },
        );
        assert!(out[0].present);
        assert!(!out[0].has_gyro);
        assert_eq!(entry_name(&out[0]), "Keyboard");
        assert!(out[1..].iter().all(|entry| !entry.present));
    }

    #[test]
    fn switch2_uses_only_first_connected_controller() {
        let sources = [
            RosterSource::default(),
            source("First", true),
            source("Ignored", false),
            RosterSource::default(),
        ];
        let out = build_local_roster_entries(
            &sources,
            RosterPolicy {
                switch2_mode: true,
                ..RosterPolicy::default()
            },
        );
        assert_eq!(entry_name(&out[0]), "First");
        assert!(out[0].has_gyro);
        assert!(out[1..].iter().all(|entry| !entry.present));
    }

    #[test]
    fn switch2_override_appends_keyboard_and_preserves_gyro() {
        let sources = [
            source("DualSense", true),
            RosterSource::default(),
            RosterSource::default(),
            RosterSource::default(),
        ];
        let out = build_local_roster_entries(
            &sources,
            RosterPolicy {
                switch2_mode: true,
                keyboard_mode: KEYBOARD_MODE_OVERRIDE,
                ..RosterPolicy::default()
            },
        );
        assert_eq!(entry_name(&out[0]), "DualSense + Keyboard");
        assert!(out[0].has_gyro);
    }

    #[test]
    fn legacy_single_keyboard_shifts_physical_p1_to_first_free_source_slot() {
        let sources = [
            source("P1", true),
            source("P2", false),
            RosterSource::default(),
            source("P4", false),
        ];
        let out = build_local_roster_entries(
            &sources,
            RosterPolicy {
                keyboard_mode: KEYBOARD_MODE_SINGLE,
                ..RosterPolicy::default()
            },
        );
        assert_eq!(entry_name(&out[0]), "Keyboard");
        assert_eq!(entry_name(&out[1]), "P2");
        assert_eq!(entry_name(&out[2]), "P1");
        assert!(out[2].has_gyro);
        assert_eq!(entry_name(&out[3]), "P4");
    }

    #[test]
    fn joycon_pair_limits_legacy_roster_to_two_source_slots() {
        let sources = [
            source("L", true),
            source("R", true),
            source("Third", true),
            source("Fourth", true),
        ];
        let out = build_local_roster_entries(
            &sources,
            RosterPolicy {
                joycon_pair_mode: true,
                ..RosterPolicy::default()
            },
        );
        assert_eq!(entry_name(&out[0]), "L");
        assert_eq!(entry_name(&out[1]), "R");
        assert!(!out[2].present);
        assert!(!out[3].present);
    }

    #[test]
    fn hori_mode_disables_joycon_pair_two_slot_limit() {
        let sources = [
            source("One", false),
            source("Two", false),
            source("Three", false),
            source("Four", false),
        ];
        let out = build_local_roster_entries(
            &sources,
            RosterPolicy {
                hori_mode: true,
                joycon_pair_mode: true,
                ..RosterPolicy::default()
            },
        );
        assert!(out.iter().all(|entry| entry.present));
    }

    #[test]
    fn empty_controller_names_use_cpp_fallback() {
        let sources = [
            source("", false),
            RosterSource::default(),
            RosterSource::default(),
            RosterSource::default(),
        ];
        let out = build_local_roster_entries(&sources, RosterPolicy::default());
        assert_eq!(entry_name(&out[0]), "Controller");
    }

    #[test]
    fn roster_name_is_truncated_to_47_bytes_and_null_terminated() {
        let sources = [
            source(&"x".repeat(80), false),
            RosterSource::default(),
            RosterSource::default(),
            RosterSource::default(),
        ];
        let out = build_local_roster_entries(&sources, RosterPolicy::default());
        assert_eq!(out[0].name_bytes().len(), ROSTER_NAME_CAP - 1);
        assert_eq!(out[0].name[ROSTER_NAME_CAP - 1], 0);
    }
}