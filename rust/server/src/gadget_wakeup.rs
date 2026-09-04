use crate::virtual_controller::{LEGACY_REPORT_DESC, VIRTUAL_CONTROLLER_REPORT_DESC};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const DEFAULT_GADGET_DIR: &str = "/sys/kernel/config/usb_gadget/ns_ctrl";
pub const DEFAULT_UDC_ROOT: &str = "/sys/class/udc";
pub const DEFAULT_HID_DEVICE_ROOT: &str = "/dev";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GadgetIdentity {
    Switch1,
    Switch2,
    Hori,
}

impl GadgetIdentity {
    #[must_use]
    pub const fn vendor_id(self) -> &'static str {
        match self {
            Self::Hori => "0x0F0D",
            Self::Switch1 | Self::Switch2 => "0x057e",
        }
    }

    #[must_use]
    pub const fn product_id(self) -> &'static str {
        match self {
            Self::Hori => "0x0092",
            Self::Switch1 => "0x2009",
            Self::Switch2 => "0x2069",
        }
    }

    #[must_use]
    pub const fn bcd_device(self) -> &'static str {
        match self {
            Self::Switch2 => "0x0400",
            Self::Switch1 | Self::Hori => "0x0200",
        }
    }

    #[must_use]
    pub const fn product_string(self) -> &'static str {
        match self {
            Self::Hori => "Legacy USB Gamepad",
            Self::Switch1 => "Nintendo Switch Pro Controller",
            Self::Switch2 => "Switch 2 Pro Controller",
        }
    }

    #[must_use]
    pub const fn manufacturer(self) -> &'static str {
        match self {
            Self::Hori => "NS Bridge",
            Self::Switch1 | Self::Switch2 => "Nintendo",
        }
    }

    #[must_use]
    pub const fn report_length(self) -> usize {
        match self {
            Self::Hori => 8,
            Self::Switch1 | Self::Switch2 => 64,
        }
    }

    #[must_use]
    pub const fn report_descriptor(self) -> &'static [u8] {
        match self {
            Self::Hori => &LEGACY_REPORT_DESC,
            Self::Switch1 | Self::Switch2 => VIRTUAL_CONTROLLER_REPORT_DESC,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WakeAdvertisement {
    hci_device: String,
    mac: String,
    advertising_hex: String,
    original_mac: Option<String>,
}

impl WakeAdvertisement {
    pub fn from_config(path: &Path) -> io::Result<Self> {
        let text = fs::read_to_string(path)?;
        Self::parse(&text)
    }

    pub fn parse(text: &str) -> io::Result<Self> {
        let mut hci_device = "hci0".to_owned();
        let mut mac = String::new();
        let mut advertising_hex = String::new();
        let mut original_mac = None;
        for raw_line in text.lines() {
            let line = raw_line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((key, value)) = line.split_once('=') else {
                continue;
            };
            let value = value.trim();
            match key.trim() {
                "hci" | "hci_device" => hci_device = value.to_owned(),
                "mac" => mac = value.to_ascii_lowercase(),
                "adv" | "advertising_hex" | "adv_hex" => {
                    advertising_hex = value
                        .chars()
                        .filter(|character| !character.is_ascii_whitespace())
                        .flat_map(char::to_uppercase)
                        .collect();
                }
                "original_mac" if valid_mac(value) => {
                    original_mac = Some(value.to_ascii_lowercase());
                }
                _ => {}
            }
        }
        if !valid_mac(&mac) || !valid_adv_hex(&advertising_hex) || !valid_hci(&hci_device) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "wake config contains an invalid MAC, HCI device, or advertisement payload",
            ));
        }
        Ok(Self {
            hci_device,
            mac,
            advertising_hex,
            original_mac,
        })
    }

    #[must_use]
    pub fn mac(&self) -> &str {
        &self.mac
    }

    #[must_use]
    pub fn hci_device(&self) -> &str {
        &self.hci_device
    }

    #[must_use]
    pub fn advertising_hex(&self) -> &str {
        &self.advertising_hex
    }

    #[must_use]
    pub fn original_mac(&self) -> Option<&str> {
        self.original_mac.as_deref()
    }

    pub fn save(&self, path: &Path) -> io::Result<()> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut output = format!(
            "# NS-PC-Control Switch 2 wake config\nmac={}\nadv={}\nhci={}\n",
            self.mac, self.advertising_hex, self.hci_device
        );
        if let Some(original_mac) = &self.original_mac {
            output.push_str(&format!("original_mac={original_mac}\n"));
        }
        fs::write(path, output)
    }

    pub fn advertise(&self) -> io::Result<bool> {
        let index = self.hci_device.trim_start_matches("hci");
        let status = Command::new("btmgmt")
            .args([
                "--index",
                index,
                "add-adv",
                "-d",
                self.advertising_hex.as_str(),
                "1",
            ])
            .status()?;
        Ok(status.success())
    }

    pub fn stop_advertising(&self) -> io::Result<bool> {
        let index = self.hci_device.trim_start_matches("hci");
        let status = Command::new("btmgmt")
            .args(["--index", index, "rm-adv", "1"])
            .status()?;
        Ok(status.success())
    }
}

#[derive(Clone, Debug)]
pub struct ConfigFsGadget {
    gadget_dir: PathBuf,
    udc_root: PathBuf,
    device_root: PathBuf,
}

impl Default for ConfigFsGadget {
    fn default() -> Self {
        Self::new(DEFAULT_GADGET_DIR, DEFAULT_UDC_ROOT, DEFAULT_HID_DEVICE_ROOT)
    }
}

impl ConfigFsGadget {
    #[must_use]
    pub fn new(
        gadget_dir: impl Into<PathBuf>,
        udc_root: impl Into<PathBuf>,
        device_root: impl Into<PathBuf>,
    ) -> Self {
        Self {
            gadget_dir: gadget_dir.into(),
            udc_root: udc_root.into(),
            device_root: device_root.into(),
        }
    }

    #[must_use]
    pub fn gadget_dir(&self) -> &Path {
        &self.gadget_dir
    }

    pub fn setup_legacy(&self, identity: GadgetIdentity, serial: &str) -> io::Result<()> {
        if identity == GadgetIdentity::Switch2 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Switch 2 uses the native Raw Gadget transport, not legacy f_hid",
            ));
        }
        self.unbind()?;
        let strings = self.gadget_dir.join("strings/0x409");
        let config = self.gadget_dir.join("configs/c.1");
        fs::create_dir_all(&strings)?;
        fs::create_dir_all(&config)?;
        fs::create_dir_all(self.gadget_dir.join("functions"))?;

        write_value(self.gadget_dir.join("bcdDevice"), identity.bcd_device())?;
        write_value(self.gadget_dir.join("bcdUSB"), "0x0200")?;
        write_value(self.gadget_dir.join("idVendor"), identity.vendor_id())?;
        write_value(self.gadget_dir.join("idProduct"), identity.product_id())?;
        write_value(
            self.gadget_dir.join("bDeviceClass"),
            if identity == GadgetIdentity::Hori { "0xFF" } else { "0xEF" },
        )?;
        write_value(
            self.gadget_dir.join("bDeviceSubClass"),
            if identity == GadgetIdentity::Hori { "0xFF" } else { "0x02" },
        )?;
        write_value(
            self.gadget_dir.join("bDeviceProtocol"),
            if identity == GadgetIdentity::Hori { "0xFF" } else { "0x01" },
        )?;
        write_value(
            strings.join("serialnumber"),
            if identity == GadgetIdentity::Hori {
                "000000000001"
            } else {
                serial
            },
        )?;
        write_value(strings.join("manufacturer"), identity.manufacturer())?;
        write_value(strings.join("product"), identity.product_string())?;
        write_value(config.join("MaxPower"), "500")?;

        for port in 0..4 {
            self.create_hid_function(port, identity)?;
        }
        let udc = self
            .first_udc_name()?
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "no USB device controller found"))?;
        write_value(self.gadget_dir.join("UDC"), &udc)
    }

    pub fn unbind(&self) -> io::Result<()> {
        let udc = self.gadget_dir.join("UDC");
        if udc.exists() {
            fs::write(udc, b"")?;
        }
        Ok(())
    }

    #[must_use]
    pub fn hidg_nodes_ready(&self, count: usize) -> bool {
        (0..count).all(|port| self.device_root.join(format!("hidg{port}")).exists())
    }

    pub fn first_udc_name(&self) -> io::Result<Option<String>> {
        if !self.udc_root.exists() {
            return Ok(None);
        }
        let mut names = fs::read_dir(&self.udc_root)?
            .filter_map(Result::ok)
            .filter_map(|entry| entry.file_name().into_string().ok())
            .collect::<Vec<_>>();
        names.sort();
        Ok(names.into_iter().next())
    }

    fn create_hid_function(&self, port: usize, identity: GadgetIdentity) -> io::Result<()> {
        let function_name = format!("hid.usb{port}");
        let function = self.gadget_dir.join("functions").join(&function_name);
        fs::create_dir_all(&function)?;
        write_value(function.join("protocol"), "0")?;
        write_value(function.join("subclass"), "0")?;
        write_value(
            function.join("report_length"),
            &identity.report_length().to_string(),
        )?;
        fs::write(function.join("report_desc"), identity.report_descriptor())?;
        let link = self.gadget_dir.join("configs/c.1").join(function_name);
        if link.exists() || fs::symlink_metadata(&link).is_ok() {
            fs::remove_file(&link)?;
        }
        create_symlink(&function, &link)
    }
}

fn write_value(path: impl AsRef<Path>, value: &str) -> io::Result<()> {
    fs::write(path, value.as_bytes())
}

#[cfg(unix)]
fn create_symlink(source: &Path, target: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(source, target)
}

#[cfg(not(unix))]
fn create_symlink(_source: &Path, _target: &Path) -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "USB configfs gadget setup is only supported on Unix",
    ))
}

fn valid_mac(mac: &str) -> bool {
    let bytes = mac.as_bytes();
    bytes.len() == 17
        && bytes.iter().enumerate().all(|(index, byte)| {
            if index % 3 == 2 {
                *byte == b':'
            } else {
                byte.is_ascii_hexdigit()
            }
        })
}

fn valid_adv_hex(adv: &str) -> bool {
    !adv.is_empty()
        && adv.len().is_multiple_of(2)
        && adv.len() <= 62
        && adv.as_bytes().iter().all(u8::is_ascii_hexdigit)
}

fn valid_hci(hci: &str) -> bool {
    hci.strip_prefix("hci")
        .is_some_and(|suffix| !suffix.is_empty() && suffix.bytes().all(|byte| byte.is_ascii_digit()))
}

#[cfg(test)]
mod tests {
    use super::{ConfigFsGadget, GadgetIdentity, WakeAdvertisement};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn parses_cpp_wake_config_contract() {
        let wake = WakeAdvertisement::parse(
            "mac=aa:bb:cc:dd:ee:ff\nadv=02010603FFAA55\nhci=hci0\noriginal_mac=11:22:33:44:55:66\n",
        )
        .expect("wake config");
        assert_eq!(wake.mac(), "aa:bb:cc:dd:ee:ff");
        assert_eq!(wake.advertising_hex(), "02010603FFAA55");
        assert_eq!(wake.original_mac(), Some("11:22:33:44:55:66"));
    }

    #[test]
    fn builds_legacy_configfs_shape() {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("time")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("ns-gadget-{nonce}"));
        let gadget = root.join("gadget");
        let udc = root.join("udc");
        let dev = root.join("dev");
        fs::create_dir_all(udc.join("dummy_udc")).expect("udc");
        fs::create_dir_all(&dev).expect("dev");
        let configurator = ConfigFsGadget::new(&gadget, &udc, &dev);
        configurator
            .setup_legacy(GadgetIdentity::Switch1, "NSGP260606A0")
            .expect("setup");
        assert_eq!(fs::read_to_string(gadget.join("idVendor")).expect("vendor"), "0x057e");
        assert_eq!(fs::read_to_string(gadget.join("idProduct")).expect("product"), "0x2009");
        assert_eq!(fs::read_to_string(gadget.join("UDC")).expect("udc attr"), "dummy_udc");
        assert!(gadget.join("configs/c.1/hid.usb0").exists());
        let _ = fs::remove_dir_all(root);
    }
}
