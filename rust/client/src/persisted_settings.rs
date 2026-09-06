use std::collections::BTreeMap;

use crate::input_settings::{
    default_controller_bindings, default_key_bindings, normalize_audio_device_selection,
    normalize_key_name, sanitize_controller_binding, sanitize_keyboard_mode,
    S2_AUDIO_DEVICE_DEFAULT,
};

pub const DEFAULT_LAST_IP: &str = "192.168.1.100";
pub const CONTROLLER_TYPE_JOYCON_L: i32 = 1;
pub const CONTROLLER_TYPE_JOYCON_R: i32 = 2;
pub const CONTROLLER_TYPE_PRO: i32 = 3;
pub const CONTROLLER_TYPE_JOYCON_PAIR: i32 = 4;

#[derive(Clone, Debug, PartialEq)]
pub struct PersistedFeatureSettings {
    pub gyro_enabled: bool,
    pub rumble_enabled: bool,
    pub switch2_audio_enabled: bool,
    pub switch2_microphone_enabled: bool,
    pub switch2_playback_device: String,
    pub switch2_microphone_device: String,
    pub home_shortcut_enabled: bool,
    pub capture_shortcut_enabled: bool,
    pub mouse_mode_enabled: bool,
    pub mouse_sensitivity: f64,
    pub controller_type: i32,
    pub joycon_horizontal_mode: bool,
}

impl Default for PersistedFeatureSettings {
    fn default() -> Self {
        Self {
            gyro_enabled: true,
            rumble_enabled: true,
            switch2_audio_enabled: false,
            switch2_microphone_enabled: false,
            switch2_playback_device: S2_AUDIO_DEVICE_DEFAULT.to_string(),
            switch2_microphone_device: S2_AUDIO_DEVICE_DEFAULT.to_string(),
            home_shortcut_enabled: true,
            capture_shortcut_enabled: true,
            mouse_mode_enabled: false,
            mouse_sensitivity: 1.0,
            controller_type: CONTROLLER_TYPE_PRO,
            joycon_horizontal_mode: false,
        }
    }
}

impl PersistedFeatureSettings {
    #[must_use]
    pub fn sanitize(mut self) -> Self {
        self.switch2_playback_device =
            normalize_audio_device_selection(&self.switch2_playback_device);
        self.switch2_microphone_device =
            normalize_audio_device_selection(&self.switch2_microphone_device);
        self.mouse_sensitivity = sanitize_mouse_sensitivity(self.mouse_sensitivity);
        self.controller_type = sanitize_controller_type(self.controller_type);
        self.joycon_horizontal_mode = sanitize_joycon_horizontal_mode(
            self.controller_type,
            self.joycon_horizontal_mode,
        );
        self
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct PersistedClientSettings {
    pub last_ip: String,
    pub keyboard_mode: i32,
    pub key_bindings: BTreeMap<String, String>,
    pub controller_bindings: BTreeMap<String, String>,
    pub features: PersistedFeatureSettings,
}

impl Default for PersistedClientSettings {
    fn default() -> Self {
        Self {
            last_ip: DEFAULT_LAST_IP.to_string(),
            keyboard_mode: 0,
            key_bindings: default_key_bindings(),
            controller_bindings: default_controller_bindings(),
            features: PersistedFeatureSettings::default(),
        }
    }
}

impl PersistedClientSettings {
    #[must_use]
    pub fn from_saved_values(
        last_ip: Option<&str>,
        keyboard_mode: i32,
        saved_key_bindings: &BTreeMap<String, String>,
        saved_controller_bindings: &BTreeMap<String, String>,
        features: PersistedFeatureSettings,
    ) -> Self {
        Self {
            last_ip: last_ip.unwrap_or(DEFAULT_LAST_IP).to_string(),
            keyboard_mode: sanitize_keyboard_mode(keyboard_mode),
            key_bindings: merge_saved_key_bindings(saved_key_bindings),
            controller_bindings: merge_saved_controller_bindings(saved_controller_bindings),
            features: features.sanitize(),
        }
    }
}

#[must_use]
pub fn merge_saved_key_bindings(
    saved: &BTreeMap<String, String>,
) -> BTreeMap<String, String> {
    let mut bindings = default_key_bindings();
    for (target, source) in saved {
        if let Some(slot) = bindings.get_mut(target) {
            *slot = normalize_key_name(source);
        }
    }
    bindings
}

#[must_use]
pub fn merge_saved_controller_bindings(
    saved: &BTreeMap<String, String>,
) -> BTreeMap<String, String> {
    let mut bindings = default_controller_bindings();
    for (target, source) in saved {
        let Some(slot) = bindings.get_mut(target) else {
            continue;
        };
        if let Some(source) = sanitize_controller_binding(source) {
            *slot = source;
        }
    }
    bindings
}

#[must_use]
pub fn sanitize_mouse_sensitivity(value: f64) -> f64 {
    if value.is_finite() {
        value.clamp(0.0, 5.0)
    } else {
        1.0
    }
}

#[must_use]
pub const fn sanitize_controller_type(value: i32) -> i32 {
    match value {
        CONTROLLER_TYPE_JOYCON_L
        | CONTROLLER_TYPE_JOYCON_R
        | CONTROLLER_TYPE_PRO
        | CONTROLLER_TYPE_JOYCON_PAIR => value,
        _ => CONTROLLER_TYPE_PRO,
    }
}

#[must_use]
pub const fn sanitize_joycon_horizontal_mode(controller_type: i32, enabled: bool) -> bool {
    enabled
        && (controller_type == CONTROLLER_TYPE_JOYCON_L
            || controller_type == CONTROLLER_TYPE_JOYCON_R)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpp_feature_defaults_are_preserved() {
        let settings = PersistedFeatureSettings::default();
        assert!(settings.gyro_enabled);
        assert!(settings.rumble_enabled);
        assert!(!settings.switch2_audio_enabled);
        assert!(!settings.switch2_microphone_enabled);
        assert_eq!(settings.switch2_playback_device, S2_AUDIO_DEVICE_DEFAULT);
        assert_eq!(settings.switch2_microphone_device, S2_AUDIO_DEVICE_DEFAULT);
        assert!(settings.home_shortcut_enabled);
        assert!(settings.capture_shortcut_enabled);
        assert!(!settings.mouse_mode_enabled);
        assert_eq!(settings.mouse_sensitivity, 1.0);
        assert_eq!(settings.controller_type, CONTROLLER_TYPE_PRO);
        assert!(!settings.joycon_horizontal_mode);
    }

    #[test]
    fn saved_key_bindings_override_only_known_targets() {
        let mut saved = BTreeMap::new();
        saved.insert("A".to_string(), " KeyB ".to_string());
        saved.insert("CAPTURE".to_string(), "PrintScreen".to_string());
        saved.insert("UNKNOWN".to_string(), "F9".to_string());

        let merged = merge_saved_key_bindings(&saved);
        assert_eq!(merged.len(), 31);
        assert_eq!(merged.get("A").map(String::as_str), Some("B"));
        assert_eq!(merged.get("CAPTURE").map(String::as_str), Some("SNAPSHOT"));
        assert!(!merged.contains_key("UNKNOWN"));
        assert_eq!(merged.get("B").map(String::as_str), Some("X"));
    }

    #[test]
    fn invalid_saved_controller_sources_leave_defaults_unchanged() {
        let mut saved = BTreeMap::new();
        saved.insert("A".to_string(), "ZR".to_string());
        saved.insert("B".to_string(), "UNKNOWN".to_string());
        saved.insert("X".to_string(), String::new());
        saved.insert("UNKNOWN".to_string(), "A".to_string());

        let merged = merge_saved_controller_bindings(&saved);
        assert_eq!(merged.len(), 31);
        assert_eq!(merged.get("A").map(String::as_str), Some("ZR"));
        assert_eq!(merged.get("B").map(String::as_str), Some("B"));
        assert_eq!(merged.get("X").map(String::as_str), Some(""));
        assert!(!merged.contains_key("UNKNOWN"));
    }

    #[test]
    fn feature_sanitizers_match_cpp_load_boundaries() {
        assert_eq!(sanitize_mouse_sensitivity(-0.1), 0.0);
        assert_eq!(sanitize_mouse_sensitivity(2.5), 2.5);
        assert_eq!(sanitize_mouse_sensitivity(5.1), 5.0);
        assert_eq!(sanitize_mouse_sensitivity(f64::NAN), 1.0);
        assert_eq!(sanitize_mouse_sensitivity(f64::INFINITY), 1.0);

        for controller_type in [
            CONTROLLER_TYPE_JOYCON_L,
            CONTROLLER_TYPE_JOYCON_R,
            CONTROLLER_TYPE_PRO,
            CONTROLLER_TYPE_JOYCON_PAIR,
        ] {
            assert_eq!(sanitize_controller_type(controller_type), controller_type);
        }
        assert_eq!(sanitize_controller_type(0), CONTROLLER_TYPE_PRO);
        assert_eq!(sanitize_controller_type(5), CONTROLLER_TYPE_PRO);

        assert!(sanitize_joycon_horizontal_mode(CONTROLLER_TYPE_JOYCON_L, true));
        assert!(sanitize_joycon_horizontal_mode(CONTROLLER_TYPE_JOYCON_R, true));
        assert!(!sanitize_joycon_horizontal_mode(CONTROLLER_TYPE_PRO, true));
        assert!(!sanitize_joycon_horizontal_mode(CONTROLLER_TYPE_JOYCON_PAIR, true));
    }

    #[test]
    fn saved_snapshot_matches_cpp_fallback_behavior() {
        let mut keys = BTreeMap::new();
        keys.insert("A".to_string(), "ArrowLeft".to_string());
        let mut controllers = BTreeMap::new();
        controllers.insert("A".to_string(), "NOPE".to_string());

        let snapshot = PersistedClientSettings::from_saved_values(
            None,
            99,
            &keys,
            &controllers,
            PersistedFeatureSettings {
                switch2_playback_device: String::new(),
                switch2_microphone_device: String::new(),
                mouse_sensitivity: f64::NEG_INFINITY,
                controller_type: 42,
                joycon_horizontal_mode: true,
                ..PersistedFeatureSettings::default()
            },
        );

        assert_eq!(snapshot.last_ip, DEFAULT_LAST_IP);
        assert_eq!(snapshot.keyboard_mode, 0);
        assert_eq!(snapshot.key_bindings.get("A").map(String::as_str), Some("LEFT"));
        assert_eq!(snapshot.controller_bindings.get("A").map(String::as_str), Some("A"));
        assert_eq!(snapshot.features.switch2_playback_device, S2_AUDIO_DEVICE_DEFAULT);
        assert_eq!(snapshot.features.switch2_microphone_device, S2_AUDIO_DEVICE_DEFAULT);
        assert_eq!(snapshot.features.mouse_sensitivity, 1.0);
        assert_eq!(snapshot.features.controller_type, CONTROLLER_TYPE_PRO);
        assert!(!snapshot.features.joycon_horizontal_mode);
    }
}
