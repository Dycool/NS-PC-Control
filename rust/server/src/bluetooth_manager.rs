use std::io;
use std::process::{Command, Output};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BluetoothDevice { address: String, name: String, connected: bool }

impl BluetoothDevice {
    pub fn address(&self) -> &str { &self.address }
    pub fn name(&self) -> &str { &self.name }
    pub fn connected(&self) -> bool { self.connected }
}

#[derive(Clone, Debug)]
pub struct BluetoothManager { command: String }
impl Default for BluetoothManager { fn default() -> Self { Self { command: "bluetoothctl".to_string() } } }

impl BluetoothManager {
    pub fn with_command(command: impl Into<String>) -> Self { Self { command: command.into() } }
    pub fn list_devices(&self) -> io::Result<Vec<BluetoothDevice>> {
        let output = self.run(["devices"])?;
        let text = String::from_utf8_lossy(&output.stdout);
        Ok(text.lines().filter_map(|line| {
            let mut parts = line.splitn(3, ' ');
            (parts.next()? == "Device").then(|| BluetoothDevice { address: parts.next().unwrap_or_default().to_string(), name: parts.next().unwrap_or_default().to_string(), connected: false })
        }).collect())
    }
    pub fn connect(&self, address: &str) -> io::Result<bool> { Ok(self.run(["connect", address])?.status.success()) }
    pub fn disconnect(&self, address: &str) -> io::Result<bool> { Ok(self.run(["disconnect", address])?.status.success()) }
    pub fn power_on(&self) -> io::Result<bool> { Ok(self.run(["power", "on"])?.status.success()) }
    fn run<const N: usize>(&self, arguments: [&str; N]) -> io::Result<Output> { Command::new(&self.command).args(arguments).output() }
}
