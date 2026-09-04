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

