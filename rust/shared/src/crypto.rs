//! Dependency-free SHA-256 and HMAC-SHA256 used by the legacy C++ protocol.

const BLOCK_BYTES: usize = 64;
const DIGEST_BYTES: usize = 32;

const INITIAL_STATE: [u32; 8] = [
    0x6a09_e667,
    0xbb67_ae85,
    0x3c6e_f372,
    0xa54f_f53a,
    0x510e_527f,
    0x9b05_688c,
    0x1f83_d9ab,
    0x5be0_cd19,
];

const ROUND_CONSTANTS: [u32; 64] = [
    0x428a_2f98, 0x7137_4491, 0xb5c0_fbcf, 0xe9b5_dba5, 0x3956_c25b, 0x59f1_11f1,
    0x923f_82a4, 0xab1c_5ed5, 0xd807_aa98, 0x1283_5b01, 0x2431_85be, 0x550c_7dc3,
    0x72be_5d74, 0x80de_b1fe, 0x9bdc_06a7, 0xc19b_f174, 0xe49b_69c1, 0xefbe_4786,
    0x0fc1_9dc6, 0x240c_a1cc, 0x2de9_2c6f, 0x4a74_84aa, 0x5cb0_a9dc, 0x76f9_88da,
    0x983e_5152, 0xa831_c66d, 0xb003_27c8, 0xbf59_7fc7, 0xc6e0_0bf3, 0xd5a7_9147,
    0x06ca_6351, 0x1429_2967, 0x27b7_0a85, 0x2e1b_2138, 0x4d2c_6dfc, 0x5338_0d13,
    0x650a_7354, 0x766a_0abb, 0x81c2_c92e, 0x9272_2c85, 0xa2bf_e8a1, 0xa81a_664b,
    0xc24b_8b70, 0xc76c_51a3, 0xd192_e819, 0xd699_0624, 0xf40e_3585, 0x106a_a070,
    0x19a4_c116, 0x1e37_6c08, 0x2748_774c, 0x34b0_bcb5, 0x391c_0cb3, 0x4ed8_aa4a,
    0x5b9c_ca4f, 0x682e_6ff3, 0x748f_82ee, 0x78a5_636f, 0x84c8_7814, 0x8cc7_0208,
    0x90be_fffa, 0xa450_6ceb, 0xbef9_a3f7, 0xc671_78f2,
];

#[derive(Clone)]
struct Sha256 {
    state: [u32; 8],
    buffer: [u8; BLOCK_BYTES],
    buffer_len: usize,
    total_len: u64,
}

impl Default for Sha256 {
    fn default() -> Self {
        Self {
            state: INITIAL_STATE,
            buffer: [0; BLOCK_BYTES],
            buffer_len: 0,
            total_len: 0,
        }
    }
}

impl Sha256 {
    fn update(&mut self, mut data: &[u8]) {
        self.total_len = self
            .total_len
            .checked_add(u64::try_from(data.len()).unwrap_or(u64::MAX))
            .unwrap_or(u64::MAX);

        if self.buffer_len != 0 {
            let take = (BLOCK_BYTES - self.buffer_len).min(data.len());
            self.buffer[self.buffer_len..self.buffer_len + take].copy_from_slice(&data[..take]);
            self.buffer_len += take;
            data = &data[take..];
            if self.buffer_len == BLOCK_BYTES {
                let block = self.buffer;
                self.transform(&block);
                self.buffer_len = 0;
            }
        }

        while data.len() >= BLOCK_BYTES {
            let (block, rest) = data.split_at(BLOCK_BYTES);
            let block: &[u8; BLOCK_BYTES] = block
                .try_into()
                .expect("split_at returned a complete SHA-256 block");
            self.transform(block);
            data = rest;
        }

        self.buffer[..data.len()].copy_from_slice(data);
        self.buffer_len = data.len();
    }

    fn transform(&mut self, block: &[u8; BLOCK_BYTES]) {
        let mut schedule = [0u32; 64];
        for (index, chunk) in block.chunks_exact(4).enumerate() {
            schedule[index] = u32::from_be_bytes(
                chunk
                    .try_into()
                    .expect("chunks_exact(4) always yields four bytes"),
            );
        }
        for index in 16..64 {
            let s0 = schedule[index - 15].rotate_right(7)
                ^ schedule[index - 15].rotate_right(18)
                ^ (schedule[index - 15] >> 3);
            let s1 = schedule[index - 2].rotate_right(17)
                ^ schedule[index - 2].rotate_right(19)
                ^ (schedule[index - 2] >> 10);
            schedule[index] = schedule[index - 16]
                .wrapping_add(s0)
                .wrapping_add(schedule[index - 7])
                .wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.state;
        for index in 0..64 {
            let sigma1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choice = (e & f) ^ ((!e) & g);
            let temp1 = h
                .wrapping_add(sigma1)
                .wrapping_add(choice)
                .wrapping_add(ROUND_CONSTANTS[index])
                .wrapping_add(schedule[index]);
            let sigma0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = sigma0.wrapping_add(majority);

            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }

        self.state[0] = self.state[0].wrapping_add(a);
        self.state[1] = self.state[1].wrapping_add(b);
        self.state[2] = self.state[2].wrapping_add(c);
        self.state[3] = self.state[3].wrapping_add(d);
        self.state[4] = self.state[4].wrapping_add(e);
        self.state[5] = self.state[5].wrapping_add(f);
        self.state[6] = self.state[6].wrapping_add(g);
        self.state[7] = self.state[7].wrapping_add(h);
    }

    fn finalize(mut self) -> [u8; DIGEST_BYTES] {
        let bit_len = self.total_len.wrapping_mul(8);
        self.buffer[self.buffer_len] = 0x80;
        self.buffer_len += 1;

        if self.buffer_len > 56 {
            self.buffer[self.buffer_len..].fill(0);
            let block = self.buffer;
            self.transform(&block);
            self.buffer = [0; BLOCK_BYTES];
            self.buffer_len = 0;
        }

        self.buffer[self.buffer_len..56].fill(0);
        self.buffer[56..64].copy_from_slice(&bit_len.to_be_bytes());
        let block = self.buffer;
        self.transform(&block);

        let mut digest = [0u8; DIGEST_BYTES];
        for (chunk, value) in digest.chunks_exact_mut(4).zip(self.state) {
            chunk.copy_from_slice(&value.to_be_bytes());
        }
        digest
    }
}

/// Returns SHA-256(data), matching the C++ OpenSSL implementation.
#[must_use]
pub fn sha256(data: &[u8]) -> [u8; DIGEST_BYTES] {
    let mut state = Sha256::default();
    state.update(data);
    state.finalize()
}

/// Derives the 32-byte protocol key exactly as the C++ implementation does.
#[must_use]
pub fn derive_key(secret: &str) -> [u8; DIGEST_BYTES] {
    sha256(secret.as_bytes())
}

/// Computes RFC 2104 HMAC-SHA256.
#[must_use]
pub fn hmac_sha256(key: &[u8], message: &[u8]) -> [u8; DIGEST_BYTES] {
    let mut normalized = [0u8; BLOCK_BYTES];
    if key.len() > BLOCK_BYTES {
        normalized[..DIGEST_BYTES].copy_from_slice(&sha256(key));
    } else {
        normalized[..key.len()].copy_from_slice(key);
    }

    let mut inner_pad = [0x36u8; BLOCK_BYTES];
    let mut outer_pad = [0x5cu8; BLOCK_BYTES];
    for ((inner, outer), key_byte) in inner_pad
        .iter_mut()
        .zip(outer_pad.iter_mut())
        .zip(normalized)
    {
        *inner ^= key_byte;
        *outer ^= key_byte;
    }

    let mut inner = Sha256::default();
    inner.update(&inner_pad);
    inner.update(message);
    let inner_digest = inner.finalize();

    let mut outer = Sha256::default();
    outer.update(&outer_pad);
    outer.update(&inner_digest);
    outer.finalize()
}

/// Verifies the 16-byte truncated or full 32-byte tag accepted by the C++ code.
#[must_use]
pub fn hmac_verify(key: &[u8], message: &[u8], tag: &[u8]) -> bool {
    if tag.len() != 16 && tag.len() != DIGEST_BYTES {
        return false;
    }
    let computed = hmac_sha256(key, message);
    computed[..tag.len()]
        .iter()
        .zip(tag)
        .fold(0u8, |difference, (left, right)| difference | (left ^ right))
        == 0
}

#[cfg(test)]
mod tests {
    use super::{derive_key, hmac_sha256, hmac_verify, sha256};

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    #[test]
    fn sha256_known_vector() {
        assert_eq!(
            hex(&sha256(b"abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn hmac_known_vector_and_truncation() {
        let key = [0x0bu8; 20];
        let tag = hmac_sha256(&key, b"Hi There");
        assert_eq!(
            hex(&tag),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
        assert!(hmac_verify(&key, b"Hi There", &tag));
        assert!(hmac_verify(&key, b"Hi There", &tag[..16]));
        assert!(!hmac_verify(&key, b"Hi There!", &tag[..16]));
        assert!(!hmac_verify(&key, b"Hi There", &tag[..15]));
    }

    #[test]
    fn derive_key_is_sha256_of_utf8_secret() {
        assert_eq!(derive_key("NS PC Control"), sha256(b"NS PC Control"));
    }
}
