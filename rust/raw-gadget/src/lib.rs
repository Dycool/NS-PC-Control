//! SAFETY BOUNDARY: Linux USB Raw Gadget syscall wrapper.
//!
//! This crate is the *only* crate in the NS PC Control Rust dependency graph
//! permitted to contain `unsafe`. Its public API is safe and owns every buffer
//! passed to the kernel for the full duration of each ioctl. It deliberately
//! has no dependencies so the unsafe surface cannot spread transitively.

#![deny(unsafe_op_in_unsafe_fn)]

use std::fs::{File, OpenOptions};
use std::io;
use std::path::Path;
use std::sync::OnceLock;

const UDC_NAME_LENGTH_MAX: usize = 128;
const USB_RAW_EPS_NUM_MAX: usize = 30;
const USB_RAW_EP_NAME_MAX: usize = 16;
const USB_RAW_EP_INFO_SIZE: usize = 32;
const USB_RAW_EPS_INFO_SIZE: usize = USB_RAW_EPS_NUM_MAX * USB_RAW_EP_INFO_SIZE;
const USB_RAW_EVENT_HEADER_SIZE: usize = 8;
const USB_RAW_EP_IO_HEADER_SIZE: usize = 8;
const USB_ENDPOINT_DESCRIPTOR_SIZE: usize = 9;
const USB_RAW_INIT_SIZE: usize = UDC_NAME_LENGTH_MAX * 2 + 1;

const IOC_NRBITS: usize = 8;
const IOC_TYPEBITS: usize = 8;
const IOC_SIZEBITS: usize = 14;
const IOC_NRSHIFT: usize = 0;
const IOC_TYPESHIFT: usize = IOC_NRSHIFT + IOC_NRBITS;
const IOC_SIZESHIFT: usize = IOC_TYPESHIFT + IOC_TYPEBITS;
const IOC_DIRSHIFT: usize = IOC_SIZESHIFT + IOC_SIZEBITS;
const IOC_NONE: usize = 0;
const IOC_WRITE: usize = 1;
const IOC_READ: usize = 2;
const RAW_GADGET_IOC_MAGIC: usize = 0x55;

const fn ioc(direction: usize, number: usize, size: usize) -> usize {
    (direction << IOC_DIRSHIFT)
        | (RAW_GADGET_IOC_MAGIC << IOC_TYPESHIFT)
        | (number << IOC_NRSHIFT)
        | (size << IOC_SIZESHIFT)
}

const fn io(number: usize) -> usize {
    ioc(IOC_NONE, number, 0)
}

const fn iow(number: usize, size: usize) -> usize {
    ioc(IOC_WRITE, number, size)
}

const fn ior(number: usize, size: usize) -> usize {
    ioc(IOC_READ, number, size)
}

const fn iowr(number: usize, size: usize) -> usize {
    ioc(IOC_READ | IOC_WRITE, number, size)
}

const USB_RAW_IOCTL_INIT: usize = iow(0, USB_RAW_INIT_SIZE);
const USB_RAW_IOCTL_RUN: usize = io(1);
const USB_RAW_IOCTL_EVENT_FETCH: usize = ior(2, USB_RAW_EVENT_HEADER_SIZE);
const USB_RAW_IOCTL_EP0_WRITE: usize = iow(3, USB_RAW_EP_IO_HEADER_SIZE);
const USB_RAW_IOCTL_EP0_READ: usize = iowr(4, USB_RAW_EP_IO_HEADER_SIZE);
const USB_RAW_IOCTL_EP_ENABLE: usize = iow(5, USB_ENDPOINT_DESCRIPTOR_SIZE);
const USB_RAW_IOCTL_EP_DISABLE: usize = iow(6, size_of::<u32>());
const USB_RAW_IOCTL_EP_WRITE: usize = iow(7, USB_RAW_EP_IO_HEADER_SIZE);
const USB_RAW_IOCTL_EP_READ: usize = iowr(8, USB_RAW_EP_IO_HEADER_SIZE);
const USB_RAW_IOCTL_CONFIGURE: usize = io(9);
const USB_RAW_IOCTL_VBUS_DRAW: usize = iow(10, size_of::<u32>());
const USB_RAW_IOCTL_EPS_INFO: usize = ior(11, USB_RAW_EPS_INFO_SIZE);
const USB_RAW_IOCTL_EP0_STALL: usize = io(12);
const USB_RAW_IOCTL_EP_SET_HALT: usize = iow(13, size_of::<u32>());
const USB_RAW_IOCTL_EP_CLEAR_HALT: usize = iow(14, size_of::<u32>());
const USB_RAW_IOCTL_EP_SET_WEDGE: usize = iow(15, size_of::<u32>());

#[cfg(target_os = "linux")]
const SIGUSR1: i32 = 10;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum UsbSpeed {
    Low = 1,
    Full = 2,
    High = 3,
    Super = 4,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EventKind {
    Connect,
    Control,
    Suspend,
    Resume,
    Reset,
    Disconnect,
    Unknown(u32),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Event {
    kind: EventKind,
    data: Vec<u8>,
}

impl Event {
    #[must_use]
    pub const fn kind(&self) -> EventKind {
        self.kind
    }

    #[must_use]
    pub fn data(&self) -> &[u8] {
        &self.data
    }

    pub fn control_request(&self) -> io::Result<ControlRequest> {
        if self.kind != EventKind::Control {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "event is not a control request",
            ));
        }
        ControlRequest::decode(&self.data)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ControlRequest {
    request_type: u8,
    request: u8,
    value: u16,
    index: u16,
    length: u16,
}

impl ControlRequest {
    pub fn decode(bytes: &[u8]) -> io::Result<Self> {
        if bytes.len() < 8 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "USB control request is shorter than eight bytes",
            ));
        }
        Ok(Self {
            request_type: bytes[0],
            request: bytes[1],
            value: u16::from_le_bytes([bytes[2], bytes[3]]),
            index: u16::from_le_bytes([bytes[4], bytes[5]]),
            length: u16::from_le_bytes([bytes[6], bytes[7]]),
        })
    }

    #[must_use]
    pub const fn request_type(self) -> u8 {
        self.request_type
    }

    #[must_use]
    pub const fn request(self) -> u8 {
        self.request
    }

    #[must_use]
    pub const fn value(self) -> u16 {
        self.value
    }

    #[must_use]
    pub const fn index(self) -> u16 {
        self.index
    }

    #[must_use]
    pub const fn length(self) -> u16 {
        self.length
    }

    #[must_use]
    pub fn encode(self) -> [u8; 8] {
        let mut bytes = [0u8; 8];
        bytes[0] = self.request_type;
        bytes[1] = self.request;
        bytes[2..4].copy_from_slice(&self.value.to_le_bytes());
        bytes[4..6].copy_from_slice(&self.index.to_le_bytes());
        bytes[6..8].copy_from_slice(&self.length.to_le_bytes());
        bytes
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EndpointDescriptor {
    address: u8,
    attributes: u8,
    max_packet_size: u16,
    interval: u8,
}

impl EndpointDescriptor {
    #[must_use]
    pub const fn new(address: u8, attributes: u8, max_packet_size: u16, interval: u8) -> Self {
        Self {
            address,
            attributes,
            max_packet_size,
            interval,
        }
    }

    fn encode(self) -> [u8; USB_ENDPOINT_DESCRIPTOR_SIZE] {
        let mut bytes = [0u8; USB_ENDPOINT_DESCRIPTOR_SIZE];
        bytes[0] = 7;
        bytes[1] = 0x05;
        bytes[2] = self.address;
        bytes[3] = self.attributes;
        bytes[4..6].copy_from_slice(&self.max_packet_size.to_le_bytes());
        bytes[6] = self.interval;
        bytes
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EndpointHandle(i32);

impl EndpointHandle {
    #[must_use]
    pub const fn raw(self) -> i32 {
        self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct EndpointInfo {
    name: String,
    address: u32,
    capabilities: u32,
    max_packet_limit: u16,
    max_streams: u16,
}

impl EndpointInfo {
    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub const fn address(&self) -> u32 {
        self.address
    }

    #[must_use]
    pub const fn supports_control(&self) -> bool {
        self.capabilities & (1 << 0) != 0
    }

    #[must_use]
    pub const fn supports_isochronous(&self) -> bool {
        self.capabilities & (1 << 1) != 0
    }

    #[must_use]
    pub const fn supports_bulk(&self) -> bool {
        self.capabilities & (1 << 2) != 0
    }

    #[must_use]
    pub const fn supports_interrupt(&self) -> bool {
        self.capabilities & (1 << 3) != 0
    }

    #[must_use]
    pub const fn supports_in(&self) -> bool {
        self.capabilities & (1 << 4) != 0
    }

    #[must_use]
    pub const fn supports_out(&self) -> bool {
        self.capabilities & (1 << 5) != 0
    }

    #[must_use]
    pub const fn max_packet_limit(&self) -> u16 {
        self.max_packet_limit
    }

    #[must_use]
    pub const fn max_streams(&self) -> u16 {
        self.max_streams
    }
}

/// Opaque native-thread identity used only to interrupt blocking Raw Gadget ioctls.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ThreadInterruptToken(usize);

pub struct RawGadget {
    file: File,
}

impl RawGadget {
    pub fn open(
        path: impl AsRef<Path>,
        driver: &str,
        device: &str,
        speed: UsbSpeed,
    ) -> io::Result<Self> {
        if !cfg!(target_os = "linux") {
            return Err(unsupported());
        }
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        let gadget = Self { file };
        let init = encode_init(driver, device, speed)?;
        sys::ioctl_read_only(&gadget.file, USB_RAW_IOCTL_INIT, &init)?;
        sys::ioctl_none(&gadget.file, USB_RAW_IOCTL_RUN)?;
        Ok(gadget)
    }

    pub fn next_event(&self, maximum_payload: usize) -> io::Result<Event> {
        if maximum_payload > u32::MAX as usize {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Raw Gadget event buffer is too large",
            ));
        }
        let mut buffer = vec![0u8; USB_RAW_EVENT_HEADER_SIZE + maximum_payload];
        buffer[4..8].copy_from_slice(&(maximum_payload as u32).to_ne_bytes());
        sys::ioctl_in_out(
            &self.file,
            USB_RAW_IOCTL_EVENT_FETCH,
            &mut buffer,
        )?;
        let raw_kind = u32::from_ne_bytes(buffer[..4].try_into().expect("event type is four bytes"));
        let returned = u32::from_ne_bytes(buffer[4..8].try_into().expect("event length is four bytes")) as usize;
        if returned > maximum_payload {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "kernel returned an oversized Raw Gadget event",
            ));
        }
        let kind = match raw_kind {
            1 => EventKind::Connect,
            2 => EventKind::Control,
            3 => EventKind::Suspend,
            4 => EventKind::Resume,
            5 => EventKind::Reset,
            6 => EventKind::Disconnect,
            other => EventKind::Unknown(other),
        };
        Ok(Event {
            kind,
            data: buffer[USB_RAW_EVENT_HEADER_SIZE..USB_RAW_EVENT_HEADER_SIZE + returned].to_vec(),
        })
    }

    pub fn ep0_write(&self, data: &[u8]) -> io::Result<usize> {
        let buffer = encode_ep_io(0, data)?;
        sys::ioctl_read_only(&self.file, USB_RAW_IOCTL_EP0_WRITE, &buffer)
            .map(nonnegative_to_usize)
    }

    pub fn ep0_read(&self, length: usize) -> io::Result<Vec<u8>> {
        let mut buffer = empty_ep_io(0, length)?;
        let transferred = sys::ioctl_in_out(
            &self.file,
            USB_RAW_IOCTL_EP0_READ,
            &mut buffer,
        )?;
        decode_ep_io_payload(&buffer, transferred, length)
    }

    pub fn ep0_stall(&self) -> io::Result<()> {
        sys::ioctl_none(&self.file, USB_RAW_IOCTL_EP0_STALL).map(|_| ())
    }

    pub fn enable_endpoint(&self, descriptor: EndpointDescriptor) -> io::Result<EndpointHandle> {
        let bytes = descriptor.encode();
        let handle = sys::ioctl_read_only(
            &self.file,
            USB_RAW_IOCTL_EP_ENABLE,
            &bytes,
        )?;
        Ok(EndpointHandle(handle))
    }

    pub fn disable_endpoint(&self, endpoint: EndpointHandle) -> io::Result<()> {
        sys::ioctl_value(
            &self.file,
            USB_RAW_IOCTL_EP_DISABLE,
            endpoint.0 as usize,
        )
        .map(|_| ())
    }

    pub fn endpoint_write(&self, endpoint: EndpointHandle, data: &[u8]) -> io::Result<usize> {
        let buffer = encode_ep_io(endpoint.0, data)?;
        sys::ioctl_read_only(&self.file, USB_RAW_IOCTL_EP_WRITE, &buffer)
            .map(nonnegative_to_usize)
    }

    pub fn endpoint_read(&self, endpoint: EndpointHandle, length: usize) -> io::Result<Vec<u8>> {
        let mut buffer = empty_ep_io(endpoint.0, length)?;
        let transferred = sys::ioctl_in_out(
            &self.file,
            USB_RAW_IOCTL_EP_READ,
            &mut buffer,
        )?;
        decode_ep_io_payload(&buffer, transferred, length)
    }

    pub fn configure(&self) -> io::Result<()> {
        sys::ioctl_none(&self.file, USB_RAW_IOCTL_CONFIGURE).map(|_| ())
    }

    /// Set the bus-power limit in the kernel UAPI's 2 mA units.
    pub fn set_vbus_draw(&self, units_2ma: u32) -> io::Result<()> {
        sys::ioctl_value(
            &self.file,
            USB_RAW_IOCTL_VBUS_DRAW,
            units_2ma as usize,
        )
        .map(|_| ())
    }

    pub fn endpoint_info(&self) -> io::Result<Vec<EndpointInfo>> {
        let mut buffer = vec![0u8; USB_RAW_EPS_INFO_SIZE];
        let count = sys::ioctl_in_out(
            &self.file,
            USB_RAW_IOCTL_EPS_INFO,
            &mut buffer,
        )?;
        let count = nonnegative_to_usize(count).min(USB_RAW_EPS_NUM_MAX);
        let mut endpoints = Vec::with_capacity(count);
        for index in 0..count {
            let base = index * USB_RAW_EP_INFO_SIZE;
            let entry = &buffer[base..base + USB_RAW_EP_INFO_SIZE];
            let name_end = entry[..USB_RAW_EP_NAME_MAX]
                .iter()
                .position(|byte| *byte == 0)
                .unwrap_or(USB_RAW_EP_NAME_MAX);
            endpoints.push(EndpointInfo {
                name: String::from_utf8_lossy(&entry[..name_end]).into_owned(),
                address: u32::from_ne_bytes(entry[16..20].try_into().expect("endpoint address is four bytes")),
                capabilities: u32::from_ne_bytes(entry[20..24].try_into().expect("endpoint caps are four bytes")),
                max_packet_limit: u16::from_ne_bytes(entry[24..26].try_into().expect("maxpacket is two bytes")),
                max_streams: u16::from_ne_bytes(entry[26..28].try_into().expect("max streams is two bytes")),
            });
        }
        Ok(endpoints)
    }

    pub fn set_halt(&self, endpoint: EndpointHandle) -> io::Result<()> {
        self.endpoint_control(USB_RAW_IOCTL_EP_SET_HALT, endpoint)
    }

    pub fn clear_halt(&self, endpoint: EndpointHandle) -> io::Result<()> {
        self.endpoint_control(USB_RAW_IOCTL_EP_CLEAR_HALT, endpoint)
    }

    pub fn set_wedge(&self, endpoint: EndpointHandle) -> io::Result<()> {
        self.endpoint_control(USB_RAW_IOCTL_EP_SET_WEDGE, endpoint)
    }

    fn endpoint_control(&self, request: usize, endpoint: EndpointHandle) -> io::Result<()> {
        sys::ioctl_value(&self.file, request, endpoint.0 as usize).map(|_| ())
    }
}

pub fn install_interrupt_handler() -> io::Result<()> {
    static INSTALL: OnceLock<Result<(), i32>> = OnceLock::new();
    match *INSTALL.get_or_init(sys::install_interrupt_handler) {
        Ok(()) => Ok(()),
        Err(error) => Err(io::Error::from_raw_os_error(error)),
    }
}

#[must_use]
pub fn current_thread_interrupt_token() -> ThreadInterruptToken {
    ThreadInterruptToken(sys::current_thread())
}

pub fn interrupt_thread(token: ThreadInterruptToken) -> io::Result<()> {
    sys::interrupt_thread(token.0)
}

fn encode_init(driver: &str, device: &str, speed: UsbSpeed) -> io::Result<[u8; USB_RAW_INIT_SIZE]> {
    let driver = driver.as_bytes();
    let device = device.as_bytes();
    if driver.is_empty()
        || device.is_empty()
        || driver.len() >= UDC_NAME_LENGTH_MAX
        || device.len() >= UDC_NAME_LENGTH_MAX
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "UDC driver/device names must be 1..127 bytes",
        ));
    }
    let mut init = [0u8; USB_RAW_INIT_SIZE];
    init[..driver.len()].copy_from_slice(driver);
    init[UDC_NAME_LENGTH_MAX..UDC_NAME_LENGTH_MAX + device.len()].copy_from_slice(device);
    init[USB_RAW_INIT_SIZE - 1] = speed as u8;
    Ok(init)
}

fn encode_ep_io(endpoint: i32, data: &[u8]) -> io::Result<Vec<u8>> {
    let mut buffer = empty_ep_io(endpoint, data.len())?;
    buffer[USB_RAW_EP_IO_HEADER_SIZE..].copy_from_slice(data);
    Ok(buffer)
}

fn empty_ep_io(endpoint: i32, length: usize) -> io::Result<Vec<u8>> {
    if endpoint < 0 || endpoint > u16::MAX as i32 || length > u32::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "invalid Raw Gadget endpoint or transfer length",
        ));
    }
    let mut buffer = vec![0u8; USB_RAW_EP_IO_HEADER_SIZE + length];
    buffer[..2].copy_from_slice(&(endpoint as u16).to_ne_bytes());
    buffer[2..4].copy_from_slice(&0u16.to_ne_bytes());
    buffer[4..8].copy_from_slice(&(length as u32).to_ne_bytes());
    Ok(buffer)
}

fn decode_ep_io_payload(buffer: &[u8], transferred: i32, requested: usize) -> io::Result<Vec<u8>> {
    let transferred = nonnegative_to_usize(transferred);
    if transferred > requested || USB_RAW_EP_IO_HEADER_SIZE + transferred > buffer.len() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "kernel returned an oversized Raw Gadget transfer",
        ));
    }
    Ok(buffer[USB_RAW_EP_IO_HEADER_SIZE..USB_RAW_EP_IO_HEADER_SIZE + transferred].to_vec())
}

fn nonnegative_to_usize(value: i32) -> usize {
    usize::try_from(value).expect("successful ioctl result is non-negative")
}

fn unsupported() -> io::Error {
    io::Error::new(
        io::ErrorKind::Unsupported,
        "Linux USB Raw Gadget is unavailable on this platform",
    )
}

#[cfg(target_os = "linux")]
mod sys {
    use super::{io, File, SIGUSR1};
    use std::os::fd::AsRawFd;
    use std::os::raw::{c_int, c_ulong, c_void};

    unsafe extern "C" {
        fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
        fn pthread_self() -> usize;
        fn pthread_kill(thread: usize, signal: c_int) -> c_int;
        fn signal(signal: c_int, handler: usize) -> usize;
        fn siginterrupt(signal: c_int, flag: c_int) -> c_int;
    }

    extern "C" fn interrupt_handler(_: c_int) {}

    fn result(return_value: c_int) -> io::Result<c_int> {
        if return_value < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(return_value)
        }
    }

    pub fn ioctl_none(file: &File, request: usize) -> io::Result<c_int> {
        // SAFETY: Raw Gadget no-argument ioctls require an explicit zero third argument.
        result(unsafe { ioctl(file.as_raw_fd(), request as c_ulong, 0usize) })
    }

    pub fn ioctl_value(file: &File, request: usize, value: usize) -> io::Result<c_int> {
        // SAFETY: `value` is the scalar argument required by this Raw Gadget ioctl.
        result(unsafe { ioctl(file.as_raw_fd(), request as c_ulong, value) })
    }

    pub fn ioctl_read_only(file: &File, request: usize, bytes: &[u8]) -> io::Result<c_int> {
        let pointer = bytes.as_ptr().cast::<c_void>();
        // SAFETY: `bytes` is live and immutable for the full synchronous ioctl call.
        result(unsafe { ioctl(file.as_raw_fd(), request as c_ulong, pointer) })
    }

    pub fn ioctl_in_out(file: &File, request: usize, bytes: &mut [u8]) -> io::Result<c_int> {
        let pointer = bytes.as_mut_ptr().cast::<c_void>();
        // SAFETY: `bytes` is exclusively borrowed, writable, and live for the full synchronous ioctl call.
        result(unsafe { ioctl(file.as_raw_fd(), request as c_ulong, pointer) })
    }

    pub fn install_interrupt_handler() -> Result<(), i32> {
        // SAFETY: `interrupt_handler` has the C signal-handler ABI and is process-static.
        let previous = unsafe { signal(SIGUSR1, interrupt_handler as *const () as usize) };
        if previous == usize::MAX {
            return Err(io::Error::last_os_error().raw_os_error().unwrap_or(22));
        }
        // SAFETY: this configures SIGUSR1 so blocking Raw Gadget ioctls return EINTR instead of restarting.
        let result = unsafe { siginterrupt(SIGUSR1, 1) };
        if result == 0 {
            Ok(())
        } else {
            Err(io::Error::last_os_error().raw_os_error().unwrap_or(22))
        }
    }

    pub fn current_thread() -> usize {
        // SAFETY: pthread_self has no preconditions and returns the caller's thread identifier.
        unsafe { pthread_self() }
    }

    pub fn interrupt_thread(thread: usize) -> io::Result<()> {
        // SAFETY: the opaque token was produced by pthread_self in this process.
        let result = unsafe { pthread_kill(thread, SIGUSR1) };
        if result == 0 {
            Ok(())
        } else {
            Err(io::Error::from_raw_os_error(result))
        }
    }
}

#[cfg(not(target_os = "linux"))]
mod sys {
    use super::{unsupported, File};
    use std::io;

    pub fn ioctl_none(_: &File, _: usize) -> io::Result<i32> {
        Err(unsupported())
    }
    pub fn ioctl_value(_: &File, _: usize, _: usize) -> io::Result<i32> {
        Err(unsupported())
    }
    pub fn ioctl_read_only(_: &File, _: usize, _: &[u8]) -> io::Result<i32> {
        Err(unsupported())
    }
    pub fn ioctl_in_out(_: &File, _: usize, _: &mut [u8]) -> io::Result<i32> {
        Err(unsupported())
    }
    pub fn install_interrupt_handler() -> Result<(), i32> {
        Err(95)
    }
    pub const fn current_thread() -> usize {
        0
    }
    pub fn interrupt_thread(_: usize) -> io::Result<()> {
        Err(unsupported())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn unsupported_platform_rejects_open_before_accessing_the_device() {
        let error = RawGadget::open("not-a-device", "driver", "device", UsbSpeed::High)
            .err().expect("Raw Gadget is Linux-only");
        assert_eq!(error.kind(), io::ErrorKind::Unsupported);
    }

    #[test]
    fn ioctl_numbers_match_linux_uapi_on_generic_architectures() {
        assert_eq!(USB_RAW_IOCTL_INIT, 0x4101_5500);
        assert_eq!(USB_RAW_IOCTL_RUN, 0x0000_5501);
        assert_eq!(USB_RAW_IOCTL_EVENT_FETCH, 0x8008_5502);
        assert_eq!(USB_RAW_IOCTL_EP0_WRITE, 0x4008_5503);
        assert_eq!(USB_RAW_IOCTL_EP0_READ, 0xc008_5504);
        assert_eq!(USB_RAW_IOCTL_EP_ENABLE, 0x4009_5505);
        assert_eq!(USB_RAW_IOCTL_EP_DISABLE, 0x4004_5506);
        assert_eq!(USB_RAW_IOCTL_EP_WRITE, 0x4008_5507);
        assert_eq!(USB_RAW_IOCTL_EP_READ, 0xc008_5508);
        assert_eq!(USB_RAW_IOCTL_CONFIGURE, 0x0000_5509);
        assert_eq!(USB_RAW_IOCTL_VBUS_DRAW, 0x4004_550a);
        assert_eq!(USB_RAW_IOCTL_EP0_STALL, 0x0000_550c);
        assert_eq!(USB_RAW_IOCTL_EP_SET_HALT, 0x4004_550d);
        assert_eq!(USB_RAW_IOCTL_EP_CLEAR_HALT, 0x4004_550e);
        assert_eq!(USB_RAW_IOCTL_EP_SET_WEDGE, 0x4004_550f);
    }

    #[test]
    fn endpoint_descriptor_is_nine_bytes_with_zero_audio_extension() {
        let bytes = EndpointDescriptor::new(0x81, 0x03, 64, 4).encode();
        assert_eq!(bytes, [7, 5, 0x81, 3, 64, 0, 4, 0, 0]);
    }

    #[test]
    fn control_request_round_trip() {
        let request = ControlRequest {
            request_type: 0x81,
            request: 0x06,
            value: 0x2200,
            index: 0,
            length: 0x40,
        };
        assert_eq!(ControlRequest::decode(&request.encode()).expect("decode"), request);
    }
}
