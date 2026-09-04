#[cfg(test)]
mod tests {
    use super::*;
    use crate::s2_nfc_codec::{validate_v3_dump, V3_DUMP_SIZE};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_serial() -> &'static Mutex<()> {
        static SERIAL: OnceLock<Mutex<()>> = OnceLock::new();
        SERIAL.get_or_init(|| Mutex::new(()))
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    #[test]
    fn aes128_known_vectors() {
        let key = [
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        ];
        let input = [
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
            0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
        ];
        assert_eq!(
            hex(&aes128_encrypt_block(&key, &input)),
            "69c4e0d86a7b0430d8cdb78070b4c55a"
        );

        let ctr_key = [
            0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
            0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c,
        ];
        let iv = [
            0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
            0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
        ];
        let plaintext = [
            0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
            0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
            0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,
            0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
        ];
        let mut output = [0u8; 32];
        aes128_ctr_xor(&ctr_key, &iv, &plaintext, &mut output).expect("CTR vector");
        assert_eq!(
            hex(&output),
            "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
        );
    }

    #[test]
    fn cpp_amiibo_library_regression_contract() {
        let _serial = test_serial()
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("ns-pc-control-rust-amiibo-{nonce}"));
        {
            let mut override_root = test_data_root()
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            *override_root = Some(root.clone());
        }
        let _ = clear();

        let mut key = [0u8; RETAIL_KEY_SIZE];
        for (index, byte) in key.iter_mut().enumerate() {
            *byte = index as u8;
        }
        key[31] = 0;
        key[MASTER_KEY_SIZE + 31] = 0;

        let (generated_result, generated) = generate_template(0, 0x0034_0102, &key);
        assert!(generated_result.is_ok(), "{}", generated_result.detail());
        assert_eq!(generated.len(), RAW_SIZE);
        assert_eq!(generated[0], 0x04);

        let (selected_result, mut selected) = select(0, 0x0034_0102, 0, &generated);
        assert!(selected_result.is_ok(), "{}", selected_result.detail());
        assert_eq!(selected.len(), RAW_SIZE);
        selected[100] ^= 0x5a;
        store_writeback(0, &selected).expect("writeback");
        let (reselected_result, reselected) = select(0, 0x0034_0102, 0, &generated);
        assert!(reselected_result.is_ok(), "{}", reselected_result.detail());
        assert_eq!(reselected[100], selected[100]);

        let (v3_result, v3) = generate_template(0x1234_5678, 0x0000_0003, &key);
        assert!(v3_result.is_ok(), "{}", v3_result.detail());
        assert_eq!(v3.len(), V3_DUMP_SIZE);
        validate_v3_dump(&v3).expect("generated V3 tag");
        assert_eq!(v3[0x388], 0x01);
        assert_eq!(v3[0x3b0], 0x41);

        assert!(clear().is_ok());
        let (missing_result, missing) = select(0, 0x0034_0102, 0, &[]);
        assert_eq!(missing_result.code(), AmiiboLibraryResult::GenerationError);
        assert!(missing.is_empty());

        {
            let mut override_root = test_data_root()
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            *override_root = None;
        }
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn bundle_parser_matches_cpp_layout() {
        let tag = vec![0xa5; RAW_SIZE];
        let mut bundle = Vec::new();
        bundle.extend_from_slice(b"NSAT");
        bundle.extend_from_slice(&[1, 0, 16, 0]);
        bundle.extend_from_slice(&1u32.to_le_bytes());
        bundle.extend_from_slice(&0x1234_5678u32.to_le_bytes());
        bundle.extend_from_slice(&0x9abc_def0u32.to_le_bytes());
        bundle.extend_from_slice(&28u32.to_le_bytes());
        bundle.extend_from_slice(&(RAW_SIZE as u32).to_le_bytes());
        bundle.extend_from_slice(&tag);
        assert_eq!(parse_template_bundle(&bundle, 0x1234_5678, 0x9abc_def0), Some(tag));
    }
}
