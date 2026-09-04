//! Safe Rust port of the Switch 2 NFC/Amiibo codec and host-replayable state machine.
//!
//! All byte manipulation uses checked slices. No packed structs, raw pointers, FFI, or
//! layout casts are used.

use std::fmt;
use std::sync::{Mutex, OnceLock};

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

pub fn build_read_buffer_payload(
    raw: &[u8],
    signature: &[u8],
    operation_metadata: &[u8],
    write_mode: bool,
) -> Result<Vec<u8>, NfcError> {
    validate_raw_dump(raw)?;
    let mut output = vec![0u8; READ_PAYLOAD_SIZE];
    output[0] = 0x04;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    output[8..15].copy_from_slice(&uid_from_raw(raw));
    if signature.len() >= ORIGINALITY_SIGNATURE_SIZE {
        output[19..51].copy_from_slice(&signature[..ORIGINALITY_SIGNATURE_SIZE]);
    } else {
        output[19..51].copy_from_slice(FALLBACK_ORIGINALITY_SIGNATURE.bytes());
    }
    if operation_metadata.len() >= 9 {
        output[51..60].copy_from_slice(&operation_metadata[..9]);
    }
    if write_mode {
        output[63..71].copy_from_slice(&raw[12..20]);
        output[71] = 0x04;
        output[72] = 0x54;
        output[73] = 0x02;
        output[74] = 0x01;
    } else {
        output[63..603].copy_from_slice(raw);
        output[603] = 0x01;
        output[604] = 0x00;
        output[605] = 0x0f;
    }
    Ok(output)
}

#[must_use]
pub fn apply_write_staging(staging: &[u8], coverage: &[u8], raw: &mut [u8]) -> WriteApplyResult {
    if let Err(error) = validate_raw_dump(raw) {
        return WriteApplyResult::failed(error.to_string());
    }
    if staging.len() < WRITE_STAGING_SIZE {
        return WriteApplyResult::failed("staging buffer is incomplete");
    }
    if !all_covered(coverage, WRITE_STAGING_SIZE) {
        return WriteApplyResult::failed("staging stream did not receive all required chunks");
    }
    if staging[0] != 0xd0 || staging[1] != 0x07 {
        return WriteApplyResult::failed("staging header does not start with D0 07");
    }
    if uid_from_raw(raw) != staging[2..9] {
        return WriteApplyResult::failed("staging UID does not match the active tag");
    }
    let record_count = usize::from(staging[21]);
    if record_count == 0 || record_count > 16 {
        return WriteApplyResult::failed(format!("invalid staging record count {record_count}"));
    }
    let mut cursor = 22usize;
    let mut data_bytes = 0usize;
    for _ in 0..record_count {
        if cursor + 2 > WRITE_STAGING_SIZE {
            return WriteApplyResult::failed("staging record header overruns staging buffer");
        }
        let page = staging[cursor];
        let length = usize::from(staging[cursor + 1]);
        cursor += 2;
        if page == 0 || length == 0 || cursor + length > WRITE_STAGING_SIZE {
            return WriteApplyResult::failed(format!(
                "invalid staging record page {page} length {length}"
            ));
        }
        let address = usize::from(page) * 4;
        if address < 16 || address + length > RAW_DUMP_SIZE {
            return WriteApplyResult::failed(format!(
                "staging record target address 0x{address:03x} length {length} out of bounds"
            ));
        }
        cursor += length;
        data_bytes += length;
    }
    if staging[cursor..WRITE_STAGING_SIZE].iter().any(|value| *value != 0) {
        return WriteApplyResult::failed("staging buffer contains non-zero trailing padding");
    }
    raw[16..20].copy_from_slice(&staging[17..21]);
    cursor = 22;
    for _ in 0..record_count {
        let page = staging[cursor];
        let length = usize::from(staging[cursor + 1]);
        cursor += 2;
        let address = usize::from(page) * 4;
        raw[address..address + length].copy_from_slice(&staging[cursor..cursor + length]);
        cursor += length;
    }
    WriteApplyResult::success(record_count, data_bytes)
}

pub fn build_v3_read_buffer(
    image: &[u8],
    signature: &[u8],
    request: &[u8],
) -> Result<Vec<u8>, NfcError> {
    validate_v3_dump(image)?;
    let mut highest = 0usize;
    if request.len() >= 13 {
        let blocks = usize::from(request[10]);
        if blocks != 0 && 11 + blocks * 2 <= request.len() {
            for block in 0..blocks {
                let start = request[11 + block * 2];
                let end = request[12 + block * 2];
                if end >= start {
                    highest = highest.max((usize::from(end) + 1) * 4);
                }
            }
        }
    }
    let compat;
    let source = if highest <= RAW_DUMP_SIZE {
        compat = build_compat540(image)?;
        &compat[..]
    } else {
        image
    };
    let mut output = vec![0u8; V3_OPERATION_PREFIX_SIZE];
    output[0] = 0x04;
    output[4] = 0x01;
    output[5] = 0x02;
    output[6] = 0x00;
    output[7] = 0x07;
    output[8..15].copy_from_slice(&image[..7]);
    output[18] = 0x06;
    if signature.len() >= ORIGINALITY_SIGNATURE_SIZE {
        output[19..51].copy_from_slice(&signature[..ORIGINALITY_SIGNATURE_SIZE]);
    }
    if request.len() >= 19 {
        output[51..60].copy_from_slice(&request[10..19]);
    }
    let mut copied = false;
    if request.len() >= 13 {
        let blocks = usize::from(request[10]);
        if blocks != 0 && 11 + blocks * 2 <= request.len() {
            for block in 0..blocks {
                let start = request[11 + block * 2];
                let end = request[12 + block * 2];
                if end < start {
                    continue;
                }
                let from = usize::from(start) * 4;
                let len = (usize::from(end - start) + 1) * 4;
                if let Some(range) = source.get(from..from + len) {
                    output.extend_from_slice(range);
                    copied = true;
                }
            }
        }
    }
    if !copied {
        output.extend_from_slice(&source[..source.len().min(1024)]);
    }
    Ok(output)
}

pub fn build_v3_sector_read_buffer(
    image: &[u8],
    signature: &[u8],
    request: &[u8],
) -> Result<Vec<u8>, NfcError> {
    validate_v3_dump(image)?;
    if !(17..=23).contains(&request.len())
        || request.get(2..9) != Some(&image[..7])
        || request.get(9) != Some(&0x01)
    {
        return Err(NfcError::new("malformed sector read request"));
    }
    let range_count = usize::from(request[10]);
    if !(1..=2).contains(&range_count) {
        return Err(NfcError::new("invalid sector range count"));
    }
    let ranges_end = 11 + range_count * 3;
    if ranges_end + 6 != request.len() {
        return Err(NfcError::new("sector read request size mismatch"));
    }
    if request[ranges_end..].iter().any(|value| *value != 0) {
        return Err(NfcError::new("non-zero reserved bytes in sector read request"));
    }
    let mut output = vec![0u8; V3_SECTOR_READ_PREFIX_SIZE];
    output[0] = 0x15;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    output[8..15].copy_from_slice(&image[..7]);
    output[18] = 0x06;
    if signature.len() >= ORIGINALITY_SIGNATURE_SIZE {
        output[19..51].copy_from_slice(&signature[..ORIGINALITY_SIGNATURE_SIZE]);
    }
    output[51..51 + request.len() - 10].copy_from_slice(&request[10..]);

    for index in 0..range_count {
        let sector = request[11 + index * 3];
        let first = request[12 + index * 3];
        let last = request[13 + index * 3];
        if sector > 1 || last < first {
            return Err(NfcError::new("invalid sector or page range"));
        }
        let length = (usize::from(last - first) + 1) * 4;
        let address = usize::from(sector) * 0x400 + usize::from(first) * 4;
        let Some(range) = image.get(address..address + length) else {
            return Err(NfcError::new("sector page range out of bounds"));
        };
        let cursor = output.len();
        output.extend_from_slice(range);
        let air_riders_capability_range = range_count == 2
            && index == 1
            && sector == 1
            && request[11] == 0
            && (u16::from(request[13]) - u16::from(request[12]) + 1 == 8)
            && (u16::from(last) - u16::from(first) + 1 == 25);
        if air_riders_capability_range && !sector1_capability_valid(&output[cursor..cursor + 4]) {
            output[cursor..cursor + 4].copy_from_slice(&[0xa5, 0x00, 0x01, 0x00]);
        }
    }
    Ok(output)
}

pub fn build_v3_device_result(image: &[u8]) -> Result<Vec<u8>, NfcError> {
    validate_v3_dump(image)?;
    let mut output = vec![0u8; V3_DEVICE_RESULT_SIZE];
    output[0] = 0x18;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    output[8..15].copy_from_slice(&image[..7]);
    output[18] = 0x06;
    output[19..19 + V3_SRAM_SIZE]
        .copy_from_slice(&image[V3_SRAM_OFFSET..V3_SRAM_OFFSET + V3_SRAM_SIZE]);
    let ready_index = 19 + V3_NS_REG_OFFSET - V3_SRAM_OFFSET;
    output[ready_index] |= V3_SRAM_RF_READY;
    let crc = crc16_mcrf4xx(&output[19..19 + V3_SRAM_DATA_SIZE]);
    output[19 + V3_SRAM_DATA_SIZE..19 + V3_SRAM_SIZE].copy_from_slice(&crc.to_le_bytes());
    Ok(output)
}

#[must_use]
pub fn build_buffer_chunk(buffer: &[u8], offset: u16) -> Vec<u8> {
    let offset = usize::from(offset);
    if offset >= buffer.len() {
        return vec![0x01, 0x00, 0x00];
    }
    let chunk_len = READ_CHUNK_DATA_SIZE.min(buffer.len() - offset);
    let is_last = offset + chunk_len >= buffer.len();
    let mut output = Vec::with_capacity(READ_CHUNK_HEADER_SIZE + chunk_len);
    output.push(u8::from(is_last));
    output.extend_from_slice(&(chunk_len as u16).to_le_bytes());
    output.extend_from_slice(&buffer[offset..offset + chunk_len]);
    output
}

#[must_use]
pub fn is_v3_device_command(data: &[u8], image: &[u8]) -> bool {
    data.len() == V3_DEVICE_COMMAND_SIZE && command_identity_matches(data, image) && data[10] == 0x01
}

pub fn apply_v3_device_command(data: &[u8], image: &mut [u8]) -> bool {
    is_v3_device_command(data, image)
}

#[must_use]
pub fn is_v3_write_start(data: &[u8], image: &[u8]) -> bool {
    data.len() >= 22
        && command_identity_matches(data, image)
        && data[10] == 0x06
        && (1..=16).contains(&data[21])
}

#[must_use]
pub fn v3_extended_expected_size(data: &[u8], image: &[u8]) -> usize {
    if data.len() < 26 || !command_identity_matches(data, image) || data[10] != 0x06 {
        return 0;
    }
    if data[11..22].iter().all(|value| *value == 0)
        && data[22] == 0x02
        && data[23] == 0x00
        && data[24] == 0x92
        && data[25] == 0xf0
    {
        return V3_EXTENDED_CLEAR_SIZE;
    }
    let Some(layout) = extended_update_layout(data) else {
        return 0;
    };
    let next_capability = &data[18..22];
    let expected_generation = current_capability_generation(image, layout).wrapping_add(1);
    if data[11..13] == [0x01, 0x01]
        && data[14..18] == [0xff, 0xff, 0xff, 0xff]
        && sector1_capability_valid(next_capability)
        && next_capability[2] == expected_generation
    {
        V3_EXTENDED_UPDATE_SIZE
    } else {
        0
    }
}

#[must_use]
pub fn apply_v3_write_staging(
    staging: &[u8],
    coverage: &[u8],
    image: &mut [u8],
) -> WriteApplyResult {
    if let Err(error) = validate_v3_dump(image) {
        return WriteApplyResult::failed(error.to_string());
    }
    if staging.len() < WRITE_STAGING_SIZE || !all_covered(coverage, WRITE_STAGING_SIZE) {
        return WriteApplyResult::failed("incomplete staging stream");
    }
    if !is_v3_write_start(staging, image) {
        return WriteApplyResult::failed("invalid v3 write start envelope");
    }
    let record_count = usize::from(staging[21]);
    if record_count == 0 || record_count > 16 {
        return WriteApplyResult::failed("invalid record count");
    }
    let mut cursor = 22usize;
    let mut data_bytes = 0usize;
    for _ in 0..record_count {
        if cursor + 2 > WRITE_STAGING_SIZE {
            return WriteApplyResult::failed("record overruns staging buffer");
        }
        let page = staging[cursor];
        let length = usize::from(staging[cursor + 1]);
        cursor += 2;
        if page == 0 || length == 0 || cursor + length > WRITE_STAGING_SIZE {
            return WriteApplyResult::failed("invalid record page/length");
        }
        let address = usize::from(page) * 4;
        if address < 20 || address + length > V3_WRITE_END {
            return WriteApplyResult::failed("record address out of v3 mutable bounds");
        }
        cursor += length;
        data_bytes += length;
    }
    if staging[cursor..WRITE_STAGING_SIZE].iter().any(|value| *value != 0) {
        return WriteApplyResult::failed("non-zero trailing padding in staging buffer");
    }
    image[16..20].copy_from_slice(&staging[17..21]);
    cursor = 22;
    for _ in 0..record_count {
        let page = staging[cursor];
        let length = usize::from(staging[cursor + 1]);
        cursor += 2;
        let address = usize::from(page) * 4;
        image[address..address + length].copy_from_slice(&staging[cursor..cursor + length]);
        cursor += length;
    }
    WriteApplyResult::success(record_count, data_bytes)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct V3ExtendedRecordSpec {
    sector: u8,
    page: u8,
    length: u8,
}

const CLEAR_RECORDS: [V3ExtendedRecordSpec; 2] = [
    V3ExtendedRecordSpec {
        sector: 0x00,
        page: 0x92,
        length: 0xf0,
    },
    V3ExtendedRecordSpec {
        sector: 0x00,
        page: 0xce,
        length: 0x50,
    },
];

const UPDATE_CONTROL_RECORD: V3ExtendedRecordSpec = V3ExtendedRecordSpec {
    sector: 0x00,
    page: 0x04,
    length: 0x04,
};

#[must_use]
pub fn apply_v3_extended_staging(
    staging: &[u8],
    coverage: &[u8],
    expected_size: usize,
    image: &mut [u8],
) -> WriteApplyResult {
    if let Err(error) = validate_v3_dump(image) {
        return WriteApplyResult::failed(error.to_string());
    }
    if expected_size != V3_EXTENDED_CLEAR_SIZE && expected_size != V3_EXTENDED_UPDATE_SIZE {
        return WriteApplyResult::failed("invalid expected extended operation size");
    }
    if staging.len() < expected_size || !all_covered(coverage, expected_size) {
        return WriteApplyResult::failed("incomplete extended staging stream");
    }
    if v3_extended_expected_size(&staging[..expected_size], image) != expected_size {
        return WriteApplyResult::failed("staging content does not match expected extended operation format");
    }

    let records = if expected_size == V3_EXTENDED_UPDATE_SIZE {
        let Some(layout) = extended_update_layout(&staging[..expected_size]) else {
            return WriteApplyResult::failed("invalid extended update layout");
        };
        vec![
            UPDATE_CONTROL_RECORD,
            V3ExtendedRecordSpec {
                sector: 0,
                page: layout.sector0_page,
                length: 0x20,
            },
            V3ExtendedRecordSpec {
                sector: 1,
                page: layout.sector1_capability_page.wrapping_add(1),
                length: 0x60,
            },
        ]
    } else {
        CLEAR_RECORDS.to_vec()
    };

    let mut cursor = 23usize;
    let mut data_bytes = 0usize;
    for spec in &records {
        if cursor + 3 > expected_size
            || staging[cursor] != spec.sector
            || staging[cursor + 1] != spec.page
            || staging[cursor + 2] != spec.length
        {
            return WriteApplyResult::failed("record descriptor mismatch");
        }
        cursor += 3;
        let length = usize::from(spec.length);
        if cursor + length > expected_size {
            return WriteApplyResult::failed("record data overruns expected size");
        }
        let address = usize::from(spec.sector) * 0x400 + usize::from(spec.page) * 4;
        if address + length > V3_DUMP_SIZE {
            return WriteApplyResult::failed("record address out of bounds");
        }
        cursor += length;
        data_bytes += length;
    }
    if staging[cursor..expected_size].iter().any(|value| *value != 0) {
        return WriteApplyResult::failed("trailing non-zero bytes in extended staging");
    }

    cursor = 23;
    if expected_size == V3_EXTENDED_UPDATE_SIZE {
        let capability_offset = 0x400 + usize::from(staging[13]) * 4;
        image[capability_offset..capability_offset + 4].copy_from_slice(&staging[18..22]);
    }
    for spec in &records {
        cursor += 3;
        let length = usize::from(spec.length);
        let address = usize::from(spec.sector) * 0x400 + usize::from(spec.page) * 4;
        if spec.sector == 0 && spec.page == 0x04 {
            image[address + 2..address + 4].copy_from_slice(&staging[cursor + 2..cursor + 4]);
        } else {
            image[address..address + length].copy_from_slice(&staging[cursor..cursor + length]);
        }
        cursor += length;
    }
    WriteApplyResult::success(records.len(), data_bytes)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NfcReply {
    direction: u8,
    payload: Vec<u8>,
}

impl NfcReply {
    fn ack() -> Self {
        Self {
            direction: 0x04,
            payload: Vec::new(),
        }
    }

    fn input(payload: Vec<u8>) -> Self {
        Self {
            direction: 0x01,
            payload,
        }
    }

    #[must_use]
    pub const fn direction(&self) -> u8 {
        self.direction
    }

    #[must_use]
    pub fn payload(&self) -> &[u8] {
        &self.payload
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum V3ExtendedPhase {
    #[default]
    Idle,
    AwaitUpdate,
    UpdateCommitted,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Ntag215Runtime {
    nfc_status: u8,
    nfc_detail: u8,
    operation_active: bool,
    write_mode: bool,
    write_staging: [u8; WRITE_STAGING_SIZE],
    write_coverage: [u8; WRITE_STAGING_SIZE],
    write_committed: bool,
    operation_metadata: [u8; 9],
    op_buffer: Vec<u8>,
    tag_ejected: bool,
    represent_cooldown_until_ms: u64,
}

impl Default for Ntag215Runtime {
    fn default() -> Self {
        Self {
            nfc_status: 0x09,
            nfc_detail: 0,
            operation_active: false,
            write_mode: false,
            write_staging: [0; WRITE_STAGING_SIZE],
            write_coverage: [0; WRITE_STAGING_SIZE],
            write_committed: false,
            operation_metadata: [0; 9],
            op_buffer: Vec::new(),
            tag_ejected: false,
            represent_cooldown_until_ms: 0,
        }
    }
}

impl Ntag215Runtime {
    fn init(&mut self) {
        *self = Self::default();
    }

    fn reset_transaction(&mut self) {
        self.operation_active = false;
        self.write_mode = false;
        self.nfc_status = 0x09;
        self.nfc_detail = 0;
        self.write_committed = false;
        self.tag_ejected = false;
        self.represent_cooldown_until_ms = 0;
    }

    fn step(&mut self, now_ms: u64, sub: u8, req: &[u8], image: &mut Vec<u8>) -> NfcReply {
        let mut reply = NfcReply::ack();
        match sub {
            0x03 => {
                if self.tag_ejected && now_ms >= self.represent_cooldown_until_ms {
                    self.tag_ejected = false;
                }
                self.operation_active = false;
                if self.write_mode {
                    self.write_mode = false;
                    self.write_staging.fill(0);
                    self.write_coverage.fill(0);
                }
                if !self.write_committed {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x04 => {
                self.operation_active = false;
                self.write_mode = false;
                if self.write_committed {
                    self.write_committed = false;
                    self.tag_ejected = true;
                    self.represent_cooldown_until_ms = now_ms.saturating_add(3000);
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                } else if !self.tag_ejected {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x05 => {
                let mut payload = vec![0u8; STATUS_PAYLOAD_SIZE];
                if !self.tag_ejected && image.len() == RAW_DUMP_SIZE {
                    payload[0] = self.nfc_status;
                    payload[1] = self.nfc_detail;
                    payload[4] = 0x01;
                    payload[5] = 0x01;
                    payload[6] = 0x02;
                    payload[7] = 0x07;
                    payload[8..15].copy_from_slice(&uid_from_raw(image));
                } else {
                    payload[0] = 0x07;
                    payload[1] = 0x41;
                }
                reply = NfcReply::input(payload);
            }
            0x06 => {
                let valid = req.len() >= 19 && req[0] == 0xd0 && req[1] == 0x07 && !self.tag_ejected;
                if valid {
                    let uid = uid_from_raw(image);
                    let is_zero_uid = req[2..9].iter().all(|value| *value == 0);
                    self.write_mode = !is_zero_uid && req[2..9] == uid;
                    self.nfc_status = 0x04;
                    self.nfc_detail = 0;
                    self.operation_active = true;
                    self.write_committed = false;
                    self.operation_metadata.copy_from_slice(&req[10..19]);
                    self.op_buffer = build_read_buffer_payload(
                        image,
                        &[0; ORIGINALITY_SIGNATURE_SIZE],
                        &self.operation_metadata,
                        self.write_mode,
                    )
                    .unwrap_or_default();
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                    self.write_mode = false;
                }
            }
            0x15 => {
                if self.operation_active && self.op_buffer.len() == READ_PAYLOAD_SIZE {
                    reply = NfcReply::input(self.op_buffer.clone());
                } else {
                    reply.direction = 0x01;
                }
            }
            0x14 => {
                if req.len() >= 4 && !self.tag_ejected && self.operation_active {
                    let offset = usize::from(u16::from_le_bytes([req[0], req[1]]));
                    let declared = usize::from(u16::from_le_bytes([req[2], req[3]]));
                    if offset + declared <= WRITE_STAGING_SIZE && req.len() >= 4 + declared {
                        self.write_staging[offset..offset + declared]
                            .copy_from_slice(&req[4..4 + declared]);
                        self.write_coverage[offset..offset + declared].fill(1);
                    }
                }
            }
            0x08 => {
                if self.write_mode && self.operation_active {
                    let result = apply_write_staging(&self.write_staging, &self.write_coverage, image);
                    if result.ok() {
                        self.nfc_status = 0x05;
                        self.nfc_detail = 0;
                        self.write_committed = true;
                        self.operation_active = false;
                        self.write_mode = false;
                    } else {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    }
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            _ => {}
        }
        reply
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct AmiiboV3Runtime {
    nfc_status: u8,
    nfc_detail: u8,
    operation_active: bool,
    device_cmd_staged: bool,
    write_mode: bool,
    extended_mode: bool,
    extended_expected_size: usize,
    extended_phase: V3ExtendedPhase,
    extended_deadline_ms: u64,
    write_staging: [u8; WRITE_STAGING_SIZE],
    write_coverage: [u8; WRITE_STAGING_SIZE],
    write_committed: bool,
    op_buffer: Vec<u8>,
    tag_ejected: bool,
    represent_cooldown_until_ms: u64,
    signature: Signature,
}

impl Default for AmiiboV3Runtime {
    fn default() -> Self {
        Self {
            nfc_status: 0x09,
            nfc_detail: 0,
            operation_active: false,
            device_cmd_staged: false,
            write_mode: false,
            extended_mode: false,
            extended_expected_size: 0,
            extended_phase: V3ExtendedPhase::Idle,
            extended_deadline_ms: 0,
            write_staging: [0; WRITE_STAGING_SIZE],
            write_coverage: [0; WRITE_STAGING_SIZE],
            write_committed: false,
            op_buffer: Vec::new(),
            tag_ejected: false,
            represent_cooldown_until_ms: 0,
            signature: Signature::default(),
        }
    }
}

impl AmiiboV3Runtime {
    fn init(&mut self, signature: Signature) {
        *self = Self {
            signature,
            ..Self::default()
        };
    }

    fn reset_transaction(&mut self) {
        let signature = self.signature;
        *self = Self {
            signature,
            ..Self::default()
        };
    }

    fn step(&mut self, now_ms: u64, sub: u8, req: &[u8], image: &mut Vec<u8>) -> NfcReply {
        let mut reply = NfcReply::ack();
        if self.extended_phase == V3ExtendedPhase::AwaitUpdate && now_ms >= self.extended_deadline_ms {
            self.extended_phase = V3ExtendedPhase::Idle;
        }
        let stored_ns_reg = if image.len() == V3_DUMP_SIZE {
            let value = image[V3_NS_REG_OFFSET];
            image[V3_NS_REG_OFFSET] |= V3_SRAM_RF_READY;
            value
        } else {
            0
        };

        match sub {
            0x03 => {
                if self.tag_ejected && now_ms >= self.represent_cooldown_until_ms {
                    self.tag_ejected = false;
                }
                self.operation_active = false;
                if self.write_mode || self.extended_mode {
                    self.write_mode = false;
                    self.extended_mode = false;
                    self.extended_expected_size = 0;
                    self.write_staging.fill(0);
                    self.write_coverage.fill(0);
                }
                if !self.write_committed {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x04 => {
                let completed_write = self.write_committed;
                let continue_extended = completed_write
                    && self.extended_phase == V3ExtendedPhase::AwaitUpdate
                    && now_ms < self.extended_deadline_ms;
                self.operation_active = false;
                self.device_cmd_staged = false;
                self.write_mode = false;
                self.extended_mode = false;
                self.extended_expected_size = 0;
                if continue_extended {
                    self.write_committed = false;
                    self.tag_ejected = false;
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                } else if completed_write {
                    self.write_committed = false;
                    self.tag_ejected = true;
                    self.represent_cooldown_until_ms = now_ms.saturating_add(3000);
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                } else if !self.tag_ejected {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x05 => {
                let mut payload = vec![0u8; STATUS_PAYLOAD_SIZE];
                if !self.tag_ejected && image.len() == V3_DUMP_SIZE {
                    payload[0] = self.nfc_status;
                    payload[1] = self.nfc_detail;
                    payload[4] = 0x01;
                    payload[5] = 0x01;
                    payload[6] = 0x02;
                    payload[7] = 0x07;
                    payload[8..15].copy_from_slice(&image[..7]);
                    if matches!(self.nfc_status, 0x15 | 0x16 | 0x18) {
                        payload.fill(0);
                        payload[0] = self.nfc_status;
                    }
                } else {
                    payload[0] = 0x07;
                    payload[1] = 0x41;
                }
                reply = NfcReply::input(payload);
            }
            0x06 => {
                let desc_blocks = req.get(10).copied().unwrap_or(0);
                let zero_uid = req.len() >= 9 && req[2..9].iter().all(|value| *value == 0);
                let selected_uid = req.len() >= 9 && req[2..9] == image[..7];
                let valid = req.len() >= 13
                    && desc_blocks >= 1
                    && 11 + usize::from(desc_blocks) * 2 <= req.len()
                    && (zero_uid || selected_uid)
                    && !self.tag_ejected;
                if valid {
                    match build_v3_read_buffer(image, self.signature.bytes(), req) {
                        Ok(buffer) => {
                            self.op_buffer = buffer;
                            self.operation_active = true;
                            self.nfc_status = 0x04;
                            self.nfc_detail = 0;
                            self.write_committed = false;
                            self.write_mode = false;
                            self.extended_mode = false;
                            self.extended_expected_size = 0;
                        }
                        Err(_) => {
                            self.operation_active = false;
                            self.nfc_status = 0x07;
                            self.nfc_detail = 0x41;
                        }
                    }
                } else {
                    self.operation_active = false;
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            0x15 => {
                if self.operation_active && !self.op_buffer.is_empty() && req.len() >= 2 {
                    let offset = u16::from_le_bytes([req[0], req[1]]);
                    reply = NfcReply::input(build_buffer_chunk(&self.op_buffer, offset));
                    if self.nfc_status == 0x18 {
                        self.nfc_status = 0x09;
                    }
                }
            }
            0x1e => match build_v3_sector_read_buffer(image, self.signature.bytes(), req) {
                Ok(buffer) if !self.tag_ejected => {
                    self.op_buffer = buffer;
                    self.operation_active = true;
                    self.nfc_status = 0x15;
                    self.nfc_detail = 0;
                    self.write_mode = false;
                    self.extended_mode = false;
                    self.extended_expected_size = 0;
                }
                _ => {
                    self.operation_active = false;
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            },
            0x14 => {
                if req.len() < 4 || self.tag_ejected {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                } else {
                    let offset = usize::from(u16::from_le_bytes([req[0], req[1]]));
                    let declared = usize::from(u16::from_le_bytes([req[2], req[3]]));
                    let available = req.len() - 4;
                    if declared == 0 || declared > available {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    } else {
                        let data = &req[4..4 + declared];
                        if offset == 0 && is_v3_device_command(data, image) {
                            self.device_cmd_staged = true;
                        } else {
                            if !self.write_mode
                                && !self.extended_mode
                                && offset == 0
                                && self.operation_active
                                && self.nfc_status == 0x04
                            {
                                if is_v3_write_start(data, image) {
                                    self.write_mode = true;
                                    self.write_committed = false;
                                    self.write_staging.fill(0);
                                    self.write_coverage.fill(0);
                                } else {
                                    let size = v3_extended_expected_size(data, image);
                                    if size != 0 {
                                        self.extended_expected_size = size;
                                        self.extended_mode = true;
                                        self.write_staging.fill(0);
                                        self.write_coverage.fill(0);
                                    }
                                }
                            }
                            if self.operation_active
                                && (self.write_mode || self.extended_mode)
                                && self.nfc_status == 0x04
                            {
                                let max_size = if self.extended_mode {
                                    self.extended_expected_size
                                } else {
                                    WRITE_STAGING_SIZE
                                };
                                if offset + declared <= max_size {
                                    self.write_staging[offset..offset + declared].copy_from_slice(data);
                                    self.write_coverage[offset..offset + declared].fill(1);
                                } else {
                                    self.nfc_status = 0x07;
                                    self.nfc_detail = 0x41;
                                }
                            } else if !self.device_cmd_staged {
                                self.nfc_status = 0x07;
                                self.nfc_detail = 0x41;
                            }
                        }
                    }
                }
            }
            0x21 => {
                if self.device_cmd_staged {
                    if let Ok(buffer) = build_v3_device_result(image) {
                        self.op_buffer = buffer;
                        self.operation_active = true;
                        self.nfc_status = 0x18;
                        self.device_cmd_staged = false;
                    }
                }
            }
            0x08 => {
                if !self.tag_ejected
                    && self.operation_active
                    && self.write_mode
                    && self.nfc_status == 0x04
                {
                    if image.len() == V3_DUMP_SIZE {
                        image[V3_NS_REG_OFFSET] = stored_ns_reg;
                    }
                    let result = apply_v3_write_staging(&self.write_staging, &self.write_coverage, image);
                    if result.ok() {
                        self.nfc_status = 0x05;
                        self.nfc_detail = 0;
                        self.operation_active = false;
                        self.write_mode = false;
                        self.extended_expected_size = 0;
                        self.write_committed = true;
                    } else {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    }
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            0x20 => {
                if !self.tag_ejected
                    && self.operation_active
                    && self.extended_mode
                    && self.nfc_status == 0x04
                {
                    if image.len() == V3_DUMP_SIZE {
                        image[V3_NS_REG_OFFSET] = stored_ns_reg;
                    }
                    let committed_size = self.extended_expected_size;
                    let result = apply_v3_extended_staging(
                        &self.write_staging,
                        &self.write_coverage,
                        committed_size,
                        image,
                    );
                    if result.ok() {
                        self.nfc_status = 0x16;
                        self.nfc_detail = 0;
                        self.operation_active = false;
                        self.extended_mode = false;
                        self.extended_expected_size = 0;
                        if committed_size == V3_EXTENDED_CLEAR_SIZE {
                            self.extended_phase = V3ExtendedPhase::AwaitUpdate;
                            self.extended_deadline_ms = now_ms.saturating_add(5000);
                        } else if committed_size == V3_EXTENDED_UPDATE_SIZE {
                            self.extended_phase = V3ExtendedPhase::UpdateCommitted;
                            self.extended_deadline_ms = 0;
                        }
                        self.write_committed = false;
                    } else {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    }
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            _ => {}
        }
        if image.len() == V3_DUMP_SIZE {
            image[V3_NS_REG_OFFSET] = stored_ns_reg;
        }
        reply
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TagType {
    #[default]
    None,
    Ntag215,
    V3,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct S2NfcRuntime {
    tag_type: TagType,
    tag_image: Vec<u8>,
    signature: Signature,
    has_real_signature: bool,
    modified: bool,
    ntag215: Ntag215Runtime,
    v3: AmiiboV3Runtime,
}

impl S2NfcRuntime {
    pub fn set_tag_data(
        &mut self,
        data: &[u8],
        has_real_signature: bool,
        signature: Signature,
    ) -> Result<(), NfcError> {
        if !matches!(data.len(), TAGMO_DUMP_SIZE | RAW_DUMP_SIZE | EXTENDED_DUMP_SIZE | V3_DUMP_SIZE) {
            return Err(NfcError::new("unsupported Amiibo dump size"));
        }
        if data.len() == V3_DUMP_SIZE {
            validate_v3_dump(data)?;
            self.tag_image = data.to_vec();
            self.tag_type = TagType::V3;
            self.has_real_signature = has_real_signature;
            self.signature = signature;
            self.modified = false;
            self.v3.init(signature);
        } else {
            self.tag_image = vec![0u8; RAW_DUMP_SIZE];
            let raw_bytes = data.len().min(RAW_DUMP_SIZE);
            self.tag_image[..raw_bytes].copy_from_slice(&data[..raw_bytes]);
            validate_raw_dump(&self.tag_image)?;
            self.tag_type = TagType::Ntag215;
            self.has_real_signature = has_real_signature || data.len() == EXTENDED_DUMP_SIZE;
            self.signature = if data.len() == EXTENDED_DUMP_SIZE {
                Signature::from_bytes(
                    data[RAW_DUMP_SIZE..EXTENDED_DUMP_SIZE]
                        .try_into()
                        .expect("extended signature is exactly 32 bytes"),
                )
            } else {
                signature
            };
            self.modified = false;
            self.ntag215.init();
        }
        Ok(())
    }

    pub fn clear(&mut self) {
        self.tag_type = TagType::None;
        self.tag_image.clear();
        self.signature = Signature::from_bytes([0; ORIGINALITY_SIGNATURE_SIZE]);
        self.has_real_signature = false;
        self.modified = false;
        self.ntag215.reset_transaction();
        self.v3.reset_transaction();
    }

    #[must_use]
    pub fn step(&mut self, now_ms: u64, sub: u8, req: &[u8]) -> NfcReply {
        match self.tag_type {
            TagType::None => {
                if sub == 0x05 {
                    let mut payload = vec![0u8; STATUS_PAYLOAD_SIZE];
                    payload[0] = 0x07;
                    payload[1] = 0x41;
                    NfcReply::input(payload)
                } else {
                    NfcReply::ack()
                }
            }
            TagType::V3 => {
                let reply = self.v3.step(now_ms, sub, req, &mut self.tag_image);
                if self.v3.write_committed {
                    self.modified = true;
                }
                reply
            }
            TagType::Ntag215 => {
                let reply = self.ntag215.step(now_ms, sub, req, &mut self.tag_image);
                if self.ntag215.write_committed {
                    self.modified = true;
                }
                reply
            }
        }
    }

    #[must_use]
    pub fn is_placed(&self) -> bool {
        match self.tag_type {
            TagType::None => false,
            TagType::V3 => !self.v3.tag_ejected,
            TagType::Ntag215 => !self.ntag215.tag_ejected,
        }
    }

    #[must_use]
    pub const fn is_v3(&self) -> bool {
        matches!(self.tag_type, TagType::V3)
    }

    #[must_use]
    pub const fn is_modified(&self) -> bool {
        self.modified
    }

    pub fn clear_modified(&mut self) {
        self.modified = false;
    }

    #[must_use]
    pub const fn tag_type(&self) -> TagType {
        self.tag_type
    }

    #[must_use]
    pub fn image(&self) -> &[u8] {
        &self.tag_image
    }

    pub fn replace_image(&mut self, image: Vec<u8>) -> Result<(), NfcError> {
        match self.tag_type {
            TagType::V3 => validate_v3_dump(&image)?,
            TagType::Ntag215 => validate_raw_dump(&image)?,
            TagType::None => return Err(NfcError::new("cannot replace image without an active tag")),
        }
        self.tag_image = image;
        Ok(())
    }

    #[must_use]
    pub const fn signature(&self) -> &Signature {
        &self.signature
    }

    #[must_use]
    pub const fn has_real_signature(&self) -> bool {
        self.has_real_signature
    }

    #[must_use]
    pub const fn nfc_status(&self) -> u8 {
        match self.tag_type {
            TagType::V3 => self.v3.nfc_status,
            TagType::Ntag215 => self.ntag215.nfc_status,
            TagType::None => 0x07,
        }
    }

    #[must_use]
    pub const fn nfc_detail(&self) -> u8 {
        match self.tag_type {
            TagType::V3 => self.v3.nfc_detail,
            TagType::Ntag215 => self.ntag215.nfc_detail,
            TagType::None => 0x41,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_v3() -> Vec<u8> {
        let mut image = (0..V3_DUMP_SIZE).map(|index| index as u8).collect::<Vec<_>>();
        image[0] = 0x04;
        image[7] = 0x00;
        image[8] = 0x44;
        image
    }

    fn initial_read_request() -> [u8; 19] {
        [
            0xb8, 0x0b, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x04, 0x00, 0x3b, 0x3c, 0x77,
            0x78, 0x91, 0xe2, 0xe6,
        ]
    }

    fn make_raw() -> Vec<u8> {
        let mut raw = vec![0u8; RAW_DUMP_SIZE];
        raw[0] = 0x04;
        raw[1] = 0x11;
        raw[2] = 0x22;
        raw[3] = 0x88 ^ raw[0] ^ raw[1] ^ raw[2];
        raw[4] = 0x33;
        raw[5] = 0x44;
        raw[6] = 0x55;
        raw[7] = 0x66;
        raw[8] = raw[4] ^ raw[5] ^ raw[6] ^ raw[7];
        raw
    }

    #[test]
    fn cpp_v3_codec_regression_contract() {
        let mut image = make_v3();
        validate_v3_dump(&image).expect("v3 image");
        assert_eq!(uid_from_dump(&image), image[..7]);

        let signature = Signature::from_bytes([0; ORIGINALITY_SIGNATURE_SIZE]);
        let read_request = initial_read_request();
        let operation = build_v3_read_buffer(&image, signature.bytes(), &read_request).expect("read buffer");
        assert_eq!(operation.len(), 664);
        assert_eq!(operation[18], 0x06);
        assert_eq!(&operation[V3_OPERATION_PREFIX_SIZE..V3_OPERATION_PREFIX_SIZE + 240], &image[..240]);

        let chunk = build_buffer_chunk(&operation, 0);
        assert_eq!(chunk.len(), 73);
        assert_eq!(&chunk[..3], &[0, 70, 0]);
        let chunk = build_buffer_chunk(&operation, 630);
        assert_eq!(chunk.len(), 37);
        assert_eq!(&chunk[..3], &[1, 34, 0]);

        for index in 0..V3_SRAM_SIZE - 2 {
            image[V3_SRAM_OFFSET + index] = (index * 3) as u8;
        }
        let crc = crc16_mcrf4xx(&image[V3_SRAM_OFFSET..V3_SRAM_OFFSET + V3_SRAM_SIZE - 2]);
        image[V3_SRAM_OFFSET + 62] = (crc >> 8) as u8;
        image[V3_SRAM_OFFSET + 63] = crc as u8;
        assert!(v3_sram_response_valid(&image));
        let operation = build_v3_device_result(&image).expect("device result");
        assert_eq!(operation.len(), V3_DEVICE_RESULT_SIZE);
        assert_eq!(operation[0], 0x18);
        assert_eq!(operation[18], 0x06);

        let mut staging = [0u8; WRITE_STAGING_SIZE];
        let mut coverage = [1u8; WRITE_STAGING_SIZE];
        staging[2..9].copy_from_slice(&image[..7]);
        staging[9] = 0x01;
        staging[10] = 0x06;
        staging[17..21].copy_from_slice(&[0xaa, 0xbb, 0xcc, 0xdd]);
        staging[21] = 1;
        staging[22] = 5;
        staging[23] = 4;
        staging[24..28].copy_from_slice(&[1, 2, 3, 4]);
        let result = apply_v3_write_staging(&staging, &coverage, &mut image);
        assert!(result.ok());
        assert_eq!(result.record_count(), 1);
        assert_eq!(&image[20..24], &[1, 2, 3, 4]);
        assert_eq!(&image[16..20], &[0xaa, 0xbb, 0xcc, 0xdd]);

        staging.fill(0);
        coverage.fill(0);
        staging[2..9].copy_from_slice(&image[..7]);
        staging[9] = 0x01;
        staging[10] = 0x06;
        staging[22] = 2;
        let mut cursor = 23;
        staging[cursor..cursor + 3].copy_from_slice(&[0x00, 0x92, 0xf0]);
        cursor += 3;
        staging[cursor..cursor + 0xf0].fill(0x11);
        cursor += 0xf0;
        staging[cursor..cursor + 3].copy_from_slice(&[0x00, 0xce, 0x50]);
        cursor += 3;
        staging[cursor..cursor + 0x50].fill(0x22);
        coverage[..V3_EXTENDED_CLEAR_SIZE].fill(1);
        assert_eq!(
            v3_extended_expected_size(&staging[..70], &image),
            V3_EXTENDED_CLEAR_SIZE
        );
        let result = apply_v3_extended_staging(
            &staging,
            &coverage,
            V3_EXTENDED_CLEAR_SIZE,
            &mut image,
        );
        assert!(result.ok());
        assert_eq!(result.record_count(), 2);
        assert_eq!(image[0x92 * 4], 0x11);
        assert_eq!(image[0xce * 4], 0x22);

        let mut sector_request = [0u8; 23];
        sector_request[2..9].copy_from_slice(&image[..7]);
        sector_request[9] = 0x01;
        sector_request[10] = 2;
        sector_request[11..17].copy_from_slice(&[0, 0x92, 0x99, 1, 0, 0x18]);
        let operation = build_v3_sector_read_buffer(&image, signature.bytes(), &sector_request)
            .expect("sector read");
        assert_eq!(operation.len(), 196);
        assert_eq!(operation[0], 0x15);
        assert_eq!(operation[18], 0x06);
        assert_eq!(operation[96], 0xa5);
        assert_eq!(operation[98], 0x01);

        staging.fill(0);
        coverage.fill(0);
        staging[2..9].copy_from_slice(&image[..7]);
        staging[9] = 0x01;
        staging[10] = 0x06;
        staging[11] = 0x01;
        staging[12] = 0x01;
        staging[13] = 0x64;
        staging[14..18].fill(0xff);
        staging[18] = 0xa5;
        staging[20] = 0x02;
        staging[22] = 3;
        staging[23..26].copy_from_slice(&[0x00, 0x04, 0x04]);
        staging[26] = 0xa5;
        staging[28] = 0x07;
        staging[30] = 0x00;
        staging[31] = 0xb2;
        staging[32] = 0x20;
        staging[33..65].fill(0x33);
        staging[65..68].copy_from_slice(&[0x01, 0x65, 0x60]);
        staging[68..164].fill(0x44);
        coverage[..V3_EXTENDED_UPDATE_SIZE].fill(1);
        assert_eq!(
            v3_extended_expected_size(&staging[..70], &image),
            V3_EXTENDED_UPDATE_SIZE
        );
        let result = apply_v3_extended_staging(
            &staging,
            &coverage,
            V3_EXTENDED_UPDATE_SIZE,
            &mut image,
        );
        assert!(result.ok());
        assert_eq!(result.record_count(), 3);
        assert_eq!(image[0xb2 * 4], 0x33);
        assert_eq!(image[0x400 + 0x65 * 4], 0x44);
        let capability = 0x400 + 0x64 * 4;
        assert_eq!(image[capability], 0xa5);
        assert_eq!(image[capability + 2], 0x02);

        sector_request[12] = 0xb2;
        sector_request[13] = 0xb9;
        sector_request[15] = 0x64;
        sector_request[16] = 0x7c;
        let operation = build_v3_sector_read_buffer(&image, signature.bytes(), &sector_request)
            .expect("second sector read");
        assert_eq!(operation.len(), 196);
        assert_eq!(operation[96], 0xa5);
        assert_eq!(operation[98], 0x02);
    }

    #[test]
    fn runtime_fails_closed_without_active_write_operation() {
        let raw = make_raw();
        let mut runtime = S2NfcRuntime::default();
        runtime
            .set_tag_data(&raw, false, Signature::from_bytes([0; 32]))
            .expect("tag");
        let mut chunk = vec![0u8; 8];
        chunk[2..4].copy_from_slice(&4u16.to_le_bytes());
        runtime.step(0, 0x14, &chunk);
        assert_eq!(runtime.nfc_status(), 0x09);
        runtime.step(0, 0x08, &[]);
        assert_eq!(runtime.nfc_status(), 0x07);
        assert_eq!(runtime.nfc_detail(), 0x41);
    }

    #[test]
    fn extended_dump_preserves_real_signature() {
        let raw = make_raw();
        let mut extended = raw.clone();
        extended.extend_from_slice(&[0x5a; ORIGINALITY_SIGNATURE_SIZE]);
        let mut runtime = S2NfcRuntime::default();
        runtime
            .set_tag_data(&extended, false, Signature::from_bytes([0; 32]))
            .expect("extended tag");
        assert!(runtime.has_real_signature());
        assert_eq!(runtime.signature().bytes(), &[0x5a; ORIGINALITY_SIGNATURE_SIZE]);
    }
}
