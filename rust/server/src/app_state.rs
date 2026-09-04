use ns_shared::control_packets::{
    AmiiboDataPacket, AmiiboRequestPacket, ClientAssignmentPacket, ControllerStatusPacket,
    RosterEntry, RosterPacket,
};
use ns_shared::protocol::{
    is_supported_amiibo_dump_size, ControllerType, HidReport, MultiReport, RumblePacket,
    CLIENT_ASSIGNMENT_FLAG_ACCEPTED, CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID,
    CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP, CONTROLLER_STATUS_FLAG_BODY_RGB_VALID,
    EXT_PAD_PRESENT,
};
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
const ROSTER_RESEND_US: u64 = 2_000_000;

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
        Self {
            player_leds: 0,
            player_index: 0xff,
            body_rgb: [0; 3],
            body_rgb_valid: false,
        }
    }
}

impl ControllerStatusState {
    #[must_use]
    pub const fn player_leds(&self) -> u8 {
        self.player_leds
    }

    #[must_use]
    pub const fn player_index(&self) -> u8 {
        self.player_index
    }

    #[must_use]
    pub const fn body_rgb(&self) -> Option<[u8; 3]> {
        if self.body_rgb_valid {
            Some(self.body_rgb)
        } else {
            None
        }
    }
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
        Self {
            console_port_mask: 0,
            primary_console_port: 0xff,
            requested_type: ControllerType::Default,
            virtual_type: ControllerType::Default,
        }
    }
}

impl ClientAssignmentState {
    #[must_use]
    pub const fn new(
        console_port_mask: u8,
        primary_console_port: u8,
        requested_type: ControllerType,
        virtual_type: ControllerType,
    ) -> Self {
        Self {
            console_port_mask,
            primary_console_port,
            requested_type,
            virtual_type,
        }
    }

    #[must_use]
    pub const fn console_port_mask(&self) -> u8 {
        self.console_port_mask
    }

    #[must_use]
    pub const fn primary_console_port(&self) -> u8 {
        self.primary_console_port
    }

    #[must_use]
    pub const fn requested_type(&self) -> ControllerType {
        self.requested_type
    }

    #[must_use]
    pub const fn virtual_type(&self) -> ControllerType {
        self.virtual_type
    }
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
    fn reset(&mut self) {
        *self = Self::default();
    }

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
            if present {
                self.pad_last_present_us[index] = now_us;
            }
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
    #[must_use]
    pub const fn active(&self) -> bool {
        self.active
    }

    #[must_use]
    pub const fn source(&self) -> InputSource {
        self.source
    }

    #[must_use]
    pub const fn report(&self) -> &MultiReport {
        &self.report
    }

    #[must_use]
    pub const fn generation(&self) -> u64 {
        self.generation
    }

    #[must_use]
    pub const fn pad_present(&self) -> [bool; 4] {
        self.pad_present
    }

    #[must_use]
    pub const fn pad_last_present_us(&self) -> [u64; 4] {
        self.pad_last_present_us
    }

    #[must_use]
    pub const fn connected_us(&self) -> u64 {
        self.connected_us
    }
}

#[derive(Debug)]
pub enum SessionError {
    ServerFull,
    UnknownSession,
    StaleSequence,
    InvalidAmiibo,
}

struct FeedbackState {
    controller_status_sequence: [[u32; 4]; MAX_CLIENTS],
    assignment_sequence: [[u32; 4]; MAX_CLIENTS],
    udp_last_controller_status_sequence: [[u32; 4]; MAX_CLIENTS],
    udp_last_assignment_sequence: [[u32; 4]; MAX_CLIENTS],
    udp_last_rumble_sequence: [[u32; 4]; MAX_CLIENTS],
    udp_enabled: [bool; MAX_CLIENTS],
    source_pads: [[RosterEntry; 4]; MAX_CLIENTS],
    roster: [RosterEntry; HID_PORT_COUNT],
    roster_sequence: u64,
    udp_last_roster_sequence: [u64; MAX_CLIENTS],
    udp_last_roster_send_us: [u64; MAX_CLIENTS],
    server_state_packed: Option<(u8, u8, bool)>,
    server_state_sequence: u64,
    udp_last_server_state_sequence: [u64; MAX_CLIENTS],
    amiibo_request_pending: [[bool; 4]; MAX_CLIENTS],
    amiibo_requested: [[bool; 4]; MAX_CLIENTS],
    amiibo_request_repeats: [[u8; 4]; MAX_CLIENTS],
    amiibo_request_sequence: [[u16; 4]; MAX_CLIENTS],
    amiibo_writeback: [[Option<Vec<u8>>; 4]; MAX_CLIENTS],
}

impl Default for FeedbackState {
    fn default() -> Self {
        Self {
            controller_status_sequence: [[0; 4]; MAX_CLIENTS],
            assignment_sequence: [[0; 4]; MAX_CLIENTS],
            udp_last_controller_status_sequence: [[0; 4]; MAX_CLIENTS],
            udp_last_assignment_sequence: [[0; 4]; MAX_CLIENTS],
            udp_last_rumble_sequence: [[0; 4]; MAX_CLIENTS],
            udp_enabled: [false; MAX_CLIENTS],
            source_pads: array::from_fn(|_| array::from_fn(|_| RosterEntry::default())),
            roster: array::from_fn(|_| RosterEntry::default()),
            roster_sequence: 1,
            udp_last_roster_sequence: [0; MAX_CLIENTS],
            udp_last_roster_send_us: [0; MAX_CLIENTS],
            server_state_packed: None,
            server_state_sequence: 1,
            udp_last_server_state_sequence: [0; MAX_CLIENTS],
            amiibo_request_pending: [[false; 4]; MAX_CLIENTS],
            amiibo_requested: [[false; 4]; MAX_CLIENTS],
            amiibo_request_repeats: [[0; 4]; MAX_CLIENTS],
            amiibo_request_sequence: [[0; 4]; MAX_CLIENTS],
            amiibo_writeback: array::from_fn(|_| array::from_fn(|_| None)),
        }
    }
}

#[derive(Clone, Debug)]
pub struct UdpFeedbackBatch {
    target: SocketAddr,
    rumble: Vec<RumblePacket>,
    assignments: Vec<ClientAssignmentPacket>,
    controller_status: Vec<ControllerStatusPacket>,
    roster: Option<RosterPacket>,
    amiibo_requests: Vec<AmiiboRequestPacket>,
    amiibo_data: Vec<AmiiboDataPacket>,
}

impl UdpFeedbackBatch {
    #[must_use]
    pub const fn target(&self) -> SocketAddr {
        self.target
    }

    #[must_use]
    pub fn rumble(&self) -> &[RumblePacket] {
        &self.rumble
    }

    #[must_use]
    pub fn assignments(&self) -> &[ClientAssignmentPacket] {
        &self.assignments
    }

    #[must_use]
    pub fn controller_status(&self) -> &[ControllerStatusPacket] {
        &self.controller_status
    }

    #[must_use]
    pub const fn roster(&self) -> Option<&RosterPacket> {
        self.roster.as_ref()
    }

    #[must_use]
    pub fn amiibo_requests(&self) -> &[AmiiboRequestPacket] {
        &self.amiibo_requests
    }

    #[must_use]
    pub fn amiibo_data(&self) -> &[AmiiboDataPacket] {
        &self.amiibo_data
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.rumble.is_empty()
            && self.assignments.is_empty()
            && self.controller_status.is_empty()
            && self.roster.is_none()
            && self.amiibo_requests.is_empty()
            && self.amiibo_data.is_empty()
    }
}

pub struct ServerContext {
    running: AtomicBool,
    family: Mutex<UsbControllerFamily>,
    clients: Mutex<[ClientSession; MAX_CLIENTS]>,
    feedback: Mutex<FeedbackState>,
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
            feedback: Mutex::new(FeedbackState::default()),
            packets_received: AtomicU64::new(0),
            hid_writes: AtomicU64::new(0),
            switch2_last_usb_activity_us: AtomicU64::new(0),
            switch2_host_suspended: AtomicBool::new(false),
            switch2_sleep_confirmed: AtomicBool::new(false),
        }
    }
}

impl ServerContext {
    #[must_use]
    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::Acquire)
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::Release);
    }

    #[must_use]
    pub fn family(&self) -> UsbControllerFamily {
        *self.family.lock().unwrap_or_else(|poison| poison.into_inner())
    }

    pub fn set_family(&self, family: UsbControllerFamily, now_us: u64) -> Result<(), SessionError> {
        if self.active_client_count(now_us) != 0 {
            return Err(SessionError::ServerFull);
        }
        *self.family.lock().unwrap_or_else(|poison| poison.into_inner()) = family;
        Ok(())
    }

    pub fn register_udp(&self, address: SocketAddr, now_us: u64) -> Result<usize, SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        if let Some((index, client)) = clients
            .iter_mut()
            .enumerate()
            .find(|(_, client)| client.active && client.address == Some(address))
        {
            client.last_rx_us = now_us;
            return Ok(index);
        }
        if let Some((index, client)) = clients
            .iter_mut()
            .enumerate()
            .find(|(_, client)| !client.is_recent(now_us))
        {
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

    pub fn enable_udp_feedback(&self, index: usize) -> Result<(), SessionError> {
        let clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get(index).ok_or(SessionError::UnknownSession)?;
        if !client.active || client.source != InputSource::Udp {
            return Err(SessionError::UnknownSession);
        }
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        feedback.udp_enabled[index] = true;
        feedback.udp_last_rumble_sequence[index] = client.rumble_sequence;
        Ok(())
    }

    pub fn update_udp_report(
        &self,
        index: usize,
        sequence: u32,
        report: MultiReport,
        now_us: u64,
    ) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        if !client.active || client.source != InputSource::Udp {
            return Err(SessionError::UnknownSession);
        }
        if !client.accepts_sequence(sequence) {
            return Err(SessionError::StaleSequence);
        }
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
        drop(clients);
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        feedback.udp_enabled[index] = false;
        feedback.amiibo_request_pending[index].fill(false);
        feedback.amiibo_request_repeats[index].fill(0);
        feedback.amiibo_writeback[index] = array::from_fn(|_| None);
        Ok(())
    }

    pub fn expire_stale_clients(&self, now_us: u64) {
        let mut expired = [false; MAX_CLIENTS];
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        for (index, client) in clients.iter_mut().enumerate() {
            if client.active && !client.is_recent(now_us) {
                client.reset();
                expired[index] = true;
            } else if client.active
                && now_us.saturating_sub(client.last_rx_us) > CLIENT_STALE_NEUTRAL_US
            {
                client.report.reset();
                client.pad_present.fill(false);
            }
        }
        drop(clients);
        if expired.iter().any(|value| *value) {
            let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
            for (index, did_expire) in expired.into_iter().enumerate() {
                if did_expire {
                    feedback.udp_enabled[index] = false;
                    feedback.amiibo_request_pending[index].fill(false);
                    feedback.amiibo_request_repeats[index].fill(0);
                    feedback.amiibo_writeback[index] = array::from_fn(|_| None);
                }
            }
        }
    }

    #[must_use]
    pub fn active_client_count(&self, now_us: u64) -> usize {
        self.clients
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .iter()
            .filter(|client| client.is_recent(now_us))
            .count()
    }

    #[must_use]
    pub fn snapshot(&self, index: usize, now_us: u64) -> Option<ClientSnapshot> {
        let clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        clients.get(index).map(|client| ClientSnapshot {
            active: client.is_recent(now_us),
            source: client.source,
            report: client.report,
            generation: client.report_generation,
            pad_present: client.pad_present,
            pad_last_present_us: client.pad_last_present_us,
            connected_us: client.connected_us,
        })
    }

    pub fn publish_rumble(
        &self,
        index: usize,
        subpad: usize,
        rumble: RumblePacket,
    ) -> Result<u32, SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        let slot = client.rumble.get_mut(subpad).ok_or(SessionError::UnknownSession)?;
        *slot = rumble;
        client.rumble_sequence[subpad] = client.rumble_sequence[subpad].wrapping_add(1);
        Ok(client.rumble_sequence[subpad])
    }

    pub fn publish_controller_status(
        &self,
        index: usize,
        subpad: usize,
        player_leds: u8,
        body_rgb: Option<[u8; 3]>,
    ) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        let status = client
            .controller_status
            .get_mut(subpad)
            .ok_or(SessionError::UnknownSession)?;
        status.player_leds = player_leds;
        status.player_index = switch_player_index_from_leds(player_leds);
        if let Some(rgb) = body_rgb {
            status.body_rgb = rgb;
            status.body_rgb_valid = true;
        }
        drop(clients);
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        let sequence = feedback
            .controller_status_sequence
            .get_mut(index)
            .and_then(|slots| slots.get_mut(subpad))
            .ok_or(SessionError::UnknownSession)?;
        *sequence = sequence.wrapping_add(1);
        Ok(())
    }

    #[must_use]
    pub fn controller_status(&self, index: usize, subpad: usize) -> Option<ControllerStatusState> {
        self.clients
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .get(index)
            .and_then(|client| client.controller_status.get(subpad))
            .copied()
    }

    pub fn publish_assignment(
        &self,
        index: usize,
        subpad: usize,
        assignment: ClientAssignmentState,
    ) -> Result<(), SessionError> {
        let mut clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let client = clients.get_mut(index).ok_or(SessionError::UnknownSession)?;
        let slot = client
            .assignments
            .get_mut(subpad)
            .ok_or(SessionError::UnknownSession)?;
        *slot = assignment;
        drop(clients);
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        let sequence = feedback
            .assignment_sequence
            .get_mut(index)
            .and_then(|slots| slots.get_mut(subpad))
            .ok_or(SessionError::UnknownSession)?;
        *sequence = sequence.wrapping_add(1);
        Ok(())
    }

    #[must_use]
    pub fn assignment(&self, index: usize, subpad: usize) -> Option<ClientAssignmentState> {
        self.clients
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .get(index)
            .and_then(|client| client.assignments.get(subpad))
            .copied()
    }

    pub fn store_client_source_names(
        &self,
        index: usize,
        pads: [RosterEntry; 4],
    ) -> Result<(), SessionError> {
        if index >= MAX_CLIENTS {
            return Err(SessionError::UnknownSession);
        }
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        feedback.source_pads[index] = pads;
        feedback.roster_sequence = feedback.roster_sequence.wrapping_add(1).max(1);
        Ok(())
    }

    pub fn publish_amiibo_request(
        &self,
        index: usize,
        subpad: usize,
        requested: bool,
    ) -> Result<u16, SessionError> {
        if index >= MAX_CLIENTS || subpad >= 4 {
            return Err(SessionError::UnknownSession);
        }
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        feedback.amiibo_requested[index][subpad] = requested;
        feedback.amiibo_request_pending[index][subpad] = true;
        feedback.amiibo_request_repeats[index][subpad] = 3;
        let mut sequence = feedback.amiibo_request_sequence[index][subpad].wrapping_add(1);
        if sequence == 0 {
            sequence = 1;
        }
        feedback.amiibo_request_sequence[index][subpad] = sequence;
        Ok(sequence)
    }

    pub fn publish_amiibo_writeback(
        &self,
        index: usize,
        subpad: usize,
        data: &[u8],
    ) -> Result<(), SessionError> {
        if index >= MAX_CLIENTS || subpad >= 4 {
            return Err(SessionError::UnknownSession);
        }
        if !is_supported_amiibo_dump_size(data.len()) {
            return Err(SessionError::InvalidAmiibo);
        }
        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        feedback.amiibo_writeback[index][subpad] = Some(data.to_vec());
        Ok(())
    }

    #[must_use]
    pub fn take_udp_feedback(&self, index: usize, now_us: u64) -> Option<UdpFeedbackBatch> {
        let family = self.family();
        let sleeping = self.switch2_sleep_confirmed.load(Ordering::Acquire);
        let clients = self.clients.lock().unwrap_or_else(|poison| poison.into_inner());
        let active_clients = clients.iter().filter(|client| client.is_recent(now_us)).count();
        let virtual_ports = if family == UsbControllerFamily::Switch2 { 1 } else { HID_PORT_COUNT };
        let used_mask = clients
            .iter()
            .filter(|client| client.is_recent(now_us))
            .flat_map(|client| client.assignments)
            .fold(0_u8, |mask, assignment| mask | assignment.console_port_mask);
        let available_mask = if virtual_ports == 1 {
            0x01
        } else {
            (1_u8 << virtual_ports) - 1
        };
        let used_slots = (used_mask & available_mask).count_ones() as usize;
        let free_slots = virtual_ports.saturating_sub(used_slots);
        let client = clients.get(index)?;
        if !client.is_recent(now_us) || client.source != InputSource::Udp {
            return None;
        }
        let target = client.address?;

        let mut feedback = self.feedback.lock().unwrap_or_else(|poison| poison.into_inner());
        if !feedback.udp_enabled[index] {
            return None;
        }

        let active_clients_u8 = u8::try_from(active_clients).unwrap_or(u8::MAX);
        let free_slots_u8 = u8::try_from(free_slots).unwrap_or(u8::MAX);
        let state = (active_clients_u8, free_slots_u8, sleeping);
        if feedback.server_state_packed != Some(state) {
            feedback.server_state_packed = Some(state);
            feedback.server_state_sequence = feedback.server_state_sequence.wrapping_add(1).max(1);
        }

        let mut roster = array::from_fn(|_| RosterEntry::default());
        for source_index in 0..MAX_CLIENTS {
            let source_client = &clients[source_index];
            if !source_client.is_recent(now_us) {
                continue;
            }
            for subpad in 0..4 {
                let entry = feedback.source_pads[source_index][subpad];
                let port = usize::from(source_client.assignments[subpad].primary_console_port);
                if entry.present() && port < virtual_ports {
                    roster[port] = entry;
                }
            }
        }
        if feedback.roster != roster {
            feedback.roster = roster;
            feedback.roster_sequence = feedback.roster_sequence.wrapping_add(1).max(1);
        }

        let mut batch = UdpFeedbackBatch {
            target,
            rumble: Vec::new(),
            assignments: Vec::new(),
            controller_status: Vec::new(),
            roster: None,
            amiibo_requests: Vec::new(),
            amiibo_data: Vec::new(),
        };
        let mut sent_assignment = false;

        for subpad in 0..4 {
            if client.rumble_sequence[subpad] != feedback.udp_last_rumble_sequence[index][subpad] {
                batch.rumble.push(client.rumble[subpad]);
                feedback.udp_last_rumble_sequence[index][subpad] = client.rumble_sequence[subpad];
            }

            let assignment_sequence = feedback.assignment_sequence[index][subpad];
            if assignment_sequence != feedback.udp_last_assignment_sequence[index][subpad] {
                batch.assignments.push(make_assignment_packet(
                    index,
                    subpad,
                    client.assignments[subpad],
                    active_clients_u8,
                    free_slots_u8,
                    sleeping,
                ));
                feedback.udp_last_assignment_sequence[index][subpad] = assignment_sequence;
                sent_assignment = true;
            }

            let status_sequence = feedback.controller_status_sequence[index][subpad];
            if status_sequence != feedback.udp_last_controller_status_sequence[index][subpad] {
                batch.controller_status.push(make_status_packet(
                    subpad,
                    client.controller_status[subpad],
                ));
                feedback.udp_last_controller_status_sequence[index][subpad] = status_sequence;
            }

            if feedback.amiibo_request_pending[index][subpad] {
                batch.amiibo_requests.push(AmiiboRequestPacket::new(
                    subpad as u8,
                    feedback.amiibo_requested[index][subpad],
                    feedback.amiibo_request_sequence[index][subpad],
                ));
                feedback.amiibo_request_repeats[index][subpad] =
                    feedback.amiibo_request_repeats[index][subpad].saturating_sub(1);
                feedback.amiibo_request_pending[index][subpad] =
                    feedback.amiibo_request_repeats[index][subpad] != 0;
            }

            if let Some(data) = feedback.amiibo_writeback[index][subpad].take()
                && let Ok(packet) = AmiiboDataPacket::new(subpad as u8, &data)
            {
                batch.amiibo_data.push(packet);
            }
        }

        let server_state_sequence = feedback.server_state_sequence;
        if feedback.udp_last_server_state_sequence[index] != server_state_sequence {
            feedback.udp_last_server_state_sequence[index] = server_state_sequence;
            if !sent_assignment {
                batch.assignments.push(make_assignment_packet(
                    index,
                    0,
                    client.assignments[0],
                    active_clients_u8,
                    free_slots_u8,
                    sleeping,
                ));
            }
        }

        let periodic_roster = feedback.udp_last_roster_send_us[index] == 0
            || now_us.saturating_sub(feedback.udp_last_roster_send_us[index]) >= ROSTER_RESEND_US;
        if feedback.udp_last_roster_sequence[index] != feedback.roster_sequence || periodic_roster {
            feedback.udp_last_roster_sequence[index] = feedback.roster_sequence;
            feedback.udp_last_roster_send_us[index] = now_us;
            batch.roster = Some(RosterPacket::new(feedback.roster));
        }

        Some(batch)
    }

    pub fn record_hid_write(&self) {
        self.hid_writes.fetch_add(1, Ordering::Relaxed);
    }

    #[must_use]
    pub fn counters(&self) -> (u64, u64) {
        (
            self.packets_received.load(Ordering::Relaxed),
            self.hid_writes.load(Ordering::Relaxed),
        )
    }

    pub fn mark_switch2_usb_activity(&self, now_us: u64) {
        self.switch2_last_usb_activity_us.store(now_us, Ordering::Release);
        self.switch2_host_suspended.store(false, Ordering::Release);
        self.switch2_sleep_confirmed.store(false, Ordering::Release);
    }

    pub fn mark_switch2_suspended(&self) {
        self.switch2_host_suspended.store(true, Ordering::Release);
    }

    pub fn poll_switch2_sleep(&self, now_us: u64) -> bool {
        let inactive_since = self.switch2_last_usb_activity_us.load(Ordering::Acquire);
        let sleeping = self.switch2_host_suspended.load(Ordering::Acquire)
            && inactive_since != 0
            && now_us.saturating_sub(inactive_since) >= SWITCH2_USB_SLEEP_CONFIRM_US;
        self.switch2_sleep_confirmed.store(sleeping, Ordering::Release);
        sleeping
    }
}

fn make_assignment_packet(
    client_index: usize,
    subpad: usize,
    assignment: ClientAssignmentState,
    active_clients: u8,
    free_slots: u8,
    sleeping: bool,
) -> ClientAssignmentPacket {
    let mut flags = CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
    if assignment.console_port_mask != 0 {
        flags |= CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
    }
    if sleeping {
        flags |= CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
    }
    ClientAssignmentPacket::new(
        flags,
        [
            client_index as u8,
            subpad as u8,
            assignment.console_port_mask,
            assignment.primary_console_port,
        ],
        [assignment.requested_type, assignment.virtual_type],
        [active_clients, MAX_CLIENTS as u8, free_slots],
    )
}

fn make_status_packet(subpad: usize, status: ControllerStatusState) -> ControllerStatusPacket {
    let flags = if status.body_rgb_valid {
        CONTROLLER_STATUS_FLAG_BODY_RGB_VALID
    } else {
        0
    };
    ControllerStatusPacket::new(
        subpad as u8,
        status.player_index,
        status.player_leds,
        status.body_rgb,
        flags,
    )
}

#[must_use]
pub fn switch_player_index_from_leds(player_leds: u8) -> u8 {
    match player_leds & 0x0f {
        0x01 => 0,
        0x03 => 1,
        0x07 => 2,
        0x0f => 3,
        _ => 0xff,
    }
}

#[must_use]
pub fn controller_profile_supported(family: UsbControllerFamily, profile: ControllerType) -> bool {
    match family {
        UsbControllerFamily::Switch2 => matches!(
            profile,
            ControllerType::Default
                | ControllerType::ProS2
                | ControllerType::JoyconLS2
                | ControllerType::JoyconRS2
        ),
        UsbControllerFamily::Hori => {
            matches!(profile, ControllerType::Default | ControllerType::Hori)
        }
        UsbControllerFamily::Switch1 => matches!(
            profile,
            ControllerType::Default
                | ControllerType::Pro
                | ControllerType::JoyconL
                | ControllerType::JoyconR
                | ControllerType::JoyconPair
        ),
    }
}

#[must_use]
pub fn requested_virtual_slots(report: &HidReport, present: bool) -> usize {
    if !present {
        return 0;
    }
    match report.controller_type().unwrap_or(ControllerType::Default) {
        ControllerType::JoyconPair | ControllerType::JoyconPairS2 => 2,
        _ => 1,
    }
}

#[cfg(test)]
mod tests {
    use super::{
        controller_profile_supported, switch_player_index_from_leds, ClientAssignmentState,
        ServerContext, SessionError, UsbControllerFamily,
    };
    use ns_shared::control_packets::RosterEntry;
    use ns_shared::protocol::{ControllerType, MultiReport, RumblePacket};
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    fn address() -> SocketAddr {
        SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 7331)
    }

    #[test]
    fn allocates_reuses_and_expires_udp_sessions() {
        let context = ServerContext::default();
        let address = address();
        let slot = context.register_udp(address, 1_000).expect("slot");
        assert_eq!(context.register_udp(address, 2_000).expect("same slot"), slot);
        context
            .update_udp_report(slot, 1, MultiReport::default(), 2_000)
            .expect("report");
        assert_eq!(context.active_client_count(2_000), 1);
        context.expire_stale_clients(31_000_001);
        assert_eq!(context.active_client_count(31_000_001), 0);
    }

    #[test]
    fn family_change_fails_with_active_clients() {
        let context = ServerContext::default();
        context.register_udp(address(), 100).expect("slot");
        assert!(matches!(
            context.set_family(UsbControllerFamily::Switch2, 100),
            Err(SessionError::ServerFull)
        ));
    }

    #[test]
    fn profile_and_led_rules_match_existing_server() {
        assert!(controller_profile_supported(
            UsbControllerFamily::Switch2,
            ControllerType::ProS2
        ));
        assert!(!controller_profile_supported(
            UsbControllerFamily::Switch2,
            ControllerType::JoyconPairS2
        ));
        assert_eq!(switch_player_index_from_leds(0x07), 2);
        assert_eq!(switch_player_index_from_leds(0x02), 0xff);
    }

    #[test]
    fn udp_feedback_is_event_driven_and_roster_is_periodic() {
        let context = ServerContext::default();
        let slot = context.register_udp(address(), 1_000).expect("slot");
        context
            .update_udp_report(slot, 1, MultiReport::default(), 1_000)
            .expect("report");
        context.enable_udp_feedback(slot).expect("feedback");
        context
            .publish_assignment(
                slot,
                0,
                ClientAssignmentState::new(0x01, 0, ControllerType::Pro, ControllerType::Pro),
            )
            .expect("assignment");
        context
            .publish_controller_status(slot, 0, 0x03, Some([1, 2, 3]))
            .expect("status");
        context
            .store_client_source_names(
                slot,
                [
                    RosterEntry::new(true, true, "Pad 1"),
                    RosterEntry::default(),
                    RosterEntry::default(),
                    RosterEntry::default(),
                ],
            )
            .expect("names");
        context
            .publish_rumble(slot, 0, RumblePacket::new(0, 10, 20, 3))
            .expect("rumble");

        let first = context.take_udp_feedback(slot, 1_000).expect("batch");
        assert_eq!(first.rumble().len(), 1);
        assert_eq!(first.assignments().len(), 1);
        assert_eq!(first.controller_status().len(), 1);
        assert!(first.roster().is_some());

        let quiet = context.take_udp_feedback(slot, 1_100).expect("quiet batch");
        assert!(quiet.is_empty());
        let periodic = context
            .take_udp_feedback(slot, 2_001_001)
            .expect("periodic batch");
        assert!(periodic.roster().is_some());
    }

    #[test]
    fn amiibo_request_repeats_three_times_and_writeback_is_one_shot() {
        let context = ServerContext::default();
        let slot = context.register_udp(address(), 1_000).expect("slot");
        context
            .update_udp_report(slot, 1, MultiReport::default(), 1_000)
            .expect("report");
        context.enable_udp_feedback(slot).expect("feedback");
        let sequence = context
            .publish_amiibo_request(slot, 2, true)
            .expect("amiibo request");
        assert_ne!(sequence, 0);
        let dump = vec![0; 540];
        context
            .publish_amiibo_writeback(slot, 2, &dump)
            .expect("writeback");

        for iteration in 0..3 {
            let batch = context
                .take_udp_feedback(slot, 1_000 + iteration)
                .expect("amiibo batch");
            assert_eq!(batch.amiibo_requests().len(), 1);
            if iteration == 0 {
                assert_eq!(batch.amiibo_data().len(), 1);
            } else {
                assert!(batch.amiibo_data().is_empty());
            }
        }
        let fourth = context.take_udp_feedback(slot, 2_000).expect("fourth batch");
        assert!(fourth.amiibo_requests().is_empty());
    }
}
