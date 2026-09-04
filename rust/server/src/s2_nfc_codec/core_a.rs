pub const TAGMO_DUMP_SIZE: usize = 532;
pub const RAW_DUMP_SIZE: usize = 540;
pub const ORIGINALITY_SIGNATURE_SIZE: usize = 32;
pub const EXTENDED_DUMP_SIZE: usize = RAW_DUMP_SIZE + ORIGINALITY_SIGNATURE_SIZE;
pub const V3_DUMP_SIZE: usize = 2048;
pub const V3_COMPAT_SPLIT: usize = 0x80;
pub const V3_COMPAT_SHIFT: usize = 0x40;
pub const V3_SRAM_OFFSET: usize = 0x3c0;
pub const V3_SRAM_SIZE: usize = 64;
pub const V3_SRAM_DATA_SIZE: usize = 62;
pub const V3_NS_REG_OFFSET: usize = 0x3b6;
pub const V3_SRAM_RF_READY: u8 = 0x08;
pub const V3_WRITE_END: usize = 0x248;
pub const V3_DEVICE_COMMAND_SIZE: usize = 74;
pub const V3_DEVICE_RESULT_SIZE: usize = 19 + V3_SRAM_SIZE;
pub const V3_EXTENDED_CLEAR_SIZE: usize = 355;
pub const V3_EXTENDED_UPDATE_SIZE: usize = 167;
pub const STATUS_PAYLOAD_SIZE: usize = 61;
pub const READ_METADATA_SIZE: usize = 63;
pub const READ_TRAILER_SIZE: usize = 19;
pub const READ_PAYLOAD_SIZE: usize = READ_METADATA_SIZE + RAW_DUMP_SIZE + READ_TRAILER_SIZE;
pub const V3_OPERATION_PREFIX_SIZE: usize = 60;
pub const V3_SECTOR_READ_PREFIX_SIZE: usize = 64;
pub const V3_SECTOR_READ_MAX_SIZE: usize = V3_SECTOR_READ_PREFIX_SIZE + V3_DUMP_SIZE;
pub const READ_CHUNK_DATA_SIZE: usize = 70;
pub const READ_CHUNK_HEADER_SIZE: usize = 3;
pub const READ_CHUNK_PAYLOAD_SIZE: usize = READ_CHUNK_HEADER_SIZE + READ_CHUNK_DATA_SIZE;
pub const WRITE_STAGING_SIZE: usize = 454;

pub const FALLBACK_ORIGINALITY_SIGNATURE: Signature = Signature([
    0x7d, 0xfd, 0xf0, 0x79, 0x36, 0x51, 0xab, 0xd7,
    0x46, 0x6e, 0x39, 0xc1, 0x91, 0xba, 0xbe, 0xb8,
    0x56, 0xce, 0xed, 0xf1, 0xce, 0x44, 0xcc, 0x75,
    0xea, 0xfb, 0x27, 0x09, 0x4d, 0x08, 0x7a, 0xe8,
]);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Signature([u8; ORIGINALITY_SIGNATURE_SIZE]);

impl Signature {
    #[must_use]
    pub const fn from_bytes(bytes: [u8; ORIGINALITY_SIGNATURE_SIZE]) -> Self {
        Self(bytes)
    }

    #[must_use]
    pub const fn bytes(&self) -> &[u8; ORIGINALITY_SIGNATURE_SIZE] {
        &self.0
    }

    pub fn fill(&mut self, value: u8) {
        self.0.fill(value);
        if value == 0 {
            resolve_v3_read_prefix(self);
        }
    }
}

impl Default for Signature {
    fn default() -> Self {
        let mut value = Self([0; ORIGINALITY_SIGNATURE_SIZE]);
        resolve_v3_read_prefix(&mut value);
        value
    }
}

pub type V3ReadPrefixResolver = fn(&mut Signature) -> bool;

fn resolver_slot() -> &'static Mutex<Option<V3ReadPrefixResolver>> {
    static SLOT: OnceLock<Mutex<Option<V3ReadPrefixResolver>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

pub fn set_v3_read_prefix_resolver(resolver: Option<V3ReadPrefixResolver>) {
    let mut slot = resolver_slot().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    *slot = resolver;
}

fn resolve_v3_read_prefix(signature: &mut Signature) {
    let resolver = {
        let slot = resolver_slot().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        *slot
    };
    if let Some(resolver) = resolver {
        let _ = resolver(signature);
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NfcError(String);

impl NfcError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for NfcError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for NfcError {}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WriteApplyResult {
    ok: bool,
    record_count: usize,
    data_bytes: usize,
    error: Option<NfcError>,
}

impl WriteApplyResult {
    #[must_use]
    pub const fn ok(&self) -> bool {
        self.ok
    }

    #[must_use]
    pub const fn record_count(&self) -> usize {
        self.record_count
    }

    #[must_use]
    pub const fn data_bytes(&self) -> usize {
        self.data_bytes
    }

    #[must_use]
    pub fn error(&self) -> Option<&NfcError> {
        self.error.as_ref()
    }

    fn failed(message: impl Into<String>) -> Self {
        Self {
            error: Some(NfcError::new(message)),
            ..Self::default()
        }
    }

    fn success(record_count: usize, data_bytes: usize) -> Self {
        Self {
            ok: true,
            record_count,
            data_bytes,
            error: None,
        }
    }
}

fn all_covered(coverage: &[u8], size: usize) -> bool {
    coverage.len() >= size && coverage[..size].iter().all(|value| *value != 0)
}

fn command_identity_matches(data: &[u8], image: &[u8]) -> bool {
    data.len() >= 11
        && image.len() == V3_DUMP_SIZE
        && image[..7] == data[2..9]
        && data[9] == 0x01
}

fn sector1_capability_valid(value: &[u8]) -> bool {
    value.len() >= 4 && value[0] == 0xa5 && value[1] == 0 && value[2] != 0 && value[3] == 0
}

fn range_nonzero(image: &[u8], offset: usize, size: usize) -> bool {
    image
        .get(offset..offset.saturating_add(size))
        .is_some_and(|range| range.iter().any(|value| *value != 0))
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct V3ExtendedLayout {
    sector0_page: u8,
    sector1_capability_page: u8,
}

fn extended_update_layout(data: &[u8]) -> Option<V3ExtendedLayout> {
    if data.len() < 68
        || data[22] != 0x03
        || data[23] != 0x00
        || data[24] != 0x04
        || data[25] != 0x04
        || data[30] != 0x00
        || data[32] != 0x20
        || data[65] != 0x01
        || data[67] != 0x60
    {
        return None;
    }
    let sector0_page = data[31];
    let capability_page = data[13];
    let data_page = u16::from(capability_page) + 1;
    if sector0_page < 0x92
        || u16::from(sector0_page) + 7 > 0xe1
        || data_page > 0xff
        || data_page + 23 > 0xff
        || data[66] != u8::try_from(data_page).ok()?
    {
        return None;
    }
    Some(V3ExtendedLayout {
        sector0_page,
        sector1_capability_page: capability_page,
    })
}

fn current_capability_generation(image: &[u8], layout: V3ExtendedLayout) -> u8 {
    let capability_offset = 0x400 + usize::from(layout.sector1_capability_page) * 4;
    let Some(stored) = image.get(capability_offset..capability_offset + 4) else {
        return 0;
    };
    if sector1_capability_valid(stored) {
        return stored[2];
    }
    let sector0_offset = usize::from(layout.sector0_page) * 4;
    if range_nonzero(image, sector0_offset, 0x20)
        || range_nonzero(image, capability_offset + 4, 0x60)
    {
        1
    } else {
        0
    }
}

fn build_compat540(image: &[u8]) -> Result<[u8; RAW_DUMP_SIZE], NfcError> {
    validate_v3_dump(image)?;
    let mut out = [0u8; RAW_DUMP_SIZE];
    out[..V3_COMPAT_SPLIT].copy_from_slice(&image[..V3_COMPAT_SPLIT]);
    let source_start = V3_COMPAT_SPLIT + V3_COMPAT_SHIFT;
    let source_end = source_start + RAW_DUMP_SIZE - V3_COMPAT_SPLIT;
    out[V3_COMPAT_SPLIT..].copy_from_slice(&image[source_start..source_end]);
    Ok(out)
}

#[must_use]
pub fn crc16_mcrf4xx(bytes: &[u8]) -> u16 {
    let mut crc = 0xffffu16;
    for value in bytes {
        crc ^= u16::from(*value);
        for _ in 0..8 {
            crc = (crc >> 1) ^ if crc & 1 != 0 { 0x8408 } else { 0 };
        }
    }
    crc
}

#[must_use]
pub fn uid_from_raw(raw: &[u8]) -> [u8; 7] {
    if raw.len() < 8 {
        return [0; 7];
    }
    [raw[0], raw[1], raw[2], raw[4], raw[5], raw[6], raw[7]]
}

#[must_use]
pub fn uid_from_dump(dump: &[u8]) -> [u8; 7] {
    if dump.len() == V3_DUMP_SIZE {
        return dump[..7].try_into().expect("v3 UID slice is exactly seven bytes");
    }
    uid_from_raw(dump)
}

pub fn validate_raw_dump(raw: &[u8]) -> Result<(), NfcError> {
    if raw.len() != RAW_DUMP_SIZE {
        return Err(NfcError::new("raw NTAG215 dump must be exactly 540 bytes"));
    }
    let bcc0 = 0x88 ^ raw[0] ^ raw[1] ^ raw[2];
    let bcc1 = raw[4] ^ raw[5] ^ raw[6] ^ raw[7];
    if raw[3] != bcc0 || raw[8] != bcc1 {
        return Err(NfcError::new(
            "UID/BCC bytes are inconsistent; this is not a valid raw NTAG215 image",
        ));
    }
    Ok(())
}

pub fn validate_v3_dump(image: &[u8]) -> Result<(), NfcError> {
    if image.len() != V3_DUMP_SIZE {
        return Err(NfcError::new("amiibo v3 dump must be exactly 2048 bytes"));
    }
    if image[0] != 0x04 || image[7] != 0x00 || image[8] != 0x44 {
        return Err(NfcError::new("invalid NTAG I2C Plus 2K UID/manufacturer header"));
    }
    Ok(())
}

#[must_use]
pub fn v3_sram_response_valid(image: &[u8]) -> bool {
    if image.len() != V3_DUMP_SIZE {
        return false;
    }
    let sram = &image[V3_SRAM_OFFSET..V3_SRAM_OFFSET + V3_SRAM_SIZE];
    let expected = crc16_mcrf4xx(&sram[..V3_SRAM_DATA_SIZE]);
    let stored = (u16::from(sram[V3_SRAM_DATA_SIZE]) << 8) | u16::from(sram[V3_SRAM_DATA_SIZE + 1]);
    stored == expected
}

