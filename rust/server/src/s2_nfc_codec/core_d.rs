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
