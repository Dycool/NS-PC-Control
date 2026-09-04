use ns_shared::protocol::{Hat, HoriHidReport};
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

const JS_EVENT_BUTTON: u8 = 0x01;
const JS_EVENT_AXIS: u8 = 0x02;
const JS_EVENT_INIT: u8 = 0x80;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct JoystickEvent { timestamp_ms: u32, value: i16, event_type: u8, number: u8 }

impl JoystickEvent {
    pub fn timestamp_ms(&self) -> u32 { self.timestamp_ms }
    pub fn value(&self) -> i16 { self.value }
    pub fn is_button(&self) -> bool { self.event_type & !JS_EVENT_INIT == JS_EVENT_BUTTON }
    pub fn is_axis(&self) -> bool { self.event_type & !JS_EVENT_INIT == JS_EVENT_AXIS }
    pub fn number(&self) -> u8 { self.number }
    pub fn decode(bytes: [u8; 8]) -> Self {
        Self { timestamp_ms: u32::from_le_bytes(bytes[..4].try_into().expect("fixed slice")), value: i16::from_le_bytes(bytes[4..6].try_into().expect("fixed slice")), event_type: bytes[6], number: bytes[7] }
    }
}

pub struct LinuxJoystick { path: PathBuf, file: File, report: HoriHidReport }

impl LinuxJoystick {
    pub fn open(path: impl Into<PathBuf>) -> io::Result<Self> {
        let path = path.into();
        let file = File::open(&path)?;
        Ok(Self { path, file, report: HoriHidReport::default() })
    }
    pub fn path(&self) -> &Path { &self.path }
    pub fn read_event(&mut self) -> io::Result<JoystickEvent> {
        let mut bytes = [0_u8; 8];
        self.file.read_exact(&mut bytes)?;
        Ok(JoystickEvent::decode(bytes))
    }
    pub fn apply_event(&mut self, event: JoystickEvent) -> HoriHidReport {
        let mut buttons = self.report.buttons();
        if event.is_button() && event.number() < 14 {
            let mask = 1_u16 << event.number();
            if event.value() != 0 { buttons |= mask; } else { buttons &= !mask; }
            self.report.set_buttons(buttons);
        } else if event.is_axis() {
            let value = axis_to_u8(event.value());
            let [mut lx, mut ly, mut rx, mut ry] = self.report.sticks();
            match event.number() {
                0 => lx = value,
                1 => ly = value,
                2 => rx = value,
                3 => ry = value,
                6 => self.report.set_hat(axis_hat(event.value(), true)),
                7 => self.report.set_hat(axis_hat(event.value(), false)),
                _ => {}
            }
            self.report.set_sticks(lx, ly, rx, ry);
        }
        self.report
    }
}

pub fn discover_linux_joysticks() -> io::Result<Vec<PathBuf>> {
    let root = Path::new("/dev/input");
    if !root.exists() { return Ok(Vec::new()); }
    let mut paths = fs::read_dir(root)?.filter_map(Result::ok).map(|entry| entry.path()).filter(|path| path.file_name().and_then(|name| name.to_str()).is_some_and(|name| name.starts_with("js"))).collect::<Vec<_>>();
    paths.sort();
    Ok(paths)
}

fn axis_to_u8(value: i16) -> u8 {
    let shifted = i32::from(value) + 32_768;
    ((shifted * 255) / 65_535).clamp(0, 255) as u8
}

fn axis_hat(value: i16, horizontal: bool) -> Hat {
    if horizontal {
        if value < -16_000 { Hat::West } else if value > 16_000 { Hat::East } else { Hat::Neutral }
    } else if value < -16_000 { Hat::North } else if value > 16_000 { Hat::South } else { Hat::Neutral }
}
