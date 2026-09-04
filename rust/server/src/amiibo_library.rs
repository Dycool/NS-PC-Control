//! Safe Rust port of the persistent Amiibo library and amiitool-compatible formatter.
//!
//! The formatter uses first-party safe Rust AES-128-CTR and the shared safe HMAC-SHA256
//! implementation. Retail keys and Nintendo tag dumps are never bundled in the source.

use crate::s2_nfc_codec::{
    crc16_mcrf4xx, set_v3_read_prefix_resolver, Signature, V3_SRAM_OFFSET,
};
use ns_shared::control_packets::AmiiboLibraryResult;
use ns_shared::crypto::hmac_sha256;
use ns_shared::protocol::is_supported_amiibo_dump_size;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

include!("amiibo_library/crypto.rs");
include!("amiibo_library/storage.rs");
include!("amiibo_library/tests.rs");
