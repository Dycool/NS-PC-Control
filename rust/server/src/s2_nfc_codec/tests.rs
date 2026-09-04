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
        let _ = runtime.step(0, 0x14, &chunk);
        assert_eq!(runtime.nfc_status(), 0x09);
        let _ = runtime.step(0, 0x08, &[]);
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
