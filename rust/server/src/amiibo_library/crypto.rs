pub const RETAIL_KEY_SIZE: usize = 160;
const MASTER_KEY_SIZE: usize = 80;
const INTERNAL_SIZE: usize = 520;
const RAW_SIZE: usize = 540;
const V3_SIZE: usize = 2048;
const CIPHER_OFFSET: usize = 0x02c;
const CIPHER_LENGTH: usize = 0x188;
const DATA_HMAC_POS: usize = 0x008;
const TAG_HMAC_POS: usize = 0x1b4;
const FORMAT_REQUEST_FLAG: u32 = 0x8000_0000;

const AES_SBOX: [u8; 256] = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
];
const AES_RCON: [u8; 10] = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36];

fn validate_key(key: &[u8]) -> bool {
    key.len() == RETAIL_KEY_SIZE && key[31] <= 16 && key[MASTER_KEY_SIZE + 31] <= 16
}

fn prepare_seed(master: &[u8; MASTER_KEY_SIZE], base_seed: &[u8; 64]) -> Vec<u8> {
    let mut seed = Vec::with_capacity(79);
    for byte in &master[16..30] {
        seed.push(*byte);
        if *byte == 0 {
            break;
        }
    }
    let magic_size = usize::from(master[31]);
    seed.extend_from_slice(&base_seed[..16 - magic_size]);
    seed.extend_from_slice(&master[32..32 + magic_size]);
    seed.extend_from_slice(&base_seed[16..32]);
    for (base, mask) in base_seed[32..].iter().zip(&master[48..]) {
        seed.push(*base ^ *mask);
    }
    seed
}

fn derive_keys(master: &[u8; MASTER_KEY_SIZE], base_seed: &[u8; 64]) -> [u8; 48] {
    let prepared = prepare_seed(master, base_seed);
    let mut derived = [0u8; 48];
    let mut written = 0usize;
    let mut counter = 0u16;
    while written < derived.len() {
        let mut input = Vec::with_capacity(2 + prepared.len());
        input.extend_from_slice(&counter.to_be_bytes());
        input.extend_from_slice(&prepared);
        counter = counter.wrapping_add(1);
        let block = hmac_sha256(&master[..16], &input);
        let take = block.len().min(derived.len() - written);
        derived[written..written + take].copy_from_slice(&block[..take]);
        written += take;
    }
    derived
}

fn calculate_seed(internal: &[u8]) -> Result<[u8; 64], String> {
    if internal.len() < INTERNAL_SIZE {
        return Err("internal Amiibo buffer is too short".to_owned());
    }
    let mut seed = [0u8; 64];
    seed[..2].copy_from_slice(&internal[0x029..0x02b]);
    seed[0x10..0x18].copy_from_slice(&internal[0x1d4..0x1dc]);
    seed[0x18..0x20].copy_from_slice(&internal[0x1d4..0x1dc]);
    seed[0x20..0x40].copy_from_slice(&internal[0x1e8..0x208]);
    Ok(seed)
}

fn aes_expand_key(key: &[u8; 16]) -> [u8; 176] {
    let mut expanded = [0u8; 176];
    expanded[..16].copy_from_slice(key);
    let mut generated = 16usize;
    let mut rcon_index = 0usize;
    let mut temp = [0u8; 4];
    while generated < expanded.len() {
        temp.copy_from_slice(&expanded[generated - 4..generated]);
        if generated.is_multiple_of(16) {
            temp.rotate_left(1);
            for value in &mut temp {
                *value = AES_SBOX[usize::from(*value)];
            }
            temp[0] ^= AES_RCON[rcon_index];
            rcon_index += 1;
        }
        for value in temp {
            expanded[generated] = expanded[generated - 16] ^ value;
            generated += 1;
        }
    }
    expanded
}

fn aes_add_round_key(state: &mut [u8; 16], round_key: &[u8]) {
    for (value, key) in state.iter_mut().zip(round_key) {
        *value ^= *key;
    }
}

fn aes_sub_bytes(state: &mut [u8; 16]) {
    for value in state {
        *value = AES_SBOX[usize::from(*value)];
    }
}

fn aes_shift_rows(state: &mut [u8; 16]) {
    let old = *state;
    state[0] = old[0]; state[4] = old[4]; state[8] = old[8]; state[12] = old[12];
    state[1] = old[5]; state[5] = old[9]; state[9] = old[13]; state[13] = old[1];
    state[2] = old[10]; state[6] = old[14]; state[10] = old[2]; state[14] = old[6];
    state[3] = old[15]; state[7] = old[3]; state[11] = old[7]; state[15] = old[11];
}

fn aes_xtime(value: u8) -> u8 {
    (value << 1) ^ if value & 0x80 != 0 { 0x1b } else { 0 }
}

fn aes_mix_columns(state: &mut [u8; 16]) {
    let (columns, _) = state.as_chunks_mut::<4>();
    for column in columns {
        let a = [column[0], column[1], column[2], column[3]];
        let t = a[0] ^ a[1] ^ a[2] ^ a[3];
        column[0] = a[0] ^ t ^ aes_xtime(a[0] ^ a[1]);
        column[1] = a[1] ^ t ^ aes_xtime(a[1] ^ a[2]);
        column[2] = a[2] ^ t ^ aes_xtime(a[2] ^ a[3]);
        column[3] = a[3] ^ t ^ aes_xtime(a[3] ^ a[0]);
    }
}

fn aes128_encrypt_block(key: &[u8; 16], input: &[u8; 16]) -> [u8; 16] {
    let expanded = aes_expand_key(key);
    let mut state = *input;
    aes_add_round_key(&mut state, &expanded[..16]);
    for round in 1..10 {
        aes_sub_bytes(&mut state);
        aes_shift_rows(&mut state);
        aes_mix_columns(&mut state);
        aes_add_round_key(&mut state, &expanded[round * 16..(round + 1) * 16]);
    }
    aes_sub_bytes(&mut state);
    aes_shift_rows(&mut state);
    aes_add_round_key(&mut state, &expanded[160..176]);
    state
}

fn increment_counter(counter: &mut [u8; 16]) {
    for byte in counter.iter_mut().rev() {
        let (next, carry) = byte.overflowing_add(1);
        *byte = next;
        if !carry {
            break;
        }
    }
}

fn aes128_ctr_xor(key: &[u8; 16], iv: &[u8; 16], input: &[u8], output: &mut [u8]) -> Result<(), String> {
    if input.len() != output.len() {
        return Err("AES-CTR input/output sizes differ".to_owned());
    }
    let mut counter = *iv;
    for (source, destination) in input.chunks(16).zip(output.chunks_mut(16)) {
        let stream = aes128_encrypt_block(key, &counter);
        for (index, value) in source.iter().copied().enumerate() {
            destination[index] = value ^ stream[index];
        }
        increment_counter(&mut counter);
    }
    Ok(())
}

fn internal_to_tag(internal: &[u8], tag: &mut [u8], v3: bool) -> Result<(), String> {
    if internal.len() < INTERNAL_SIZE || tag.len() < if v3 { V3_SIZE } else { RAW_SIZE } {
        return Err("Amiibo tag/internal buffer is too short".to_owned());
    }
    tag[0x008..0x010].copy_from_slice(&internal[0x000..0x008]);
    let user_offset = if v3 { 0x0c0 } else { 0x080 };
    tag[user_offset..user_offset + 0x020].copy_from_slice(&internal[0x008..0x028]);
    tag[0x010..0x034].copy_from_slice(&internal[0x028..0x04c]);
    let data_offset = if v3 { 0x0e0 } else { 0x0a0 };
    tag[data_offset..data_offset + 0x168].copy_from_slice(&internal[0x04c..0x1b4]);
    tag[0x034..0x054].copy_from_slice(&internal[0x1b4..0x1d4]);
    tag[0x000..0x008].copy_from_slice(&internal[0x1d4..0x1dc]);
    tag[0x054..0x080].copy_from_slice(&internal[0x1dc..0x208]);
    Ok(())
}

fn pack_tag(retail_key: &[u8; RETAIL_KEY_SIZE], plain: &[u8], tag: &mut [u8], v3: bool) -> Result<(), String> {
    if plain.len() < INTERNAL_SIZE {
        return Err("plain Amiibo template is too short".to_owned());
    }
    let seed = calculate_seed(plain)?;
    let data_master: &[u8; MASTER_KEY_SIZE] = retail_key[..MASTER_KEY_SIZE]
        .try_into()
        .expect("retail key first master record is 80 bytes");
    let tag_master: &[u8; MASTER_KEY_SIZE] = retail_key[MASTER_KEY_SIZE..]
        .try_into()
        .expect("retail key second master record is 80 bytes");
    let tag_keys = derive_keys(tag_master, &seed);
    let data_keys = derive_keys(data_master, &seed);

    let mut cipher = [0u8; INTERNAL_SIZE];
    let tag_hmac = hmac_sha256(&tag_keys[32..48], &plain[0x1d4..0x208]);
    cipher[TAG_HMAC_POS..TAG_HMAC_POS + 32].copy_from_slice(&tag_hmac);

    let mut data_hmac_input = Vec::with_capacity(0x18b + 0x20 + 0x34);
    data_hmac_input.extend_from_slice(&plain[0x029..0x1b4]);
    data_hmac_input.extend_from_slice(&tag_hmac);
    data_hmac_input.extend_from_slice(&plain[0x1d4..0x208]);
    let data_hmac = hmac_sha256(&data_keys[32..48], &data_hmac_input);
    cipher[DATA_HMAC_POS..DATA_HMAC_POS + 32].copy_from_slice(&data_hmac);

    let key: &[u8; 16] = data_keys[..16].try_into().expect("derived AES key is 16 bytes");
    let iv: &[u8; 16] = data_keys[16..32].try_into().expect("derived AES IV is 16 bytes");
    aes128_ctr_xor(
        key,
        iv,
        &plain[CIPHER_OFFSET..CIPHER_OFFSET + CIPHER_LENGTH],
        &mut cipher[CIPHER_OFFSET..CIPHER_OFFSET + CIPHER_LENGTH],
    )?;
    cipher[..8].copy_from_slice(&plain[..8]);
    cipher[0x028..0x02c].copy_from_slice(&plain[0x028..0x02c]);
    cipher[0x1d4..0x208].copy_from_slice(&plain[0x1d4..0x208]);
    internal_to_tag(&cipher, tag, v3)
}

fn fill_random(bytes: &mut [u8]) -> Result<(), String> {
    let mut source = File::open("/dev/urandom")
        .map_err(|error| format!("cannot open operating-system random source: {error}"))?;
    source
        .read_exact(bytes)
        .map_err(|error| format!("cannot read operating-system random source: {error}"))
}

fn write_be32(target: &mut [u8], value: u32) {
    target.copy_from_slice(&value.to_be_bytes());
}

fn generate_tag(head: u32, tail: u32, retail_key: &[u8; RETAIL_KEY_SIZE]) -> Result<Vec<u8>, String> {
    let mut plain = [0u8; RAW_SIZE];
    let mut uid = [0u8; 7];
    uid[0] = 0x04;
    fill_random(&mut uid[1..])?;
    fill_random(&mut plain[0x1e8..0x208])?;

    write_be32(&mut plain[0x1dc..0x1e0], head);
    write_be32(&mut plain[0x1e0..0x1e4], tail);
    write_be32(&mut plain[0x054..0x058], head);
    write_be32(&mut plain[0x058..0x05c], tail);

    const INTERNAL_STATIC_LOCK: [u8; 8] = [0x65,0x48,0x0f,0xe0,0xf1,0x10,0xff,0xee];
    plain[..8].copy_from_slice(&INTERNAL_STATIC_LOCK);
    plain[0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
    plain[0x1d4] = uid[0];
    plain[0x1d5] = uid[1];
    plain[0x1d6] = uid[2];
    plain[0x1d7] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    plain[0x1d8..0x1dc].copy_from_slice(&uid[3..]);
    plain[0x028..0x02c].copy_from_slice(&[0xa5,0x00,0x00,0x00]);

    let v3 = tail & 0xff == 0x03;
    let mut tag = vec![0u8; if v3 { V3_SIZE } else { RAW_SIZE }];
    pack_tag(retail_key, &plain, &mut tag, v3)?;
    const LOCK: [u8; 20] = [
        0x01,0x00,0x0f,0xbd,0x00,0x00,0x00,0x04,0x5f,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    ];
    let lock_offset = if v3 { 0x248 } else { INTERNAL_SIZE };
    tag[lock_offset..lock_offset + LOCK.len()].copy_from_slice(&LOCK);
    if !v3 {
        return Ok(tag);
    }

    tag[0x25c..0x388].fill(0);
    tag[0x388..0x38c].copy_from_slice(&[0x01,0x00,0xff,0x00]);
    tag[0x38c..0x390].copy_from_slice(&[0x00,0x00,0x00,0x04]);
    tag[0x390..0x394].copy_from_slice(&[0x07,0x00,0x00,0x00]);
    tag[0x394..0x3b0].fill(0);
    tag[0x3b0..0x3b4].copy_from_slice(&[0x41,0x00,0xf8,0x48]);
    tag[0x3b4..0x3b8].copy_from_slice(&[0x08,0x01,0x29,0x00]);
    tag[0x3b8..0x400].fill(0);
    let crc = crc16_mcrf4xx(&tag[0x3c0..0x3fe]);
    tag[0x3fe] = (crc >> 8) as u8;
    tag[0x3ff] = crc as u8;
    tag[..7].copy_from_slice(&uid);
    tag[7] = 0;
    tag[8] = 0x44;
    Ok(tag)
}
