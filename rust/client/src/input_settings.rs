use std::collections::BTreeMap;

pub const S2_AUDIO_DEVICE_DEFAULT: &str = "@default";

pub const KEYBOARD_MODE_OFF: i32 = 0;
pub const KEYBOARD_MODE_SINGLE: i32 = 1;
pub const KEYBOARD_MODE_OVERRIDE: i32 = 2;

const BINDING_KEYS: [(&str, &str); 28] = [
    ("A", "V"), ("B", "X"), ("X", "C"), ("Y", "Z"),
    ("L", "Q"), ("R", "E"), ("ZL", "1"), ("ZR", "2"),
    ("MINUS", "3"), ("PLUS", "4"), ("LSTICK", "LSHIFT"),
    ("RSTICK", "RSHIFT"), ("HOME", "HOME"), ("CAPTURE", "SNAPSHOT"),
    ("SL", "F4"), ("SR", "F5"), ("LSTICK_UP", "W"),
    ("LSTICK_DOWN", "S"), ("LSTICK_LEFT", "A"), ("LSTICK_RIGHT", "D"),
    ("RSTICK_UP", "I"), ("RSTICK_DOWN", "K"), ("RSTICK_LEFT", "J"),
    ("RSTICK_RIGHT", "L"), ("DPAD_UP", "UP"), ("DPAD_DOWN", "DOWN"),
    ("DPAD_LEFT", "LEFT"), ("DPAD_RIGHT", "RIGHT"),
];

const S2_BINDING_KEYS: [(&str, &str); 3] = [("C", "F1"), ("GL", "F2"), ("GR", "F3")];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AxisSettings {
    deadzone: i16,
    invert: bool,
}

impl Default for AxisSettings {
    fn default() -> Self {
        Self { deadzone: 3_000, invert: false }
    }
}

impl AxisSettings {
    pub fn new(deadzone: i16, invert: bool) -> Result<Self, String> {
        if !(0..=32_000).contains(&deadzone) {
            return Err("deadzone must be in 0..=32000".to_string());
        }
        Ok(Self { deadzone, invert })
    }

    #[must_use]
    pub fn apply(&self, value: i16) -> u8 {
        let mut value = i32::from(value);
        if value.abs() <= i32::from(self.deadzone) {
            value = 0;
        }
        if self.invert {
            value = -value;
        }
        (((value + 32_768) * 255) / 65_535).clamp(0, 255) as u8
    }

    #[must_use]
    pub const fn deadzone(&self) -> i16 { self.deadzone }

    #[must_use]
    pub const fn inverted(&self) -> bool { self.invert }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct InputSettings {
    axes: [AxisSettings; 4],
    background_input: bool,
}

impl Default for InputSettings {
    fn default() -> Self {
        Self { axes: [AxisSettings::default(); 4], background_input: true }
    }
}

impl InputSettings {
    #[must_use]
    pub fn axis(&self, index: usize) -> Option<AxisSettings> { self.axes.get(index).copied() }

    pub fn set_axis(&mut self, index: usize, settings: AxisSettings) -> Result<(), String> {
        let slot = self.axes.get_mut(index).ok_or_else(|| "axis index out of range".to_string())?;
        *slot = settings;
        Ok(())
    }

    #[must_use]
    pub const fn background_input(&self) -> bool { self.background_input }

    pub fn set_background_input(&mut self, enabled: bool) { self.background_input = enabled; }
}

#[must_use]
pub fn binding_keys() -> &'static [(&'static str, &'static str)] { &BINDING_KEYS }

#[must_use]
pub fn s2_binding_keys() -> &'static [(&'static str, &'static str)] { &S2_BINDING_KEYS }

#[must_use]
pub fn default_key_bindings() -> BTreeMap<String, String> {
    BINDING_KEYS.iter().chain(S2_BINDING_KEYS.iter())
        .map(|(name, key)| ((*name).to_string(), (*key).to_string()))
        .collect()
}

#[must_use]
pub fn controller_binding_keys() -> Vec<(&'static str, &'static str)> {
    BINDING_KEYS.iter().chain(S2_BINDING_KEYS.iter())
        .map(|(name, _)| (*name, *name))
        .collect()
}

#[must_use]
pub fn default_controller_bindings() -> BTreeMap<String, String> {
    controller_binding_keys().into_iter()
        .map(|(target, source)| (target.to_string(), source.to_string()))
        .collect()
}

#[must_use]
pub fn normalize_key_name(raw: &str) -> String {
    let value = raw.trim().to_ascii_uppercase();
    let bytes = value.as_bytes();
    if bytes.len() == 4 && value.starts_with("KEY") && bytes[3].is_ascii_uppercase() {
        return value[3..].to_string();
    }
    if bytes.len() == 6 && value.starts_with("DIGIT") && bytes[5].is_ascii_digit() {
        return value[5..].to_string();
    }
    match value.as_str() {
        "ESCAPE" => "ESC".to_string(),
        "ARROWUP" => "UP".to_string(),
        "ARROWDOWN" => "DOWN".to_string(),
        "ARROWLEFT" => "LEFT".to_string(),
        "ARROWRIGHT" => "RIGHT".to_string(),
        "SHIFTLEFT" => "LSHIFT".to_string(),
        "SHIFTRIGHT" => "RSHIFT".to_string(),
        "CONTROLLEFT" => "LCTRL".to_string(),
        "CONTROLRIGHT" => "RCTRL".to_string(),
        "ALTLEFT" => "LALT".to_string(),
        "ALTRIGHT" => "RALT".to_string(),
        "METALEFT" => "LMETA".to_string(),
        "METARIGHT" => "RMETA".to_string(),
        "PRINTSCREEN" => "SNAPSHOT".to_string(),
        _ => value,
    }
}

#[must_use]
pub fn normalize_macro_hotkey_for_io(raw: &str) -> String { normalize_key_name(raw) }

#[must_use]
pub fn is_mouse_button_name(raw: &str) -> bool {
    let value = normalize_key_name(raw);
    let bytes = value.as_bytes();
    bytes.len() == 6 && value.starts_with("MOUSE") && (b'1'..=b'5').contains(&bytes[5])
}

#[must_use]
pub fn is_valid_key_code(raw: &str) -> bool {
    let value = normalize_key_name(raw);
    if value.is_empty() || is_mouse_button_name(&value) {
        return true;
    }
    const NAMED: [&str; 39] = [
        "ESC", "ESCAPE", "SPACE", "ENTER", "TAB", "BACKSPACE", "DELETE", "INSERT",
        "HOME", "END", "PAGEUP", "PAGEDOWN", "CAPSLOCK", "NUMLOCK", "SCROLLLOCK",
        "PAUSE", "SNAPSHOT", "PRINTSCREEN", "CONTEXTMENU", "UP", "DOWN", "LEFT", "RIGHT",
        "LSHIFT", "RSHIFT", "LCTRL", "RCTRL", "LALT", "RALT", "LMETA", "RMETA",
        "SHIFTLEFT", "SHIFTRIGHT", "METALEFT", "METARIGHT", "CONTROLLEFT", "CONTROLRIGHT",
        "ALTLEFT", "ALTRIGHT",
    ];
    if NAMED.contains(&value.as_str()) {
        return true;
    }
    if value.len() == 1 && value.as_bytes()[0].is_ascii_alphanumeric() {
        return true;
    }
    if let Some(rest) = value.strip_prefix('F')
        && (1..=2).contains(&rest.len())
        && rest.bytes().all(|byte| byte.is_ascii_digit())
        && let Ok(number) = rest.parse::<u8>()
    {
        return (1..=24).contains(&number);
    }
    false
}

#[must_use]
pub fn sanitize_keyboard_mode(mode: i32) -> i32 {
    if (KEYBOARD_MODE_OFF..=KEYBOARD_MODE_OVERRIDE).contains(&mode) {
        mode
    } else {
        KEYBOARD_MODE_OFF
    }
}

#[must_use]
pub fn normalize_audio_device_selection(raw: &str) -> String {
    if raw.is_empty() { S2_AUDIO_DEVICE_DEFAULT.to_string() } else { raw.to_string() }
}

#[must_use]
pub fn sanitize_controller_binding(source: &str) -> Option<String> {
    if source.is_empty() {
        return Some(String::new());
    }
    controller_binding_keys().iter()
        .any(|(name, _)| *name == source)
        .then(|| source.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpp_default_bindings_are_preserved() {
        let keys = default_key_bindings();
        assert_eq!(keys.len(), 31);
        assert_eq!(keys.get("A").map(String::as_str), Some("V"));
        assert_eq!(keys.get("CAPTURE").map(String::as_str), Some("SNAPSHOT"));
        assert_eq!(keys.get("C").map(String::as_str), Some("F1"));
        assert_eq!(keys.get("GR").map(String::as_str), Some("F3"));

        let controllers = default_controller_bindings();
        assert_eq!(controllers.len(), 31);
        assert_eq!(controllers.get("ZR").map(String::as_str), Some("ZR"));
        assert_eq!(controllers.get("GL").map(String::as_str), Some("GL"));
    }

    #[test]
    fn key_normalization_matches_cpp_aliases() {
        assert_eq!(normalize_key_name(" KeyA "), "A");
        assert_eq!(normalize_key_name("Digit7"), "7");
        assert_eq!(normalize_key_name("Escape"), "ESC");
        assert_eq!(normalize_key_name("ArrowLeft"), "LEFT");
        assert_eq!(normalize_key_name("ControlRight"), "RCTRL");
        assert_eq!(normalize_key_name("PrintScreen"), "SNAPSHOT");
        assert_eq!(normalize_key_name("mouse3"), "MOUSE3");
    }

    #[test]
    fn key_validation_matches_cpp_boundaries() {
        for value in ["", "A", "7", "F1", "F24", "SPACE", "MOUSE1", "MOUSE5", "ArrowUp"] {
            assert!(is_valid_key_code(value), "expected valid key: {value}");
        }
        for value in ["F0", "F25", "F100", "MOUSE0", "MOUSE6", "BUTTON1", "?"] {
            assert!(!is_valid_key_code(value), "expected invalid key: {value}");
        }
    }

    #[test]
    fn persisted_setting_sanitizers_match_cpp_fallbacks() {
        assert_eq!(sanitize_keyboard_mode(KEYBOARD_MODE_OFF), KEYBOARD_MODE_OFF);
        assert_eq!(sanitize_keyboard_mode(KEYBOARD_MODE_SINGLE), KEYBOARD_MODE_SINGLE);
        assert_eq!(sanitize_keyboard_mode(KEYBOARD_MODE_OVERRIDE), KEYBOARD_MODE_OVERRIDE);
        assert_eq!(sanitize_keyboard_mode(-1), KEYBOARD_MODE_OFF);
        assert_eq!(sanitize_keyboard_mode(3), KEYBOARD_MODE_OFF);
        assert_eq!(normalize_audio_device_selection(""), S2_AUDIO_DEVICE_DEFAULT);
        assert_eq!(normalize_audio_device_selection("USB DAC"), "USB DAC");
        assert_eq!(sanitize_controller_binding("GL").as_deref(), Some("GL"));
        assert_eq!(sanitize_controller_binding("").as_deref(), Some(""));
        assert_eq!(sanitize_controller_binding("UNKNOWN"), None);
    }
}
