//! Remaining byte-compatible control/status packet types from the C++ protocol.
//!
//! These codecs use explicit little-endian serialization. They never depend on
//! Rust struct layout, packed references, transmutation, or raw pointers.

use crate::protocol::{
    AMIIBO_DATA_MAGIC, AMIIBO_DATA_PACKET_SIZE, AMIIBO_LIBRARY_AUTH_SIZE,
    AMIIBO_LIBRARY_MAGIC, AMIIBO_LIBRARY_PACKET_SIZE, AMIIBO_LIBRARY_RESULT_MAGIC,
    AMIIBO_LIBRARY_RESULT_PACKET_SIZE, AMIIBO_LIBRARY_VERSION, AMIIBO_MAX_DUMP_SIZE,
    AMIIBO_REQUEST_MAGIC, AMIIBO_REQUEST_PACKET_SIZE, CLIENT_ASSIGNMENT_MAGIC,
    CLIENT_ASSIGNMENT_PACKET_SIZE, CLIENT_ASSIGNMENT_VERSION, CLIENT_NAMES_AUTH_SIZE,
    CLIENT_NAMES_MAGIC, CLIENT_NAMES_PACKET_SIZE, CONTROLLER_STATUS_MAGIC,
    CONTROLLER_STATUS_PACKET_SIZE, ControllerType, HMAC_TAG_SIZE, ROSTER_ENTRY_SIZE,
    ROSTER_MAGIC, ROSTER_NAME_CAP, ROSTER_PACKET_SIZE, S2_AUDIO_CAPS_AUTH_SIZE,
    S2_AUDIO_CAPS_MAGIC, S2_AUDIO_CAPS_PACKET_SIZE, S2_AUDIO_VERSION, SERVER_INFO_VERSION,
    WireError,
};

fn exact<const N: usize>(bytes: &[u8]) -> Result<&[u8; N], WireError> {
    bytes.try_into().map_err(|_| WireError::InvalidLength {
        expected: N,
        actual: bytes.len(),
    })
}

fn u16_le(bytes: &[u8]) -> u16 {
    u16::from_le_bytes(bytes.try_into().expect("wire slice has two bytes"))
}

fn u32_le(bytes: &[u8]) -> u32 {
    u32::from_le_bytes(bytes.try_into().expect("wire slice has four bytes"))
}

fn u64_le(bytes: &[u8]) -> u64 {
    u64::from_le_bytes(bytes.try_into().expect("wire slice has eight bytes"))
}

fn ensure_magic(expected: u32, actual: u32) -> Result<(), WireError> {
    if expected == actual {
        Ok(())
    } else {
        Err(WireError::InvalidMagic { expected, actual })
    }
}

fn controller_type_from_wire(value: u8) -> Result<ControllerType, WireError> {
    match value {
        0 => Ok(ControllerType::Default),
        1 => Ok(ControllerType::JoyconL),
        2 => Ok(ControllerType::JoyconR),
        3 => Ok(ControllerType::Pro),
        4 => Ok(ControllerType::JoyconPair),
        5 => Ok(ControllerType::Hori),
        6 => Ok(ControllerType::ProS2),
        7 => Ok(ControllerType::JoyconLS2),
        8 => Ok(ControllerType::JoyconRS2),
        9 => Ok(ControllerType::JoyconPairS2),
        other => Err(WireError::InvalidValue("controller type", other)),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClientAssignmentPacket {
    flags: u8,
    server_slot: u8,
    subpad: u8,
    console_port_mask: u8,
    primary_console_port: u8,
    requested_type: ControllerType,
    virtual_type: ControllerType,
    active_clients: u8,
    max_clients: u8,
    free_virtual_slots: u8,
}

impl ClientAssignmentPacket {
    #[must_use]
    pub const fn new(
        flags: u8,
        slots: [u8; 4],
        types: [ControllerType; 2],
        capacity: [u8; 3],
    ) -> Self {
        Self {
            flags,
            server_slot: slots[0],
            subpad: slots[1],
            console_port_mask: slots[2],
            primary_console_port: slots[3],
            requested_type: types[0],
            virtual_type: types[1],
            active_clients: capacity[0],
            max_clients: capacity[1],
            free_virtual_slots: capacity[2],
        }
    }

    #[must_use]
    pub const fn flags(&self) -> u8 {
        self.flags
    }

    #[must_use]
    pub const fn slots(&self) -> [u8; 4] {
        [
            self.server_slot,
            self.subpad,
            self.console_port_mask,
            self.primary_console_port,
        ]
    }

    #[must_use]
    pub const fn controller_types(&self) -> [ControllerType; 2] {
        [self.requested_type, self.virtual_type]
    }

    #[must_use]
    pub const fn capacity(&self) -> [u8; 3] {
        [self.active_clients, self.max_clients, self.free_virtual_slots]
    }

    #[must_use]
    pub fn encode(&self) -> [u8; CLIENT_ASSIGNMENT_PACKET_SIZE] {
        let mut out = [0u8; CLIENT_ASSIGNMENT_PACKET_SIZE];
        out[..4].copy_from_slice(&CLIENT_ASSIGNMENT_MAGIC.to_le_bytes());
        out[4] = CLIENT_ASSIGNMENT_VERSION;
        out[5] = self.flags;
        out[6] = self.server_slot;
        out[7] = self.subpad;
        out[8] = self.console_port_mask;
        out[9] = self.primary_console_port;
        out[10] = self.requested_type as u8;
        out[11] = self.virtual_type as u8;
        out[12] = self.active_clients;
        out[13] = self.max_clients;
        out[14] = self.free_virtual_slots;
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<CLIENT_ASSIGNMENT_PACKET_SIZE>(bytes)?;
        ensure_magic(CLIENT_ASSIGNMENT_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self::new(
            bytes[5],
            [bytes[6], bytes[7], bytes[8], bytes[9]],
            [
                controller_type_from_wire(bytes[10])?,
                controller_type_from_wire(bytes[11])?,
            ],
            [bytes[12], bytes[13], bytes[14]],
        ))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ControllerStatusPacket {
    subpad: u8,
    player_index: u8,
    player_leds: u8,
    body_rgb: [u8; 3],
    flags: u8,
}

impl ControllerStatusPacket {
    #[must_use]
    pub const fn new(
        subpad: u8,
        player_index: u8,
        player_leds: u8,
        body_rgb: [u8; 3],
        flags: u8,
    ) -> Self {
        Self {
            subpad,
            player_index,
            player_leds,
            body_rgb,
            flags,
        }
    }

    #[must_use]
    pub const fn subpad(&self) -> u8 {
        self.subpad
    }

    #[must_use]
    pub const fn player_index(&self) -> u8 {
        self.player_index
    }

    #[must_use]
    pub const fn player_leds(&self) -> u8 {
        self.player_leds
    }

    #[must_use]
    pub const fn body_rgb(&self) -> [u8; 3] {
        self.body_rgb
    }

    #[must_use]
    pub const fn flags(&self) -> u8 {
        self.flags
    }

    #[must_use]
    pub fn encode(&self) -> [u8; CONTROLLER_STATUS_PACKET_SIZE] {
        let mut out = [0u8; CONTROLLER_STATUS_PACKET_SIZE];
        out[..4].copy_from_slice(&CONTROLLER_STATUS_MAGIC.to_le_bytes());
        out[4] = SERVER_INFO_VERSION;
        out[5] = self.subpad;
        out[6] = self.player_index;
        out[7] = self.player_leds;
        out[8..11].copy_from_slice(&self.body_rgb);
        out[11] = self.flags;
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<CONTROLLER_STATUS_PACKET_SIZE>(bytes)?;
        ensure_magic(CONTROLLER_STATUS_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self::new(
            bytes[5],
            bytes[6],
            bytes[7],
            [bytes[8], bytes[9], bytes[10]],
            bytes[11],
        ))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RosterEntry {
    present: bool,
    has_gyro: bool,
    name: [u8; ROSTER_NAME_CAP],
}

impl Default for RosterEntry {
    fn default() -> Self {
        Self {
            present: false,
            has_gyro: false,
            name: [0; ROSTER_NAME_CAP],
        }
    }
}

impl RosterEntry {
    #[must_use]
    pub fn new(present: bool, has_gyro: bool, name: &str) -> Self {
        let mut bytes = [0u8; ROSTER_NAME_CAP];
        let count = name.len().min(ROSTER_NAME_CAP.saturating_sub(1));
        bytes[..count].copy_from_slice(&name.as_bytes()[..count]);
        Self {
            present,
            has_gyro,
            name: bytes,
        }
    }

    #[must_use]
    pub const fn present(&self) -> bool {
        self.present
    }

    #[must_use]
    pub const fn has_gyro(&self) -> bool {
        self.has_gyro
    }

    #[must_use]
    pub fn name(&self) -> String {
        let end = self
            .name
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(ROSTER_NAME_CAP);
        String::from_utf8_lossy(&self.name[..end]).into_owned()
    }

    #[must_use]
    pub fn encode(&self) -> [u8; ROSTER_ENTRY_SIZE] {
        let mut out = [0u8; ROSTER_ENTRY_SIZE];
        out[0] = u8::from(self.present);
        out[1] = u8::from(self.has_gyro);
        out[2..].copy_from_slice(&self.name);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<ROSTER_ENTRY_SIZE>(bytes)?;
        Ok(Self {
            present: bytes[0] != 0,
            has_gyro: bytes[1] != 0,
            name: bytes[2..50].try_into().expect("roster name is 48 bytes"),
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClientNamesPacket {
    pads: [RosterEntry; 4],
    hmac: [u8; HMAC_TAG_SIZE],
}

impl ClientNamesPacket {
    #[must_use]
    pub const fn new(pads: [RosterEntry; 4]) -> Self {
        Self {
            pads,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn pads(&self) -> &[RosterEntry; 4] {
        &self.pads
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, hmac: [u8; HMAC_TAG_SIZE]) {
        self.hmac = hmac;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; CLIENT_NAMES_PACKET_SIZE] {
        let mut out = [0u8; CLIENT_NAMES_PACKET_SIZE];
        out[..4].copy_from_slice(&CLIENT_NAMES_MAGIC.to_le_bytes());
        out[4] = SERVER_INFO_VERSION;
        for (index, pad) in self.pads.iter().enumerate() {
            let start = 8 + index * ROSTER_ENTRY_SIZE;
            out[start..start + ROSTER_ENTRY_SIZE].copy_from_slice(&pad.encode());
        }
        out[CLIENT_NAMES_AUTH_SIZE..].copy_from_slice(&self.hmac);
        out
    }

    #[must_use]
    pub fn authenticated_bytes(&self) -> [u8; CLIENT_NAMES_AUTH_SIZE] {
        let encoded = self.encode();
        encoded[..CLIENT_NAMES_AUTH_SIZE]
            .try_into()
            .expect("client names auth prefix has fixed size")
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<CLIENT_NAMES_PACKET_SIZE>(bytes)?;
        ensure_magic(CLIENT_NAMES_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            pads: [
                RosterEntry::decode(&bytes[8..58])?,
                RosterEntry::decode(&bytes[58..108])?,
                RosterEntry::decode(&bytes[108..158])?,
                RosterEntry::decode(&bytes[158..208])?,
            ],
            hmac: bytes[208..224].try_into().expect("HMAC field is 16 bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RosterPacket {
    ports: [RosterEntry; 4],
}

impl RosterPacket {
    #[must_use]
    pub const fn new(ports: [RosterEntry; 4]) -> Self {
        Self { ports }
    }

    #[must_use]
    pub const fn ports(&self) -> &[RosterEntry; 4] {
        &self.ports
    }

    #[must_use]
    pub fn encode(&self) -> [u8; ROSTER_PACKET_SIZE] {
        let mut out = [0u8; ROSTER_PACKET_SIZE];
        out[..4].copy_from_slice(&ROSTER_MAGIC.to_le_bytes());
        out[4] = SERVER_INFO_VERSION;
        for (index, port) in self.ports.iter().enumerate() {
            let start = 8 + index * ROSTER_ENTRY_SIZE;
            out[start..start + ROSTER_ENTRY_SIZE].copy_from_slice(&port.encode());
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<ROSTER_PACKET_SIZE>(bytes)?;
        ensure_magic(ROSTER_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self::new([
            RosterEntry::decode(&bytes[8..58])?,
            RosterEntry::decode(&bytes[58..108])?,
            RosterEntry::decode(&bytes[108..158])?,
            RosterEntry::decode(&bytes[158..208])?,
        ]))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct S2AudioCapabilitiesPacket {
    flags: u8,
    seq: u32,
    ts_us: u64,
    hmac: [u8; HMAC_TAG_SIZE],
}

impl S2AudioCapabilitiesPacket {
    #[must_use]
    pub const fn new(flags: u8, seq: u32, ts_us: u64) -> Self {
        Self {
            flags,
            seq,
            ts_us,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn flags(&self) -> u8 {
        self.flags
    }

    #[must_use]
    pub const fn sequence(&self) -> u32 {
        self.seq
    }

    #[must_use]
    pub const fn timestamp_us(&self) -> u64 {
        self.ts_us
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, hmac: [u8; HMAC_TAG_SIZE]) {
        self.hmac = hmac;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; S2_AUDIO_CAPS_PACKET_SIZE] {
        let mut out = [0u8; S2_AUDIO_CAPS_PACKET_SIZE];
        out[..4].copy_from_slice(&S2_AUDIO_CAPS_MAGIC.to_le_bytes());
        out[4] = S2_AUDIO_VERSION;
        out[5] = self.flags;
        out[8..12].copy_from_slice(&self.seq.to_le_bytes());
        out[12..20].copy_from_slice(&self.ts_us.to_le_bytes());
        out[S2_AUDIO_CAPS_AUTH_SIZE..].copy_from_slice(&self.hmac);
        out
    }

    #[must_use]
    pub fn authenticated_bytes(&self) -> [u8; S2_AUDIO_CAPS_AUTH_SIZE] {
        let encoded = self.encode();
        encoded[..S2_AUDIO_CAPS_AUTH_SIZE]
            .try_into()
            .expect("S2 audio capabilities auth prefix has fixed size")
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<S2_AUDIO_CAPS_PACKET_SIZE>(bytes)?;
        ensure_magic(S2_AUDIO_CAPS_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            flags: bytes[5],
            seq: u32_le(&bytes[8..12]),
            ts_us: u64_le(&bytes[12..20]),
            hmac: bytes[20..36].try_into().expect("HMAC field is 16 bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AmiiboRequestPacket {
    subpad: u8,
    requested: bool,
    sequence: u16,
}

impl AmiiboRequestPacket {
    #[must_use]
    pub const fn new(subpad: u8, requested: bool, sequence: u16) -> Self {
        Self {
            subpad,
            requested,
            sequence,
        }
    }

    #[must_use]
    pub const fn subpad(&self) -> u8 {
        self.subpad
    }

    #[must_use]
    pub const fn requested(&self) -> bool {
        self.requested
    }

    #[must_use]
    pub const fn sequence(&self) -> u16 {
        self.sequence
    }

    #[must_use]
    pub fn encode(&self) -> [u8; AMIIBO_REQUEST_PACKET_SIZE] {
        let mut out = [0u8; AMIIBO_REQUEST_PACKET_SIZE];
        out[..4].copy_from_slice(&AMIIBO_REQUEST_MAGIC.to_le_bytes());
        out[4] = self.subpad;
        out[5] = u8::from(self.requested);
        out[6..8].copy_from_slice(&self.sequence.to_le_bytes());
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<AMIIBO_REQUEST_PACKET_SIZE>(bytes)?;
        ensure_magic(AMIIBO_REQUEST_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self::new(bytes[4], bytes[5] != 0, u16_le(&bytes[6..8])))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AmiiboDataPacket {
    subpad: u8,
    data_len: u16,
    data: [u8; AMIIBO_MAX_DUMP_SIZE],
}

impl AmiiboDataPacket {
    pub fn new(subpad: u8, data: &[u8]) -> Result<Self, WireError> {
        if data.len() > AMIIBO_MAX_DUMP_SIZE || data.len() > usize::from(u16::MAX) {
            return Err(WireError::InvalidLength {
                expected: AMIIBO_MAX_DUMP_SIZE,
                actual: data.len(),
            });
        }
        let mut buffer = [0u8; AMIIBO_MAX_DUMP_SIZE];
        buffer[..data.len()].copy_from_slice(data);
        Ok(Self {
            subpad,
            data_len: u16::try_from(data.len()).expect("validated amiibo length fits in u16"),
            data: buffer,
        })
    }

    #[must_use]
    pub const fn subpad(&self) -> u8 {
        self.subpad
    }

    #[must_use]
    pub fn data(&self) -> &[u8] {
        &self.data[..usize::from(self.data_len)]
    }

    #[must_use]
    pub fn encode(&self) -> [u8; AMIIBO_DATA_PACKET_SIZE] {
        let mut out = [0u8; AMIIBO_DATA_PACKET_SIZE];
        out[..4].copy_from_slice(&AMIIBO_DATA_MAGIC.to_le_bytes());
        out[4] = self.subpad;
        out[5..7].copy_from_slice(&self.data_len.to_le_bytes());
        out[7..].copy_from_slice(&self.data);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<AMIIBO_DATA_PACKET_SIZE>(bytes)?;
        ensure_magic(AMIIBO_DATA_MAGIC, u32_le(&bytes[..4]))?;
        let data_len = u16_le(&bytes[5..7]);
        if usize::from(data_len) > AMIIBO_MAX_DUMP_SIZE {
            return Err(WireError::InvalidLength {
                expected: AMIIBO_MAX_DUMP_SIZE,
                actual: usize::from(data_len),
            });
        }
        Ok(Self {
            subpad: bytes[4],
            data_len,
            data: bytes[7..]
                .try_into()
                .expect("amiibo wire payload has the fixed maximum size"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AmiiboLibraryAction {
    Select = 1,
    Clear = 2,
}

impl AmiiboLibraryAction {
    fn from_wire(value: u8) -> Result<Self, WireError> {
        match value {
            1 => Ok(Self::Select),
            2 => Ok(Self::Clear),
            other => Err(WireError::InvalidValue("amiibo library action", other)),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AmiiboLibraryResult {
    Ok = 0,
    StorageError = 1,
    GenerationError = 2,
    InvalidRequest = 3,
}

impl AmiiboLibraryResult {
    fn from_wire(value: u8) -> Result<Self, WireError> {
        match value {
            0 => Ok(Self::Ok),
            1 => Ok(Self::StorageError),
            2 => Ok(Self::GenerationError),
            3 => Ok(Self::InvalidRequest),
            other => Err(WireError::InvalidValue("amiibo library result", other)),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AmiiboLibraryPacket {
    action: AmiiboLibraryAction,
    subpad: u8,
    head: u32,
    tail: u32,
    hmac: [u8; HMAC_TAG_SIZE],
}

impl AmiiboLibraryPacket {
    #[must_use]
    pub const fn new(action: AmiiboLibraryAction, subpad: u8, head: u32, tail: u32) -> Self {
        Self {
            action,
            subpad,
            head,
            tail,
            hmac: [0; HMAC_TAG_SIZE],
        }
    }

    #[must_use]
    pub const fn selection(&self) -> (AmiiboLibraryAction, u8, u32, u32) {
        (self.action, self.subpad, self.head, self.tail)
    }

    #[must_use]
    pub const fn hmac(&self) -> &[u8; HMAC_TAG_SIZE] {
        &self.hmac
    }

    pub fn set_hmac(&mut self, hmac: [u8; HMAC_TAG_SIZE]) {
        self.hmac = hmac;
    }

    #[must_use]
    pub fn encode(&self) -> [u8; AMIIBO_LIBRARY_PACKET_SIZE] {
        let mut out = [0u8; AMIIBO_LIBRARY_PACKET_SIZE];
        out[..4].copy_from_slice(&AMIIBO_LIBRARY_MAGIC.to_le_bytes());
        out[4] = AMIIBO_LIBRARY_VERSION;
        out[5] = self.action as u8;
        out[6] = self.subpad;
        out[8..12].copy_from_slice(&self.head.to_le_bytes());
        out[12..16].copy_from_slice(&self.tail.to_le_bytes());
        out[AMIIBO_LIBRARY_AUTH_SIZE..].copy_from_slice(&self.hmac);
        out
    }

    #[must_use]
    pub fn authenticated_bytes(&self) -> [u8; AMIIBO_LIBRARY_AUTH_SIZE] {
        let encoded = self.encode();
        encoded[..AMIIBO_LIBRARY_AUTH_SIZE]
            .try_into()
            .expect("amiibo library auth prefix has fixed size")
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<AMIIBO_LIBRARY_PACKET_SIZE>(bytes)?;
        ensure_magic(AMIIBO_LIBRARY_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self {
            action: AmiiboLibraryAction::from_wire(bytes[5])?,
            subpad: bytes[6],
            head: u32_le(&bytes[8..12]),
            tail: u32_le(&bytes[12..16]),
            hmac: bytes[16..32].try_into().expect("HMAC field is 16 bytes"),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AmiiboLibraryResultPacket {
    action: AmiiboLibraryAction,
    result: AmiiboLibraryResult,
    subpad: u8,
    head: u32,
    tail: u32,
    tag_size: u16,
}

impl AmiiboLibraryResultPacket {
    #[must_use]
    pub const fn new(
        action: AmiiboLibraryAction,
        result: AmiiboLibraryResult,
        subpad: u8,
        ids: [u32; 2],
        tag_size: u16,
    ) -> Self {
        Self {
            action,
            result,
            subpad,
            head: ids[0],
            tail: ids[1],
            tag_size,
        }
    }

    #[must_use]
    pub const fn result(&self) -> AmiiboLibraryResult {
        self.result
    }

    #[must_use]
    pub fn encode(&self) -> [u8; AMIIBO_LIBRARY_RESULT_PACKET_SIZE] {
        let mut out = [0u8; AMIIBO_LIBRARY_RESULT_PACKET_SIZE];
        out[..4].copy_from_slice(&AMIIBO_LIBRARY_RESULT_MAGIC.to_le_bytes());
        out[4] = AMIIBO_LIBRARY_VERSION;
        out[5] = self.action as u8;
        out[6] = self.result as u8;
        out[7] = self.subpad;
        out[8..12].copy_from_slice(&self.head.to_le_bytes());
        out[12..16].copy_from_slice(&self.tail.to_le_bytes());
        out[16..18].copy_from_slice(&self.tag_size.to_le_bytes());
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, WireError> {
        let bytes = exact::<AMIIBO_LIBRARY_RESULT_PACKET_SIZE>(bytes)?;
        ensure_magic(AMIIBO_LIBRARY_RESULT_MAGIC, u32_le(&bytes[..4]))?;
        Ok(Self::new(
            AmiiboLibraryAction::from_wire(bytes[5])?,
            AmiiboLibraryResult::from_wire(bytes[6])?,
            bytes[7],
            [u32_le(&bytes[8..12]), u32_le(&bytes[12..16])],
            u16_le(&bytes[16..18]),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::{
        AmiiboDataPacket, AmiiboLibraryAction, AmiiboLibraryPacket, AmiiboLibraryResult,
        AmiiboLibraryResultPacket, AmiiboRequestPacket, ClientAssignmentPacket, ClientNamesPacket,
        ControllerStatusPacket, RosterEntry, RosterPacket, S2AudioCapabilitiesPacket,
    };
    use crate::protocol::{
        AMIIBO_DATA_PACKET_SIZE, AMIIBO_LIBRARY_PACKET_SIZE, AMIIBO_LIBRARY_RESULT_PACKET_SIZE,
        AMIIBO_REQUEST_PACKET_SIZE, CLIENT_ASSIGNMENT_PACKET_SIZE, CLIENT_NAMES_PACKET_SIZE,
        CONTROLLER_STATUS_PACKET_SIZE, ControllerType, ROSTER_PACKET_SIZE,
        S2_AUDIO_CAPS_PACKET_SIZE,
    };

    #[test]
    fn fixed_packet_sizes_match_cpp_static_asserts() {
        let assignment = ClientAssignmentPacket::new(
            1,
            [2, 3, 4, 1],
            [ControllerType::Pro, ControllerType::ProS2],
            [2, 4, 2],
        );
        assert_eq!(assignment.encode().len(), CLIENT_ASSIGNMENT_PACKET_SIZE);
        assert_eq!(ClientAssignmentPacket::decode(&assignment.encode()), Ok(assignment));

        let status = ControllerStatusPacket::new(1, 2, 0x0f, [10, 20, 30], 1);
        assert_eq!(status.encode().len(), CONTROLLER_STATUS_PACKET_SIZE);
        assert_eq!(ControllerStatusPacket::decode(&status.encode()), Ok(status));
    }

    #[test]
    fn roster_and_client_names_round_trip() {
        let entry = RosterEntry::new(true, true, "Controller One");
        let roster = RosterPacket::new([entry; 4]);
        assert_eq!(roster.encode().len(), ROSTER_PACKET_SIZE);
        assert_eq!(RosterPacket::decode(&roster.encode()), Ok(roster));
        assert_eq!(roster.ports()[0].name(), "Controller One");

        let names = ClientNamesPacket::new([entry; 4]);
        assert_eq!(names.encode().len(), CLIENT_NAMES_PACKET_SIZE);
        assert_eq!(ClientNamesPacket::decode(&names.encode()), Ok(names));
    }

    #[test]
    fn audio_caps_and_amiibo_packets_round_trip() {
        let caps = S2AudioCapabilitiesPacket::new(3, 4, 5);
        assert_eq!(caps.encode().len(), S2_AUDIO_CAPS_PACKET_SIZE);
        assert_eq!(S2AudioCapabilitiesPacket::decode(&caps.encode()), Ok(caps));

        let request = AmiiboRequestPacket::new(2, true, 0x1234);
        assert_eq!(request.encode().len(), AMIIBO_REQUEST_PACKET_SIZE);
        assert_eq!(AmiiboRequestPacket::decode(&request.encode()), Ok(request));

        let data = AmiiboDataPacket::new(1, &[0xa5; 540]).expect("valid amiibo data");
        assert_eq!(data.encode().len(), AMIIBO_DATA_PACKET_SIZE);
        assert_eq!(AmiiboDataPacket::decode(&data.encode()), Ok(data));

        let library = AmiiboLibraryPacket::new(AmiiboLibraryAction::Select, 0, 1, 2);
        assert_eq!(library.encode().len(), AMIIBO_LIBRARY_PACKET_SIZE);
        assert_eq!(AmiiboLibraryPacket::decode(&library.encode()), Ok(library));

        let result = AmiiboLibraryResultPacket::new(
            AmiiboLibraryAction::Select,
            AmiiboLibraryResult::Ok,
            0,
            [1, 2],
            540,
        );
        assert_eq!(result.encode().len(), AMIIBO_LIBRARY_RESULT_PACKET_SIZE);
        assert_eq!(AmiiboLibraryResultPacket::decode(&result.encode()), Ok(result));
    }
}
