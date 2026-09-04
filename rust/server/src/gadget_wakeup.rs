use std::fs;
use std::io;
use std::path::Path;
use std::process::Command;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WakeAdvertisement { hci_device: String, mac: String, advertising_hex: String }

impl WakeAdvertisement {
    pub fn from_config(path: &Path) -> io::Result<Self> {
        let text = fs::read_to_string(path)?;
        let mut hci_device = "hci0".to_string();
        let mut mac = String::new();
        let mut advertising_hex = String::new();
        for line in text.lines() {
            let Some((key, value)) = line.split_once('=') else { continue; };
            match key.trim() {
                "hci" | "hci_device" => hci_device = value.trim().to_string(),
                "mac" => mac = value.trim().to_string(),
                "advertising_hex" | "adv_hex" => advertising_hex = value.trim().to_string(),
                _ => {}
            }
        }
        if mac.is_empty() || advertising_hex.is_empty() { return Err(io::Error::new(io::ErrorKind::InvalidData, "wake config is incomplete")); }
        Ok(Self { hci_device, mac, advertising_hex })
    }
    pub fn mac(&self) -> &str { &self.mac }
    pub fn advertise(&self) -> io::Result<bool> {
        let status = Command::new("btmgmt").args(["--index", self.hci_device.trim_start_matches("hci"), "add-adv", "-d", &self.advertising_hex, "1"]).status()?;
        Ok(status.success())
    }
}
