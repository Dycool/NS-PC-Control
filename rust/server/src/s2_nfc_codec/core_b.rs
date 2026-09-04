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

