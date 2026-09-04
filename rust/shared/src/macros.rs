//! Safe Rust port of `shared/src/macros.cpp`.

use crate::protocol::{
    BTN_A, BTN_B, BTN_CAPTURE, BTN_HOME, BTN_L, BTN_LSTICK, BTN_MINUS, BTN_PLUS, BTN_R,
    BTN_RSTICK, BTN_X, BTN_Y, BTN_ZL, BTN_ZR, EXT_BUTTON_C, EXT_BUTTON_GL, EXT_BUTTON_GR,
    EXT_BUTTON_MASK, EXT_BUTTON_SL, EXT_BUTTON_SR, Hat, HoriHidReport,
};
use core::fmt;

pub const JSON_MAX_BYTES: usize = 50 * 1024 * 1024;
pub const MAX_EXPANDED_STEPS: usize = 1_000_000;
pub const UDP_MAGIC: u32 = 0x4e53_4d43;
pub const UDP_CHUNK_MAGIC: u32 = 0x4e53_4d4b;
pub const UDP_TEXT_MAX: usize = JSON_MAX_BYTES;
pub const UDP_CHUNK_MAX: usize = 1200;
pub const UDP_CHUNK_COUNT_MAX: u32 =
    ((UDP_TEXT_MAX + UDP_CHUNK_MAX - 1) / UDP_CHUNK_MAX) as u32;
pub const CHUNK_FLAG_LAST: u8 = 0x01;
pub const UDP_HEADER_SIZE: usize = 14;
pub const CHUNK_HEADER_SIZE: usize = 30;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum UploadKind {
    #[default]
    Macro = 0,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MacroError(String);

impl MacroError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for MacroError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for MacroError {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Step {
    buttons: u16,
    extra_buttons: u8,
    hat: Hat,
    axes: [u8; 4],
    has_lstick: bool,
    has_rstick: bool,
    duration_ms: u32,
}

impl Default for Step {
    fn default() -> Self {
        Self {
            buttons: 0,
            extra_buttons: 0,
            hat: Hat::Neutral,
            axes: [128; 4],
            has_lstick: false,
            has_rstick: false,
            duration_ms: 0,
        }
    }
}

impl Step {
    #[must_use]
    pub const fn buttons(&self) -> u16 {
        self.buttons
    }

    #[must_use]
    pub const fn extra_buttons(&self) -> u8 {
        self.extra_buttons
    }

    #[must_use]
    pub const fn hat(&self) -> Hat {
        self.hat
    }

    #[must_use]
    pub const fn axes(&self) -> [u8; 4] {
        self.axes
    }

    #[must_use]
    pub const fn has_lstick(&self) -> bool {
        self.has_lstick
    }

    #[must_use]
    pub const fn has_rstick(&self) -> bool {
        self.has_rstick
    }

    #[must_use]
    pub const fn duration_ms(&self) -> u32 {
        self.duration_ms
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Entry {
    name: String,
    hotkey: String,
    json: String,
}

impl Entry {
    #[must_use]
    pub fn new(name: impl Into<String>, hotkey: impl Into<String>, json: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            hotkey: hotkey.into(),
            json: json.into(),
        }
    }

    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub fn hotkey(&self) -> &str {
        &self.hotkey
    }

    #[must_use]
    pub fn json(&self) -> &str {
        &self.json
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MacroUdpHeader {
    version: u8,
    subpad: u8,
    text_len: u32,
    seq: u32,
}

impl MacroUdpHeader {
    #[must_use]
    pub const fn new(version: u8, subpad: u8, text_len: u32, seq: u32) -> Self {
        Self {
            version,
            subpad,
            text_len,
            seq,
        }
    }

    #[must_use]
    pub fn encode(&self) -> [u8; UDP_HEADER_SIZE] {
        let mut out = [0u8; UDP_HEADER_SIZE];
        out[..4].copy_from_slice(&UDP_MAGIC.to_le_bytes());
        out[4] = self.version;
        out[5] = self.subpad;
        out[6..10].copy_from_slice(&self.text_len.to_le_bytes());
        out[10..14].copy_from_slice(&self.seq.to_le_bytes());
        out
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MacroUdpChunkHeader {
    version: u8,
    subpad: u8,
    flags: u8,
    upload_id: u32,
    chunk_index: u32,
    chunk_count: u32,
    total_len: u32,
    chunk_len: u16,
    seq: u32,
}

impl MacroUdpChunkHeader {
    #[must_use]
    pub const fn new(
        version: u8,
        subpad: u8,
        flags: u8,
        upload_id: u32,
        chunk: [u32; 2],
        total_len: u32,
        chunk_len: u16,
        seq: u32,
    ) -> Self {
        Self {
            version,
            subpad,
            flags,
            upload_id,
            chunk_index: chunk[0],
            chunk_count: chunk[1],
            total_len,
            chunk_len,
            seq,
        }
    }

    #[must_use]
    pub fn encode(&self) -> [u8; CHUNK_HEADER_SIZE] {
        let mut out = [0u8; CHUNK_HEADER_SIZE];
        out[..4].copy_from_slice(&UDP_CHUNK_MAGIC.to_le_bytes());
        out[4] = self.version;
        out[5] = self.subpad;
        out[6] = self.flags;
        out[8..12].copy_from_slice(&self.upload_id.to_le_bytes());
        out[12..16].copy_from_slice(&self.chunk_index.to_le_bytes());
        out[16..20].copy_from_slice(&self.chunk_count.to_le_bytes());
        out[20..24].copy_from_slice(&self.total_len.to_le_bytes());
        out[24..26].copy_from_slice(&self.chunk_len.to_le_bytes());
        out[26..30].copy_from_slice(&self.seq.to_le_bytes());
        out
    }
}

#[must_use]
pub const fn udp_auth_size(text_len: usize) -> usize {
    UDP_HEADER_SIZE.saturating_add(text_len)
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum JsonValue {
    String(String),
    Array(Vec<JsonValue>),
    Object(Vec<(String, JsonValue)>),
    Other,
}

impl JsonValue {
    fn object_get(&self, key: &str) -> Option<&Self> {
        let Self::Object(entries) = self else {
            return None;
        };
        entries
            .iter()
            .rev()
            .find_map(|(candidate, value)| (candidate == key).then_some(value))
    }
}

struct JsonParser<'a> {
    source: &'a str,
    position: usize,
}

impl<'a> JsonParser<'a> {
    fn new(source: &'a str) -> Self {
        Self {
            source,
            position: 0,
        }
    }

    fn parse(mut self) -> Result<JsonValue, MacroError> {
        self.skip_space();
        let value = self.parse_value()?;
        self.skip_space();
        if self.position != self.source.len() {
            return Err(MacroError::new("JSON parse error: trailing characters"));
        }
        Ok(value)
    }

    fn remaining(&self) -> &str {
        &self.source[self.position..]
    }

    fn peek(&self) -> Option<char> {
        self.remaining().chars().next()
    }

    fn bump(&mut self) -> Option<char> {
        let ch = self.peek()?;
        self.position += ch.len_utf8();
        Some(ch)
    }

    fn skip_space(&mut self) {
        while self.peek().is_some_and(char::is_whitespace) {
            self.bump();
        }
    }

    fn expect(&mut self, expected: char) -> Result<(), MacroError> {
        match self.bump() {
            Some(actual) if actual == expected => Ok(()),
            _ => Err(MacroError::new(format!(
                "JSON parse error: expected '{expected}'"
            ))),
        }
    }

    fn parse_value(&mut self) -> Result<JsonValue, MacroError> {
        self.skip_space();
        match self.peek() {
            Some('"') => self.parse_string().map(JsonValue::String),
            Some('[') => self.parse_array(),
            Some('{') => self.parse_object(),
            Some('t') => self.parse_literal("true"),
            Some('f') => self.parse_literal("false"),
            Some('n') => self.parse_literal("null"),
            Some('-' | '0'..='9') => self.parse_number(),
            Some(other) => Err(MacroError::new(format!(
                "JSON parse error: unexpected character '{other}'"
            ))),
            None => Err(MacroError::new("JSON parse error: unexpected end of input")),
        }
    }

    fn parse_literal(&mut self, literal: &str) -> Result<JsonValue, MacroError> {
        if self.remaining().starts_with(literal) {
            self.position += literal.len();
            Ok(JsonValue::Other)
        } else {
            Err(MacroError::new("JSON parse error: invalid literal"))
        }
    }

    fn parse_number(&mut self) -> Result<JsonValue, MacroError> {
        let start = self.position;
        while self
            .peek()
            .is_some_and(|ch| matches!(ch, '-' | '+' | '.' | 'e' | 'E' | '0'..='9'))
        {
            self.bump();
        }
        self.source[start..self.position]
            .parse::<f64>()
            .map_err(|_| MacroError::new("JSON parse error: invalid number"))?;
        Ok(JsonValue::Other)
    }

    fn parse_string(&mut self) -> Result<String, MacroError> {
        self.expect('"')?;
        let mut output = String::new();
        loop {
            let ch = self
                .bump()
                .ok_or_else(|| MacroError::new("JSON parse error: unterminated string"))?;
            match ch {
                '"' => return Ok(output),
                '\\' => {
                    let escaped = self.bump().ok_or_else(|| {
                        MacroError::new("JSON parse error: incomplete string escape")
                    })?;
                    match escaped {
                        '"' | '\\' | '/' => output.push(escaped),
                        'b' => output.push('\u{0008}'),
                        'f' => output.push('\u{000c}'),
                        'n' => output.push('\n'),
                        'r' => output.push('\r'),
                        't' => output.push('\t'),
                        'u' => self.parse_unicode_escape(&mut output)?,
                        _ => return Err(MacroError::new("JSON parse error: invalid string escape")),
                    }
                }
                value if value <= '\u{001f}' => {
                    return Err(MacroError::new("JSON parse error: unescaped control character"));
                }
                value => output.push(value),
            }
        }
    }

    fn parse_hex_quad(&mut self) -> Result<u16, MacroError> {
        let mut value = 0u16;
        for _ in 0..4 {
            let digit = self
                .bump()
                .and_then(|ch| ch.to_digit(16))
                .ok_or_else(|| MacroError::new("JSON parse error: invalid unicode escape"))?;
            value = (value << 4) | u16::try_from(digit).expect("hex digit fits in u16");
        }
        Ok(value)
    }

    fn parse_unicode_escape(&mut self, output: &mut String) -> Result<(), MacroError> {
        let first = self.parse_hex_quad()?;
        let scalar = if (0xd800..=0xdbff).contains(&first) {
            if !self.remaining().starts_with("\\u") {
                return Err(MacroError::new("JSON parse error: missing low surrogate"));
            }
            self.position += 2;
            let second = self.parse_hex_quad()?;
            if !(0xdc00..=0xdfff).contains(&second) {
                return Err(MacroError::new("JSON parse error: invalid low surrogate"));
            }
            0x1_0000
                + ((u32::from(first) - 0xd800) << 10)
                + (u32::from(second) - 0xdc00)
        } else if (0xdc00..=0xdfff).contains(&first) {
            return Err(MacroError::new("JSON parse error: unexpected low surrogate"));
        } else {
            u32::from(first)
        };
        output.push(
            char::from_u32(scalar)
                .ok_or_else(|| MacroError::new("JSON parse error: invalid unicode scalar"))?,
        );
        Ok(())
    }

    fn parse_array(&mut self) -> Result<JsonValue, MacroError> {
        self.expect('[')?;
        self.skip_space();
        let mut values = Vec::new();
        if self.peek() == Some(']') {
            self.bump();
            return Ok(JsonValue::Array(values));
        }
        loop {
            values.push(self.parse_value()?);
            self.skip_space();
            match self.bump() {
                Some(',') => self.skip_space(),
                Some(']') => return Ok(JsonValue::Array(values)),
                _ => return Err(MacroError::new("JSON parse error: expected ',' or ']'")),
            }
        }
    }

    fn parse_object(&mut self) -> Result<JsonValue, MacroError> {
        self.expect('{')?;
        self.skip_space();
        let mut entries = Vec::new();
        if self.peek() == Some('}') {
            self.bump();
            return Ok(JsonValue::Object(entries));
        }
        loop {
            let key = self.parse_string()?;
            self.skip_space();
            self.expect(':')?;
            let value = self.parse_value()?;
            entries.push((key, value));
            self.skip_space();
            match self.bump() {
                Some(',') => self.skip_space(),
                Some('}') => return Ok(JsonValue::Object(entries)),
                _ => return Err(MacroError::new("JSON parse error: expected ',' or '}'")),
            }
        }
    }
}

fn parse_json(source: &str) -> Result<JsonValue, MacroError> {
    JsonParser::new(source).parse()
}

fn json_escape(value: &str) -> String {
    let mut output = String::new();
    for ch in value.chars() {
        match ch {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            '\u{0008}' => output.push_str("\\b"),
            '\u{000c}' => output.push_str("\\f"),
            value if value <= '\u{001f}' => output.push_str(&format!("\\u{:04x}", value as u32)),
            value => output.push(value),
        }
    }
    output
}

fn json_compact(value: &JsonValue) -> String {
    match value {
        JsonValue::String(text) => format!("\"{}\"", json_escape(text)),
        JsonValue::Array(items) => format!(
            "[{}]",
            items.iter().map(json_compact).collect::<Vec<_>>().join(",")
        ),
        JsonValue::Object(entries) => format!(
            "{{{}}}",
            entries
                .iter()
                .map(|(key, value)| format!("\"{}\":{}", json_escape(key), json_compact(value)))
                .collect::<Vec<_>>()
                .join(",")
        ),
        JsonValue::Other => "null".to_owned(),
    }
}

fn trim(value: &str) -> &str {
    value.trim()
}

fn extract_commands_text(raw_input: &str) -> Result<String, MacroError> {
    if raw_input.len() > JSON_MAX_BYTES {
        return Err(MacroError::new("macro JSON exceeds 50MB limit"));
    }
    let raw = trim(raw_input);
    if raw.is_empty() {
        return Err(MacroError::new("empty macro"));
    }
    if !matches!(raw.as_bytes().first(), Some(b'{') | Some(b'[')) {
        return Ok(raw.to_owned());
    }

    match parse_json(raw)? {
        JsonValue::Array(items) => strings_to_commands(&items),
        value @ JsonValue::Object(_) => match value.object_get("commands") {
            Some(JsonValue::String(commands)) => Ok(commands.clone()),
            Some(JsonValue::Array(items)) => strings_to_commands(items),
            Some(_) => Err(MacroError::new("commands must be a string or an array of strings")),
            None => Err(MacroError::new("macro object is missing commands")),
        },
        _ => Err(MacroError::new("macro JSON must be an object or commands array")),
    }
}

fn strings_to_commands(items: &[JsonValue]) -> Result<String, MacroError> {
    if items.is_empty() {
        return Err(MacroError::new("commands array is empty"));
    }
    let mut commands = Vec::with_capacity(items.len());
    for item in items {
        let JsonValue::String(command) = item else {
            return Err(MacroError::new("commands array must contain only strings"));
        };
        commands.push(command.as_str());
    }
    Ok(commands.join(";"))
}

fn parse_u32_strict(value: &str) -> Option<u32> {
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    let mut result = 0u32;
    for byte in value.bytes() {
        result = result.checked_mul(10)?.checked_add(u32::from(byte - b'0'))?;
    }
    Some(result)
}

fn button_bit(token: &str) -> u16 {
    match token {
        "A" | "BTN_A" => BTN_A,
        "B" | "BTN_B" => BTN_B,
        "X" | "BTN_X" => BTN_X,
        "Y" | "BTN_Y" => BTN_Y,
        "L" | "BTN_L" => BTN_L,
        "R" | "BTN_R" => BTN_R,
        "ZL" | "BTN_ZL" => BTN_ZL,
        "ZR" | "BTN_ZR" => BTN_ZR,
        "MINUS" | "-" | "BTN_MINUS" => BTN_MINUS,
        "PLUS" | "+" | "BTN_PLUS" => BTN_PLUS,
        "LSTICK" | "LS" | "BTN_LSTICK" => BTN_LSTICK,
        "RSTICK" | "RS" | "BTN_RSTICK" => BTN_RSTICK,
        "HOME" | "GUIDE" | "BTN_HOME" => BTN_HOME,
        "CAPTURE" | "SHARE" | "BTN_CAPTURE" => BTN_CAPTURE,
        _ => 0,
    }
}

fn extra_button_bit(token: &str) -> u8 {
    match token {
        "C" | "BTN_C" => EXT_BUTTON_C,
        "GL" | "BTN_GL" => EXT_BUTTON_GL,
        "GR" | "BTN_GR" => EXT_BUTTON_GR,
        "SL" | "BTN_SL" => EXT_BUTTON_SL,
        "SR" | "BTN_SR" => EXT_BUTTON_SR,
        _ => 0,
    }
}

#[derive(Default)]
struct Directions {
    dpad_up: bool,
    dpad_down: bool,
    dpad_left: bool,
    dpad_right: bool,
    left_up: bool,
    left_down: bool,
    left_left: bool,
    left_right: bool,
    right_up: bool,
    right_down: bool,
    right_left: bool,
    right_right: bool,
}

fn apply_token(token: &str, step: &mut Step, directions: &mut Directions) -> Result<(), MacroError> {
    let token = trim(token).to_ascii_uppercase();
    if token.is_empty() {
        return Ok(());
    }
    let button = button_bit(&token);
    if button != 0 {
        step.buttons |= button;
        return Ok(());
    }
    let extra = extra_button_bit(&token);
    if extra != 0 {
        step.extra_buttons |= extra;
        return Ok(());
    }
    match token.as_str() {
        "DPAD_UP" | "DUP" | "UP" => directions.dpad_up = true,
        "DPAD_DOWN" | "DDOWN" | "DOWN" => directions.dpad_down = true,
        "DPAD_LEFT" | "DLEFT" | "LEFT" => directions.dpad_left = true,
        "DPAD_RIGHT" | "DRIGHT" | "RIGHT" => directions.dpad_right = true,
        "LSTICK_UP" | "LS_UP" => {
            directions.left_up = true;
            step.has_lstick = true;
        }
        "LSTICK_DOWN" | "LS_DOWN" => {
            directions.left_down = true;
            step.has_lstick = true;
        }
        "LSTICK_LEFT" | "LS_LEFT" => {
            directions.left_left = true;
            step.has_lstick = true;
        }
        "LSTICK_RIGHT" | "LS_RIGHT" => {
            directions.left_right = true;
            step.has_lstick = true;
        }
        "RSTICK_UP" | "RS_UP" => {
            directions.right_up = true;
            step.has_rstick = true;
        }
        "RSTICK_DOWN" | "RS_DOWN" => {
            directions.right_down = true;
            step.has_rstick = true;
        }
        "RSTICK_LEFT" | "RS_LEFT" => {
            directions.right_left = true;
            step.has_rstick = true;
        }
        "RSTICK_RIGHT" | "RS_RIGHT" => {
            directions.right_right = true;
            step.has_rstick = true;
        }
        _ => return Err(MacroError::new(format!("unknown macro input: {token}"))),
    }
    Ok(())
}

fn conflict(condition: bool, message: &str, command: &str) -> Result<(), MacroError> {
    if condition {
        Err(MacroError::new(format!("{message} conflict in command: {command}")))
    } else {
        Ok(())
    }
}

fn parse_one_command(part: &str) -> Result<Step, MacroError> {
    let split = part
        .char_indices()
        .rev()
        .find(|(_, ch)| ch.is_ascii_whitespace())
        .map(|(index, _)| index)
        .ok_or_else(|| MacroError::new(format!("missing duration in command: {part}")))?;
    let command = trim(&part[..split]);
    let duration_text = trim(&part[split..]);
    let duration_ms = parse_u32_strict(duration_text)
        .ok_or_else(|| MacroError::new(format!("invalid duration in command: {part}")))?;
    let mut step = Step {
        duration_ms,
        ..Step::default()
    };
    if command.eq_ignore_ascii_case("WAIT") {
        return Ok(step);
    }
    if command.is_empty() {
        return Err(MacroError::new(format!(
            "missing input before duration in command: {part}"
        )));
    }

    let normalized: String = command
        .chars()
        .map(|ch| if matches!(ch, '+' | ',' | '|') { ' ' } else { ch })
        .collect();
    let mut directions = Directions::default();
    let mut count = 0usize;
    for token in normalized.split_whitespace() {
        count += 1;
        apply_token(token, &mut step, &mut directions)?;
    }
    if count == 0 {
        return Err(MacroError::new(format!("empty input in command: {part}")));
    }

    conflict(
        directions.dpad_up && directions.dpad_down,
        "DPAD_UP and DPAD_DOWN",
        part,
    )?;
    conflict(
        directions.dpad_left && directions.dpad_right,
        "DPAD_LEFT and DPAD_RIGHT",
        part,
    )?;
    conflict(
        directions.left_up && directions.left_down,
        "LSTICK_UP and LSTICK_DOWN",
        part,
    )?;
    conflict(
        directions.left_left && directions.left_right,
        "LSTICK_LEFT and LSTICK_RIGHT",
        part,
    )?;
    conflict(
        directions.right_up && directions.right_down,
        "RSTICK_UP and RSTICK_DOWN",
        part,
    )?;
    conflict(
        directions.right_left && directions.right_right,
        "RSTICK_LEFT and RSTICK_RIGHT",
        part,
    )?;

    step.hat = match (
        directions.dpad_up,
        directions.dpad_down,
        directions.dpad_left,
        directions.dpad_right,
    ) {
        (true, false, false, true) => Hat::NorthEast,
        (true, false, true, false) => Hat::NorthWest,
        (false, true, false, true) => Hat::SouthEast,
        (false, true, true, false) => Hat::SouthWest,
        (true, false, false, false) => Hat::North,
        (false, true, false, false) => Hat::South,
        (false, false, false, true) => Hat::East,
        (false, false, true, false) => Hat::West,
        _ => Hat::Neutral,
    };
    if step.has_lstick {
        step.axes[0] = if directions.left_left {
            0
        } else if directions.left_right {
            255
        } else {
            128
        };
        step.axes[1] = if directions.left_up {
            0
        } else if directions.left_down {
            255
        } else {
            128
        };
    }
    if step.has_rstick {
        step.axes[2] = if directions.right_left {
            0
        } else if directions.right_right {
            255
        } else {
            128
        };
        step.axes[3] = if directions.right_up {
            0
        } else if directions.right_down {
            255
        } else {
            128
        };
    }
    Ok(step)
}

/// Parses and validates either the legacy command text or accepted JSON forms.
pub fn validate_text(raw_text: &str) -> Result<(Vec<Step>, Vec<String>), MacroError> {
    let mut text = extract_commands_text(raw_text)?;
    text = text.replace(['\n', '\r'], ";");
    let mut steps = Vec::new();
    let mut normalized = Vec::new();
    let mut loop_block_start = 0usize;

    for raw_part in text.split(';') {
        let part = trim(raw_part);
        if part.is_empty() || part.starts_with('#') {
            continue;
        }
        let split = part
            .char_indices()
            .rev()
            .find(|(_, ch)| ch.is_ascii_whitespace())
            .map(|(index, _)| index);
        let command = split.map_or(part, |index| trim(&part[..index]));
        if command.eq_ignore_ascii_case("LOOP") {
            let index = split
                .ok_or_else(|| MacroError::new(format!("missing count in LOOP command: {part}")))?;
            let count = parse_u32_strict(trim(&part[index..])).ok_or_else(|| {
                MacroError::new(format!("invalid LOOP count in command: {part}"))
            })?;
            if steps.len() == loop_block_start {
                return Err(MacroError::new(format!(
                    "LOOP has no previous commands to repeat: {part}"
                )));
            }
            let block_len = steps.len() - loop_block_start;
            let extra_copies = usize::try_from(count.saturating_sub(1))
                .map_err(|_| MacroError::new("LOOP expansion is too large"))?;
            let extra_steps = block_len
                .checked_mul(extra_copies)
                .ok_or_else(|| MacroError::new("LOOP expansion is too large; reduce LOOP count or split the macro"))?;
            if steps.len().saturating_add(extra_steps) > MAX_EXPANDED_STEPS {
                return Err(MacroError::new(
                    "LOOP expansion is too large; reduce LOOP count or split the macro",
                ));
            }
            let block = steps[loop_block_start..].to_vec();
            for _ in 1..count {
                steps.extend_from_slice(&block);
            }
            loop_block_start = steps.len();
            normalized.push(part.to_owned());
            continue;
        }

        if steps.len() >= MAX_EXPANDED_STEPS {
            return Err(MacroError::new("macro expands to too many steps"));
        }
        steps.push(parse_one_command(part)?);
        normalized.push(part.to_owned());
    }

    if steps.is_empty() {
        return Err(MacroError::new("no valid macro commands found"));
    }
    Ok((steps, normalized))
}

pub fn parse_text(raw_text: &str) -> Result<Vec<Step>, MacroError> {
    validate_text(raw_text).map(|(steps, _)| steps)
}

#[must_use]
pub fn extract_name_or_default(raw: &str, fallback_name: &str) -> String {
    let raw = trim(raw);
    if raw.is_empty() || !matches!(raw.as_bytes().first(), Some(b'{') | Some(b'[')) {
        return fallback_name.to_owned();
    }
    let Ok(value) = parse_json(raw) else {
        return fallback_name.to_owned();
    };
    match value.object_get("name") {
        Some(JsonValue::String(name)) if !trim(name).is_empty() => trim(name).to_owned(),
        _ => fallback_name.to_owned(),
    }
}

fn pretty_object(name: &str, hotkey: Option<&str>, commands: &[String]) -> String {
    let mut output = String::from("{\n");
    output.push_str(&format!("  \"name\": \"{}\",\n", json_escape(name)));
    if let Some(hotkey) = hotkey {
        output.push_str(&format!("  \"hotkey\": \"{}\",\n", json_escape(hotkey)));
    }
    output.push_str("  \"commands\": [\n");
    for (index, command) in commands.iter().enumerate() {
        output.push_str(&format!("    \"{}\"", json_escape(command)));
        if index + 1 != commands.len() {
            output.push(',');
        }
        output.push('\n');
    }
    output.push_str("  ]\n}");
    output
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum InvalidPrettyMode {
    #[default]
    ReturnRaw,
    FallbackWait,
}

#[must_use]
pub fn pretty_json(raw_text: &str, fallback_name: &str, mode: InvalidPrettyMode) -> String {
    match validate_text(raw_text) {
        Ok((_, lines)) => pretty_object(&extract_name_or_default(raw_text, fallback_name), None, &lines),
        Err(_) if mode == InvalidPrettyMode::ReturnRaw => raw_text.to_owned(),
        Err(_) => pretty_object(fallback_name, None, &["WAIT 200".to_owned()]),
    }
}

#[must_use]
pub fn pretty_json_with_forced_name(raw_text: &str, forced_name: &str) -> String {
    match validate_text(raw_text) {
        Ok((_, lines)) => pretty_object(forced_name, None, &lines),
        Err(_) => raw_text.to_owned(),
    }
}

pub fn validate_to_pretty_json(raw_text: &str, fallback_name: &str) -> Result<String, MacroError> {
    validate_text(raw_text)?;
    Ok(pretty_json(raw_text, fallback_name, InvalidPrettyMode::ReturnRaw))
}

#[must_use]
pub fn total_ms(steps: &[Step]) -> u64 {
    steps.iter().fold(0u64, |total, step| {
        total.saturating_add(u64::from(step.duration_ms))
    })
}

#[must_use]
pub fn step_at(steps: &[Step], elapsed_ms: u64) -> Option<&Step> {
    let mut cursor = 0u64;
    for step in steps {
        let next = cursor.saturating_add(u64::from(step.duration_ms));
        if elapsed_ms < next {
            return Some(step);
        }
        cursor = next;
    }
    None
}

#[must_use]
pub fn report_at(steps: &[Step], elapsed_ms: u64) -> Option<HoriHidReport> {
    let step = step_at(steps, elapsed_ms)?;
    Some(HoriHidReport::new(
        step.buttons,
        step.hat,
        step.axes,
        step.extra_buttons,
    ))
}

#[must_use]
pub fn buttons_to_text(buttons: u16) -> String {
    const NAMES: &[(u16, &str)] = &[
        (BTN_A, "A"),
        (BTN_B, "B"),
        (BTN_X, "X"),
        (BTN_Y, "Y"),
        (BTN_L, "L"),
        (BTN_R, "R"),
        (BTN_ZL, "ZL"),
        (BTN_ZR, "ZR"),
        (BTN_MINUS, "MINUS"),
        (BTN_PLUS, "PLUS"),
        (BTN_LSTICK, "LSTICK"),
        (BTN_RSTICK, "RSTICK"),
        (BTN_HOME, "HOME"),
        (BTN_CAPTURE, "CAPTURE"),
    ];
    NAMES
        .iter()
        .filter_map(|(bit, name)| (buttons & bit != 0).then_some(*name))
        .collect::<Vec<_>>()
        .join("+")
}

fn extra_buttons_to_text(buttons: u8) -> String {
    const NAMES: &[(u8, &str)] = &[
        (EXT_BUTTON_C, "C"),
        (EXT_BUTTON_GL, "GL"),
        (EXT_BUTTON_GR, "GR"),
        (EXT_BUTTON_SL, "SL"),
        (EXT_BUTTON_SR, "SR"),
    ];
    NAMES
        .iter()
        .filter_map(|(bit, name)| (buttons & bit != 0).then_some(*name))
        .collect::<Vec<_>>()
        .join("+")
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RecordFrame {
    buttons: u16,
    extra_buttons: u8,
    hat: Hat,
    axes: [i8; 4],
}

impl RecordFrame {
    #[must_use]
    pub fn from_report(report: &HoriHidReport) -> Self {
        fn axis_direction(value: u8) -> i8 {
            if value < 80 {
                -1
            } else if value > 176 {
                1
            } else {
                0
            }
        }
        Self {
            buttons: report.buttons(),
            extra_buttons: report.vendor() & EXT_BUTTON_MASK,
            hat: report.hat(),
            axes: report.axes().map(axis_direction),
        }
    }

    #[must_use]
    pub fn to_command_text(&self) -> String {
        let mut tokens: Vec<&str> = Vec::new();
        let buttons = buttons_to_text(self.buttons);
        let extras = extra_buttons_to_text(self.extra_buttons);
        match self.hat {
            Hat::North => tokens.push("DPAD_UP"),
            Hat::NorthEast => tokens.extend(["DPAD_UP", "DPAD_RIGHT"]),
            Hat::East => tokens.push("DPAD_RIGHT"),
            Hat::SouthEast => tokens.extend(["DPAD_DOWN", "DPAD_RIGHT"]),
            Hat::South => tokens.push("DPAD_DOWN"),
            Hat::SouthWest => tokens.extend(["DPAD_DOWN", "DPAD_LEFT"]),
            Hat::West => tokens.push("DPAD_LEFT"),
            Hat::NorthWest => tokens.extend(["DPAD_UP", "DPAD_LEFT"]),
            Hat::Neutral => {}
        }
        match self.axes[0] {
            -1 => tokens.push("LSTICK_LEFT"),
            1 => tokens.push("LSTICK_RIGHT"),
            _ => {}
        }
        match self.axes[1] {
            -1 => tokens.push("LSTICK_UP"),
            1 => tokens.push("LSTICK_DOWN"),
            _ => {}
        }
        match self.axes[2] {
            -1 => tokens.push("RSTICK_LEFT"),
            1 => tokens.push("RSTICK_RIGHT"),
            _ => {}
        }
        match self.axes[3] {
            -1 => tokens.push("RSTICK_UP"),
            1 => tokens.push("RSTICK_DOWN"),
            _ => {}
        }

        let mut pieces = Vec::new();
        if !buttons.is_empty() {
            pieces.push(buttons);
        }
        if !extras.is_empty() {
            pieces.push(extras);
        }
        if !tokens.is_empty() {
            pieces.push(tokens.join("+"));
        }
        pieces.join("+")
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Runtime {
    steps: Vec<Step>,
    running: bool,
    start_us: u64,
}

impl Runtime {
    pub fn start(&mut self, steps: Vec<Step>, now_us: u64) {
        self.steps = steps;
        self.running = true;
        self.start_us = now_us;
    }

    pub fn start_text(&mut self, raw_text: &str, now_us: u64) -> Result<(), MacroError> {
        self.start(parse_text(raw_text)?, now_us);
        Ok(())
    }

    #[must_use]
    pub fn is_running(&mut self, now_us: u64, grace_ms: u64) -> bool {
        if !self.running {
            return false;
        }
        let elapsed_ms = now_us.saturating_sub(self.start_us) / 1000;
        if elapsed_ms > total_ms(&self.steps).saturating_add(grace_ms) {
            self.running = false;
        }
        self.running
    }

    pub fn step(&mut self, now_us: u64) -> Option<&Step> {
        if !self.running {
            return None;
        }
        let elapsed_ms = now_us.saturating_sub(self.start_us) / 1000;
        let result = step_at(&self.steps, elapsed_ms);
        if result.is_none() {
            self.running = false;
        }
        result
    }

    pub fn report(&mut self, now_us: u64) -> Option<HoriHidReport> {
        if !self.running {
            return None;
        }
        let elapsed_ms = now_us.saturating_sub(self.start_us) / 1000;
        let result = report_at(&self.steps, elapsed_ms);
        if result.is_none() {
            self.running = false;
        }
        result
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Recorder {
    recording: bool,
    last_frame: RecordFrame,
    have_frame: bool,
    has_input: bool,
    last_change_us: u64,
    commands: String,
}

impl Recorder {
    pub fn start(&mut self, now_us: u64) {
        self.recording = true;
        self.last_frame = RecordFrame::default();
        self.have_frame = false;
        self.has_input = false;
        self.last_change_us = now_us;
        self.commands.clear();
    }

    fn append(&mut self, frame: RecordFrame, duration_ms: u64) {
        if duration_ms < 10 {
            return;
        }
        if !self.commands.is_empty() {
            self.commands.push_str("; ");
        }
        let combo = frame.to_command_text();
        if combo.is_empty() {
            self.commands.push_str(&format!("WAIT {duration_ms}"));
        } else {
            self.has_input = true;
            self.commands.push_str(&format!("{combo} {duration_ms}"));
        }
    }

    pub fn sample(&mut self, report: &HoriHidReport, now_us: u64, macro_playback_running: bool) {
        if !self.recording || macro_playback_running {
            return;
        }
        let frame = RecordFrame::from_report(report);
        if !self.have_frame {
            self.last_frame = frame;
            self.have_frame = true;
            self.last_change_us = now_us;
            return;
        }
        if frame != self.last_frame {
            self.append(
                self.last_frame,
                now_us.saturating_sub(self.last_change_us) / 1000,
            );
            self.last_frame = frame;
            self.last_change_us = now_us;
        }
    }

    #[must_use]
    pub fn stop(&mut self, now_us: u64) -> String {
        if self.recording && self.have_frame {
            self.append(
                self.last_frame,
                now_us.saturating_sub(self.last_change_us) / 1000,
            );
        }
        self.recording = false;
        self.have_frame = false;
        if !self.has_input {
            self.commands.clear();
            return String::new();
        }
        pretty_json(
            &self.commands,
            "Recorded Macro",
            InvalidPrettyMode::ReturnRaw,
        )
    }
}

fn normalized_hotkey(value: &str, normalize: Option<&dyn Fn(&str) -> String>) -> String {
    normalize.map_or_else(|| trim(value).to_owned(), |function| function(value))
}

#[must_use]
pub fn entry_to_object_json(
    entry: &Entry,
    normalize: Option<&dyn Fn(&str) -> String>,
) -> String {
    let lines = validate_text(&entry.json)
        .map(|(_, lines)| lines)
        .unwrap_or_else(|_| vec!["WAIT 200".to_owned()]);
    let name = if trim(&entry.name).is_empty() {
        extract_name_or_default(&entry.json, "Macro")
    } else {
        entry.name.clone()
    };
    pretty_object(
        &name,
        Some(&normalized_hotkey(&entry.hotkey, normalize)),
        &lines,
    )
}

#[must_use]
pub fn entries_to_json(entries: &[Entry], normalize: Option<&dyn Fn(&str) -> String>) -> String {
    let rendered = entries
        .iter()
        .map(|entry| entry_to_object_json(entry, normalize))
        .collect::<Vec<_>>();
    if rendered.is_empty() {
        return "{\n  \"macros\": []\n}".to_owned();
    }
    let indented = rendered
        .iter()
        .map(|entry| {
            entry
                .lines()
                .map(|line| format!("    {line}"))
                .collect::<Vec<_>>()
                .join("\n")
        })
        .collect::<Vec<_>>()
        .join(",\n");
    format!("{{\n  \"macros\": [\n{indented}\n  ]\n}}")
}

pub fn parse_entries_text(
    raw: &str,
    normalize: Option<&dyn Fn(&str) -> String>,
) -> Result<Vec<Entry>, MacroError> {
    if raw.len() > JSON_MAX_BYTES {
        return Err(MacroError::new("macro JSON exceeds 50MB limit"));
    }
    let text = trim(raw);
    if text.is_empty() {
        return Ok(Vec::new());
    }
    let parsed = parse_json(text)?;
    let items: Vec<&JsonValue> = match &parsed {
        JsonValue::Object(_) => match parsed.object_get("macros") {
            Some(JsonValue::Array(items)) => items.iter().collect(),
            _ => vec![&parsed],
        },
        JsonValue::Array(items) => items.iter().collect(),
        _ => vec![&parsed],
    };

    let mut output = Vec::with_capacity(items.len());
    for item in items {
        let object_text = match item {
            JsonValue::String(text) => text.clone(),
            value => json_compact(value),
        };
        let pretty = validate_to_pretty_json(&object_text, "Macro")?;
        let name = extract_name_or_default(&object_text, "Macro");
        let hotkey = match item.object_get("hotkey") {
            Some(JsonValue::String(value)) => normalized_hotkey(value, normalize),
            _ => String::new(),
        };
        output.push(Entry::new(name, hotkey, pretty));
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::{
        InvalidPrettyMode, Recorder, Runtime, parse_entries_text, parse_text, pretty_json, total_ms,
        validate_text,
    };
    use crate::protocol::{BTN_A, BTN_R, Hat, HoriHidReport};

    #[test]
    fn parses_legacy_commands_and_aliases() {
        let steps = parse_text("WAIT 100; A 100; R+LSTICK_LEFT 450").expect("valid macro");
        assert_eq!(steps.len(), 3);
        assert_eq!(steps[0].duration_ms(), 100);
        assert_eq!(steps[1].buttons(), BTN_A);
        assert_eq!(steps[2].buttons(), BTN_R);
        assert_eq!(steps[2].axes()[0], 0);
        assert_eq!(total_ms(&steps), 650);
    }

    #[test]
    fn parses_json_and_loop_exactly_once_plus_repeats() {
        let (steps, normalized) = validate_text(
            r#"{"name":"Dash","commands":["A 10","WAIT 20","LOOP 3"]}"#,
        )
        .expect("valid JSON macro");
        assert_eq!(steps.len(), 6);
        assert_eq!(normalized, ["A 10", "WAIT 20", "LOOP 3"]);
        assert_eq!(total_ms(&steps), 90);
    }

    #[test]
    fn rejects_conflicting_or_unbounded_inputs() {
        assert!(parse_text("DPAD_UP+DPAD_DOWN 10").is_err());
        assert!(parse_text("UNKNOWN 10").is_err());
        assert!(parse_text("LOOP 2").is_err());
        assert!(parse_text("A 10; LOOP 4294967295").is_err());
    }

    #[test]
    fn pretty_json_preserves_name_and_normalized_commands() {
        let pretty = pretty_json(
            r#"{"name":"Test","commands":"A 10; WAIT 20"}"#,
            "Macro",
            InvalidPrettyMode::ReturnRaw,
        );
        assert!(pretty.contains("\"name\": \"Test\""));
        assert!(pretty.contains("\"A 10\""));
    }

    #[test]
    fn runtime_produces_reports() {
        let mut runtime = Runtime::default();
        runtime.start_text("A 100; WAIT 100", 1_000_000).expect("valid macro");
        assert_eq!(runtime.report(1_050_000).expect("active").buttons(), BTN_A);
        assert_eq!(runtime.report(1_150_000).expect("active").buttons(), 0);
        assert!(runtime.report(1_250_000).is_none());
    }

    #[test]
    fn recorder_matches_cpp_thresholds() {
        let neutral = HoriHidReport::default();
        let pressed = HoriHidReport::new(BTN_A, Hat::Neutral, [128; 4], 0);
        let mut recorder = Recorder::default();
        recorder.start(0);
        recorder.sample(&neutral, 0, false);
        recorder.sample(&pressed, 20_000, false);
        recorder.sample(&neutral, 50_000, false);
        let json = recorder.stop(70_000);
        assert!(json.contains("WAIT 20"));
        assert!(json.contains("A 30"));
    }

    #[test]
    fn entry_parser_supports_wrapped_macro_arrays() {
        let entries = parse_entries_text(
            r#"{"macros":[{"name":"One","hotkey":" Ctrl+1 ","commands":["A 10"]}]}"#,
            None,
        )
        .expect("valid entries");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].name(), "One");
        assert_eq!(entries[0].hotkey(), "Ctrl+1");
    }
}
