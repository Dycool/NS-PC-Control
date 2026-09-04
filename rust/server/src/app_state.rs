use ns_shared::protocol::{ControllerType, HidReport, MultiReport, RumblePacket, EXT_PAD_PRESENT};
use std::array;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;

pub const CLIENT_TIMEOUT_US: u64 = 30_000_000;
pub const CLIENT_STALE_NEUTRAL_US: u64 = 350_000;
pub const JOYCON_MOUSE_TIMEOUT_US: u64 = 250_000;
pub const MAX_CLIENTS: usize = 4;
pub const HID_PORT_COUNT: usize = 4;
pub const RATE_WINDOW_US: u64 = 1_000_000;
pub const RATE_MAX_PACKETS: u32 = 2_000;
pub const SWITCH2_USB_SLEEP_CONFIRM_US: u64 = 10_000_000;
pub const SWITCH2_WAKE_CLIENT_GRACE_US: u64 = 30_000_000;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum InputSource {
    #[default]
    None,
    Udp,
    WebSocket,
    Bluetooth,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum UsbControllerFamily {
    #[default]
    Switch1,
    Switch2,
    Hori,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ControllerStatusState {
    player_leds: u8,
    player_index: u8,
    body_rgb: [u8; 3],
    body_rgb_valid: bool,
}

impl Default for ControllerStatusState {
    fn default() -> Self {
        Self { player_leds: 0, player_index: 0xff, body_rgb: [0; 3], body_rgb_valid: false }
    }
}

impl ControllerStatusState {
    pub fn player_leds(&self) -> u8 { self.player_leds }
    pub fn player_index(&self) -> u8 { self.player_index }
    pub fn body_rgb(&self) -> Option<[u8; 3]> { self.body_rgb_valid.then_some(self.body_rgb) }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClientAssignmentState {
    console_port_mask: u8,
    primary_console_port: u8,
    requested_type: ControllerType,
    virtual_type: ControllerType,
}

impl Default for ClientAssignmentState {
    fn default() -> Self {
        Self { console_port_mask: 0, primary_console_port: 0xff, requested_type: ControllerType::Default, virtual_type: ControllerType::Default }
    }
}

impl ClientAssignmentState {
    pub fn console_port_mask(&self) -> u8 { self.console_port_mask }
    pub fn primary_console_port(&self) -> u8 { self.primary_console_port }
    pub fn requested_type(&self) -> ControllerType { self.requested_type }
    pub fn virtual_type(&self) -> ControllerType { self.virtual_type }
}

#[derive(Clone, Debug)]
struct ClientSession {
    active: bool,
    source: InputSource,
    address: Option<SocketAddr>,
    connected_us: u64,
    last_rx_us: u64,
    expected_sequence: u32,
    first_packet: bool,
    report: MultiReport,
    report_generation: u64,
    rumble: [RumblePacket; 4],
    rumble_sequence: [u32; 4],
    controller_status: [ControllerStatusState; 4],
    assignments: [ClientAssignmentState; 4],
    pad_present: [bool; 4],
    pad_last_present_us: [u64; 4],
}

impl Default for ClientSession {
    fn default() -> Self {
        Self {
            active: false,
            source: InputSource::None,
            address: None,
            connected_us: 0,
            last_rx_us: 0,
            expected_sequence: 0,
            first_packet: true,
            report: MultiReport::default(),
            report_generation: 0,
            rumble: [RumblePacket::default(); 4],
            rumble_sequence: [0; 4],
            controller_status: [ControllerStatusState::default(); 4],
            assignments: [ClientAssignmentState::default(); 4],
            pad_present: [false; 4],
            pad_last_present_us: [0; 4],
        }
    }
}

impl ClientSession {
    fn reset(&mut self) { *self = Self::default(); }

    fn is_recent(&self, now_us: u64) -> bool {
        self.active && now_us.saturating_sub(self.last_rx_us) <= CLIENT_TIMEOUT_US
    }

    fn accepts_sequence(&mut self, sequence: u32) -> bool {
        if self.first_packet {
            self.first_packet = false;
            self.expected_sequence = sequence.wrapping_add(1);
            return true;
        }
        let delta = sequence.wrapping_sub(self.expected_sequence);
        if delta < 0x8000_0000 {
            self.expected_sequence = sequence.wrapping_add(1);
            true
        } else {
            false
        }
    }

    fn update_presence(&mut self, now_us: u64) {
        for (index, report) in self.report.pads().iter().enumerate() {
            let present = report.input().vendor() & EXT_PAD_PRESENT != 0;
            self.pad_present[index] = present;
            if present { self.pad_last_present_us[index] = now_us; }
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ClientSnapshot {
    active: bool,
    source: InputSource,
    report: MultiReport,
    generation: u64,
    pad_present: [bool; 4],
    pad_last_present_us: [u64; 4],
    connected_us: u64,
}

impl ClientSnapshot {
    pub fn active(&self) -> bool { self.active }
    pub fn source(&self) -> InputSource { self.source }
    pub fn report(&self) -> &MultiReport { &self.report }
    pub fn generation(&self) -> u64 { self.generation }
    pub fn pad_present(&self) -> [bool; 4] { self.pad_present }
    pub fn pad_last_present_us(&self) -> [u64; 4] { self.pad_last_present_us }
    pub fn connected_us(&self) -> u64 { self.connected_us }
}

#[derive(Debug)]
pub enum SessionError { ServerFull, UnknownSession, StaleSequence }

pub struct ServerContext {
    running: AtomicBool,
    family: Mutex<UsbControllerFamily>,
    clients: Mutex<[ClientSession; MAX_CLIENTS]>,
    packets_received: AtomicU64,
    hid_writes: AtomicU64,
    switch2_last_usb_activity_us: AtomicU64,
    switch2_host_suspended: AtomicBool,
    switch2_sleep_confirmed: AtomicBool,
}

impl Default for ServerContext {
    fn default() -> Self {
        Self {
            running: AtomicBool::new(true),
            family: Mutex::new(UsbControllerFamily::Switch1),
            clients: Mutex::new(array::from_fn(|_| ClientSession::default())),
            packets_received: AtomicU64::new(0),
            hid_writes: AtomicU64::new(0),
            switch2_last_usb_activity_us: AtomicU64::new(0),
            switch2_host_suspended: AtomicBool::new(false),
            switch2_sleep_confirmed: AtomicBool::new(false),
        }
    }
}

impl ServerContext {
    pub fn is_running(&self) -> bool { self.running.load(Ordering::Acquire) }
    pub fn stop(&self) { self.running.store(false, Ordering::Release); }
    pub fn family(&self) -> UsbControllerFamily { *self.family.lock().unwrap_or_else(|poison| poison.into_inner()) }

    pub fn set_family(&self, family: UsbControllerFamily, now_us: u64) -> Result<(), SessionError> {
        if self.active_client_count(now_us) != 0 { return Err(SessionError::ServerFull); }
        *self.family.lock().unwrap_or_else(|poison| poison.into_inner()) = family;
        Ok(())
    }

    pub fn register_udp(&self, address: SocketAddr, now_us: u64) -> Result<usize, SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        if let Some((index, client)) = clients.iter_mut().enumerate().find(|(_, client)| client.active && client.address == Some(address)) {
            client.last_rx_us = now_us;
            return Ok(index);
        }
        if let Some((index, client)) = clients.iter_mut().enumerate().find(|(_, client)| !client.is_recent(now_us)) {
            client.reset();
            client.active = true;
            client.source = InputSource::Udp;
            client.address = Some(address);
            client.connected_us = now_us;
            client.last_rx_us = now_us;
            return Ok(index);
        }
        Err(SessionError::ServerFull)
    }

    pub fn update_udp_report(&self, index: usize, sequence: u32, report: MultiReport, now_us: u64) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        if !client.active || client.source != InputSource::Udp { return Err(SessionError::UnknownSession); }
        if !client.accepts_sequence(sequence) { return Err(SessionError::StaleSequence); }
        client.report = report;
        client.last_rx_us = now_us;
        client.report_generation = client.report_generation.wrapping_add(1);
        client.update_presence(now_us);
        self.packets_received.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    pub fn disconnect(&self, index: usize) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        client.reset();
        Ok(())
    }

    pub fn expire_stale_clients(&self, now_us: u64) {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        for client in clients.iter_mut() {
            if client.active && !client.is_recent(now_us) {
                client.reset();
            } else if client.active && now_us.saturating_sub(client.last_rx_us) > CLIENT_STALE_NEUTRAL_US {
                client.report.reset();
                client.pad_present.fill(false);
            }
        }
    }

    pub fn active_client_count(&self, now_us: u64) -> usize {
        self.clients.lock().unwrap_or_else(|poison| poison.into_inner()).iter().filter(|client| client.is_recent(now_us)).count()
    }

    pub fn snapshot(&self, index: usize, now_us: u64) -> Option<ClientSnapshot> {
        let clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        clients.get(index).map(|client| ClientSnapshot {
            active: client.is_recent(now_us), source: client.source, report: client.report,
            generation: client.report_generation, pad_present: client.pad_present,
            pad_last_present_us: client.pad_last_present_us, connected_us: client.connected_us,
        })
    }

    pub fn publish_rumble(&self, index: usize, subpad: usize, rumble: RumblePacket) -> Result<u32, SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        let slot = client.rumble.get_mut(subpad).ok_or(SessionError::UnknownSession)?;
        *slot = rumble;
        client.rumble_sequence[subpad] = client.rumble_sequence[subpad].wrapping_add(1);
        Ok(client.rumble_sequence[subpad])
    }

    pub fn publish_controller_status(&self, index: usize, subpad: usize, player_leds: u8, body_rgb: Option<[u8; 3]>) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        let status = client.controller_status.get_mut(subpad).ok_or(SessionError::UnknownSession)?;
        status.player_leds = player_leds;
        status.player_index = switch_player_index_from_leds(player_leds);
        if let Some(rgb) = body_rgb { status.body_rgb = rgb; status.body_rgb_valid = true; }
        Ok(())
    }

    pub fn controller_status(&self, index: usize, subpad: usize) -> Option<ControllerStatusState> {
        self.clients.lock().unwrap_or_else(|poison| poison.into_inner()).get(index).and_then(|client| client.controller_status.get(subpad)).copied()
    }

    pub fn publish_assignment(&self, index: usize, subpad: usize, assignment: ClientAssignmentState) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        let slot = client.assignments.get_mut(subpad).ok_or(SessionError::UnknownSession)?;
        *slot = assignment;
        Ok(())
    }

    pub fn assignment(&self, index: usize, subpad: usize) -> Option<ClientAssignmentState> {
        self.clients.lock().unwrap_or_else(|poison| poison.into_inner()).get(index).and_then(|client| client.assignments.get(subpad)).copied()
    }

    pub fn record_hid_write(&self) { self.hid_writes.fetch_add(1, Ordering::Relaxed); }
    pub fn counters(&self) -> (u64, u64) { (self.packets_received.load(Ordering::Relaxed), self.hid_writes.load(Ordering::Relaxed)) }

    pub fn mark_switch2_usb_activity(&self, now_us: u64) {
        self.switch2_last_usb_activity_us.store(now_us, Ordering::Release);
        self.switch2_host_suspended.store(false, Ordering::Release);
        self.switch2_sleep_confirmed.store(false, Ordering::Release);
    }
    pub fn mark_switch2_suspended(&self) { self.switch2_host_suspended.store(true, Ordering::Release); }
    pub fn poll_switch2_sleep(&self, now_us: u64) -> bool {
        let inactive_since = self.switch2_last_usb_activity_us.load(Ordering::Acquire);
        let sleeping = self.switch2_host_suspended.load(Ordering::Acquire)
            && inactive_since != 0
            && now_us.saturating_sub(inactive_since) >= SWITCH2_USB_SLEEP_CONFIRM_US;
        self.switch2_sleep_confirmed.store(sleeping, Ordering::Release);
        sleeping
    }
}

pub fn switch_player_index_from_leds(player_leds: u8) -> u8 {
    match player_leds & 0x0f { 0x01 => 0, 0x03 => 1, 0x07 => 2, 0x0f => 3, _ => 0xff }
}

pub fn controller_profile_supported(family: UsbControllerFamily, profile: ControllerType) -> bool {
    match family {
        UsbControllerFamily::Switch2 => matches!(profile, ControllerType::Default | ControllerType::ProS2 | ControllerType::JoyconLS2 | ControllerType::JoyconRS2),
        UsbControllerFamily::Hori => matches!(profile, ControllerType::Default | ControllerType::Hori),
        UsbControllerFamily::Switch1 => matches!(profile, ControllerType::Default | ControllerType::Pro | ControllerType::JoyconL | ControllerType::JoyconR | ControllerType::JoyconPair),
    }
}

pub fn requested_virtual_slots(report: &HidReport, present: bool) -> usize {
    if !present { return 0; }
    match report.controller_type().unwrap_or(ControllerType::Default) {
        ControllerType::JoyconPair | ControllerType::JoyconPairS2 => 2,
        _ => 1,
    }
}

#[cfg(test)]
mod tests {
    use super::{controller_profile_supported, switch_player_index_from_leds, ServerContext, SessionError, UsbControllerFamily};
    use ns_shared::protocol::{ControllerType, MultiReport};
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    #[test]
    fn allocates_reuses_and_expires_udp_sessions() {
        let context = ServerContext::default();
        let address = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 7331);
        let slot = context.register_udp(address, 1_000).expect("slot");
        assert_eq!(context.register_udp(address, 2_000).expect("same slot"), slot);
        context.update_udp_report(slot, 1, MultiReport::default(), 2_000).expect("report");
        assert_eq!(context.active_client_count(2_000), 1);
        context.expire_stale_clients(31_000_001);
        assert_eq!(context.active_client_count(31_000_001), 0);
    }

    #[test]
    fn family_change_fails_with_active_clients() {
        let context = ServerContext::default();
        let address = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 7331);
        context.register_udp(address, 100).expect("slot");
        assert!(matches!(context.set_family(UsbControllerFamily::Switch2, 100), Err(SessionError::ServerFull)));
    }

    #[test]
    fn profile_and_led_rules_match_existing_server() {
        assert!(controller_profile_supported(UsbControllerFamily::Switch2, ControllerType::ProS2));
        assert!(!controller_profile_supported(UsbControllerFamily::Switch2, ControllerType::JoyconPairS2));
        assert_eq!(switch_player_index_from_leds(0x07), 2);
        assert_eq!(switch_player_index_from_leds(0x02), 0xff);
    }
}
