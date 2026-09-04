use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

pub const BT_RECONNECT_PAUSE_FILE: &str = "/tmp/ns-pc-control-bt-reconnect-paused";
const HID_UUID: &str = "00001124-0000-1000-8000-00805f9b34fb";
const HOGP_UUID: &str = "00001812-0000-1000-8000-00805f9b34fb";

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BluetoothDevice {
    address: String,
    name: String,
    icon: String,
    paired: bool,
    trusted: bool,
    connected: bool,
    has_hid_uuid: bool,
}

impl BluetoothDevice {
    #[must_use]
    pub fn address(&self) -> &str {
        &self.address
    }

    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub const fn paired(&self) -> bool {
        self.paired
    }

    #[must_use]
    pub const fn trusted(&self) -> bool {
        self.trusted
    }

    #[must_use]
    pub const fn connected(&self) -> bool {
        self.connected
    }

    #[must_use]
    pub fn is_controller_like(&self) -> bool {
        if self.has_hid_uuid {
            return true;
        }
        let text = format!("{} {}", self.name, self.icon).to_ascii_lowercase();
        const KEYWORDS: [&str; 20] = [
            "gamepad",
            "joystick",
            "controller",
            "wireless controller",
            "xbox",
            "dualsense",
            "dualshock",
            "playstation",
            "8bitdo",
            "gulikit",
            "gamesir",
            "joy-con",
            "joycon",
            "nintendo",
            "pro controller",
            "horipad",
            "nacon",
            "flydigi",
            "powera",
            "stadia",
        ];
        KEYWORDS.iter().any(|keyword| text.contains(keyword))
    }
}

#[derive(Clone, Debug)]
pub struct BluetoothManager {
    bluetoothctl: String,
    btmgmt: String,
}

impl Default for BluetoothManager {
    fn default() -> Self {
        Self {
            bluetoothctl: "bluetoothctl".to_owned(),
            btmgmt: "btmgmt".to_owned(),
        }
    }
}

impl BluetoothManager {
    #[must_use]
    pub fn with_command(command: impl Into<String>) -> Self {
        Self {
            bluetoothctl: command.into(),
            btmgmt: "btmgmt".to_owned(),
        }
    }

    #[must_use]
    pub fn with_commands(
        bluetoothctl: impl Into<String>,
        btmgmt: impl Into<String>,
    ) -> Self {
        Self {
            bluetoothctl: bluetoothctl.into(),
            btmgmt: btmgmt.into(),
        }
    }

    pub fn list_devices(&self) -> io::Result<Vec<BluetoothDevice>> {
        let output = self.run_ctl(["devices"])?;
        if !output.status.success() {
            return Err(command_error("bluetoothctl devices", &output));
        }
        let mut devices = Vec::new();
        for (address, fallback_name) in parse_device_list(&output.stdout) {
            let info = self.run_ctl(["info", address.as_str()])?;
            let mut device = if info.status.success() {
                parse_device_info(&address, &info.stdout)
            } else {
                BluetoothDevice {
                    address: address.clone(),
                    name: fallback_name,
                    icon: String::new(),
                    paired: false,
                    trusted: false,
                    connected: false,
                    has_hid_uuid: false,
                }
            };
            if device.name.is_empty() {
                device.name = address.clone();
            }
            devices.push(device);
        }
        Ok(devices)
    }

    pub fn power_on(&self) -> io::Result<bool> {
        Ok(self.run_ctl(["power", "on"])?.status.success())
    }

    pub fn scan_on(&self) -> io::Result<bool> {
        Ok(self.run_ctl(["scan", "on"])?.status.success())
    }

    pub fn scan_off(&self) -> io::Result<bool> {
        Ok(self.run_ctl(["scan", "off"])?.status.success())
    }

    pub fn pair(&self, address: &str) -> io::Result<bool> {
        Ok(self.run_ctl(["pair", address])?.status.success())
    }

    pub fn trust(&self, address: &str) -> io::Result<bool> {
        Ok(self.run_ctl(["trust", address])?.status.success())
    }

    pub fn connect(&self, address: &str) -> io::Result<bool> {
        Ok(self.run_ctl(["connect", address])?.status.success())
    }

    pub fn disconnect(&self, address: &str) -> io::Result<bool> {
        Ok(self.run_ctl(["disconnect", address])?.status.success())
    }

    pub fn remove(&self, address: &str) -> io::Result<bool> {
        Ok(self.run_ctl(["remove", address])?.status.success())
    }

    pub fn pair_controller(&self, address: &str) -> io::Result<bool> {
        if !self.pair(address)? {
            return Ok(false);
        }
        if !self.trust(address)? {
            return Ok(false);
        }
        self.connect(address)
    }

    pub fn reconnect_controllers(&self) -> io::Result<usize> {
        if Path::new(BT_RECONNECT_PAUSE_FILE).exists() {
            return Ok(0);
        }
        let mut connected = 0usize;
        for device in self.list_devices()? {
            if device.paired()
                && device.trusted()
                && !device.connected()
                && device.is_controller_like()
                && self.connect(device.address())?
            {
                connected += 1;
            }
        }
        Ok(connected)
    }

    pub fn disconnect_controllers(&self) -> io::Result<usize> {
        let mut disconnected = 0usize;
        for device in self.list_devices()? {
            if device.connected()
                && device.is_controller_like()
                && self.disconnect(device.address())?
            {
                disconnected += 1;
            }
        }
        Ok(disconnected)
    }

    pub fn run_reconnect_loop(
        &self,
        running: &AtomicBool,
        interval: Duration,
    ) -> io::Result<()> {
        while running.load(Ordering::Acquire) {
            let _ = self.reconnect_controllers();
            let mut slept = Duration::ZERO;
            while slept < interval && running.load(Ordering::Acquire) {
                let slice = Duration::from_millis(100).min(interval - slept);
                thread::sleep(slice);
                slept += slice;
            }
        }
        Ok(())
    }

    pub fn runtime_setup(&self, verbose: bool) -> io::Result<()> {
        if command_exists("rfkill") {
            run_status("rfkill", ["unblock", "bluetooth"], verbose)?;
        }
        if command_exists("modprobe") {
            run_status("modprobe", ["uhid"], verbose)?;
            let _ = Command::new("modprobe")
                .args([
                    "-a",
                    "hidp",
                    "joydev",
                    "hid_playstation",
                    "hid_sony",
                    "hid_nintendo",
                    "hid_microsoft",
                ])
                .status();
        }
        if command_exists("systemctl") {
            run_status("systemctl", ["start", "bluetooth.service"], verbose)?;
        } else if command_exists("service") {
            run_status("service", ["bluetooth", "start"], verbose)?;
        }
        let ertm = Path::new("/sys/module/bluetooth/parameters/disable_ertm");
        if ertm.exists() {
            let _ = fs::write(ertm, b"1");
        }
        if Path::new("/etc/bluetooth").exists() {
            let _ = configure_bluez_reconnect_policy(Path::new("/etc/bluetooth/main.conf"));
        }
        if command_exists(&self.btmgmt) {
            for arguments in [
                ["power", "on"],
                ["connectable", "on"],
                ["bondable", "on"],
                ["ssp", "on"],
                ["fast-conn", "on"],
            ] {
                let _ = Command::new(&self.btmgmt).args(arguments).status();
            }
        }
        Ok(())
    }

    fn run_ctl<const N: usize>(&self, arguments: [&str; N]) -> io::Result<Output> {
        Command::new(&self.bluetoothctl).args(arguments).output()
    }
}

pub fn set_proactive_reconnect_enabled(enabled: bool) -> io::Result<()> {
    let path = Path::new(BT_RECONNECT_PAUSE_FILE);
    if enabled {
        match fs::remove_file(path) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(error),
        }
    } else {
        fs::write(path, b"paused\n")
    }
}

pub fn configure_bluez_reconnect_policy(path: &Path) -> io::Result<bool> {
    let original = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == io::ErrorKind::NotFound => String::new(),
        Err(error) => return Err(error),
    };
    let mut lines: Vec<String> = original.lines().map(str::to_owned).collect();
    if lines.is_empty() {
        lines.extend(["[General]".to_owned(), String::new(), "[Policy]".to_owned()]);
    }
    let mut changed = false;
    changed |= ensure_ini_key(&mut lines, "General", "FastConnectable", "true");
    changed |= ensure_ini_key(&mut lines, "BR", "PageScanType", "1");
    changed |= ensure_ini_key(&mut lines, "BR", "PageScanInterval", "128");
    changed |= ensure_ini_key(&mut lines, "BR", "PageScanWindow", "48");
    changed |= ensure_ini_key(
        &mut lines,
        "Policy",
        "ReconnectUUIDs",
        "00001124-0000-1000-8000-00805f9b34fb,00001812-0000-1000-8000-00805f9b34fb",
    );
    changed |= ensure_ini_key(&mut lines, "Policy", "ReconnectAttempts", "15");
    changed |= ensure_ini_key(
        &mut lines,
        "Policy",
        "ReconnectIntervals",
        "1,1,1,2,2,2,4,4,8,8,16,16,32",
    );
    changed |= ensure_ini_key(&mut lines, "Policy", "AutoEnable", "true");
    if changed {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut output = lines.join("\n");
        output.push('\n');
        fs::write(path, output)?;
    }
    Ok(changed)
}

fn ensure_ini_key(lines: &mut Vec<String>, section: &str, key: &str, value: &str) -> bool {
    let section_header = format!("[{section}]");
    let wanted = format!("{key} = {value}");
    let Some(section_index) = lines.iter().position(|line| line.trim() == section_header) else {
        if !lines.is_empty() && !lines.last().is_some_and(String::is_empty) {
            lines.push(String::new());
        }
        lines.push(section_header);
        lines.push(wanted);
        return true;
    };
    let end_index = lines
        .iter()
        .enumerate()
        .skip(section_index + 1)
        .find(|(_, line)| {
            let trimmed = line.trim();
            trimmed.starts_with('[') && trimmed.ends_with(']')
        })
        .map_or(lines.len(), |(index, _)| index);
    for index in section_index + 1..end_index {
        let trimmed = lines[index].trim();
        let candidate = trimmed.strip_prefix('#').map_or(trimmed, str::trim);
        let key_matches = candidate
            .strip_prefix(key)
            .is_some_and(|tail| tail.is_empty() || tail.starts_with(char::is_whitespace) || tail.starts_with('='));
        if key_matches {
            if trimmed == wanted {
                return false;
            }
            lines[index] = wanted;
            return true;
        }
    }
    lines.insert(end_index, wanted);
    true
}

fn parse_device_list(bytes: &[u8]) -> Vec<(String, String)> {
    String::from_utf8_lossy(bytes)
        .lines()
        .filter_map(|line| {
            let line = line.trim();
            let rest = line.strip_prefix("Device ")?;
            let (address, name) = rest.split_once(' ').unwrap_or((rest, ""));
            valid_mac(address).then(|| (address.to_ascii_uppercase(), name.to_owned()))
        })
        .collect()
}

fn parse_device_info(address: &str, bytes: &[u8]) -> BluetoothDevice {
    let mut device = BluetoothDevice {
        address: address.to_ascii_uppercase(),
        name: String::new(),
        icon: String::new(),
        paired: false,
        trusted: false,
        connected: false,
        has_hid_uuid: false,
    };
    for line in String::from_utf8_lossy(bytes).lines() {
        let line = line.trim();
        let Some((key, value)) = line.split_once(':') else {
            continue;
        };
        let value = value.trim();
        match key.trim() {
            "Name" | "Alias" if device.name.is_empty() => device.name = value.to_owned(),
            "Icon" => device.icon = value.to_owned(),
            "Paired" => device.paired = parse_yes(value),
            "Trusted" => device.trusted = parse_yes(value),
            "Connected" => device.connected = parse_yes(value),
            "UUID" => {
                let lower = value.to_ascii_lowercase();
                device.has_hid_uuid |= lower.contains(HID_UUID) || lower.contains(HOGP_UUID);
            }
            _ => {}
        }
    }
    device
}

fn parse_yes(value: &str) -> bool {
    value.eq_ignore_ascii_case("yes") || value.eq_ignore_ascii_case("true")
}

fn valid_mac(address: &str) -> bool {
    let bytes = address.as_bytes();
    bytes.len() == 17
        && bytes.iter().enumerate().all(|(index, value)| {
            if index % 3 == 2 {
                *value == b':'
            } else {
                value.is_ascii_hexdigit()
            }
        })
}

fn command_exists(command: &str) -> bool {
    let path = Path::new(command);
    if path.components().count() > 1 {
        return path.is_file();
    }
    env::var_os("PATH").is_some_and(|paths| {
        env::split_paths(&paths).any(|directory| directory.join(command).is_file())
    })
}

fn run_status<const N: usize>(command: &str, arguments: [&str; N], verbose: bool) -> io::Result<()> {
    if verbose {
        eprintln!("[bt] setup: {command} {}", arguments.join(" "));
    }
    let status = Command::new(command).args(arguments).status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{command} exited with status {status}"
        )))
    }
}

fn command_error(name: &str, output: &Output) -> io::Error {
    let detail = String::from_utf8_lossy(&output.stderr).trim().to_owned();
    io::Error::other(if detail.is_empty() {
        format!("{name} failed with status {}", output.status)
    } else {
        format!("{name} failed: {detail}")
    })
}

#[cfg(test)]
mod tests {
    use super::{ensure_ini_key, parse_device_info, parse_device_list};

    #[test]
    fn parses_bluetoothctl_device_and_info_output() {
        let devices = parse_device_list(
            b"Device AA:BB:CC:DD:EE:FF Nintendo Pro Controller\ninvalid\n",
        );
        assert_eq!(devices.len(), 1);
        let info = parse_device_info(
            &devices[0].0,
            b"Name: Pro Controller\nPaired: yes\nTrusted: yes\nConnected: no\nUUID: Human Interface Device (00001124-0000-1000-8000-00805f9b34fb)\n",
        );
        assert!(info.paired());
        assert!(info.trusted());
        assert!(!info.connected());
        assert!(info.is_controller_like());
    }

    #[test]
    fn updates_or_inserts_bluez_ini_keys() {
        let mut lines = vec![
            "[General]".to_owned(),
            "#FastConnectable = false".to_owned(),
            String::new(),
            "[Policy]".to_owned(),
        ];
        assert!(ensure_ini_key(
            &mut lines,
            "General",
            "FastConnectable",
            "true"
        ));
        assert!(lines.iter().any(|line| line == "FastConnectable = true"));
        assert!(ensure_ini_key(
            &mut lines,
            "Policy",
            "ReconnectAttempts",
            "15"
        ));
        assert!(lines.iter().any(|line| line == "ReconnectAttempts = 15"));
    }
}
