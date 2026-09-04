use std::io;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UsbEventKind { Connect, Control, Reset, Suspend, Resume, Disconnect }

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UsbEvent { kind: UsbEventKind, payload: Vec<u8> }
impl UsbEvent {
    pub fn new(kind: UsbEventKind, payload: Vec<u8>) -> Self { Self { kind, payload } }
    pub fn kind(&self) -> UsbEventKind { self.kind }
    pub fn payload(&self) -> &[u8] { &self.payload }
}

pub trait RawGadgetTransport: Send {
    fn next_event(&mut self) -> io::Result<UsbEvent>;
    fn ep0_reply(&mut self, data: &[u8]) -> io::Result<()>;
    fn endpoint_read(&mut self, endpoint: u16, output: &mut [u8]) -> io::Result<usize>;
    fn endpoint_write(&mut self, endpoint: u16, data: &[u8]) -> io::Result<usize>;
}

#[derive(Clone, Debug)]
pub struct RawGadgetConfiguration { device_path: PathBuf, udc_driver: String, udc_device: String }
impl RawGadgetConfiguration {
    pub fn new(device_path: impl Into<PathBuf>, udc_driver: impl Into<String>, udc_device: impl Into<String>) -> Result<Self, String> {
        let configuration = Self { device_path: device_path.into(), udc_driver: udc_driver.into(), udc_device: udc_device.into() };
        if configuration.udc_driver.trim().is_empty() || configuration.udc_device.trim().is_empty() { return Err("UDC driver and device must be non-empty".to_string()); }
        Ok(configuration)
    }
    pub fn device_path(&self) -> &Path { &self.device_path }
    pub fn udc_driver(&self) -> &str { &self.udc_driver }
    pub fn udc_device(&self) -> &str { &self.udc_device }
}

pub fn raw_gadget_available(path: &Path) -> bool { path.exists() }
