//! Safe Rust port of the Switch 2 NFC/Amiibo codec and host-replayable state machine.
//!
//! All byte manipulation uses checked slices. No packed structs, raw pointers, FFI, or
//! layout casts are used.

use std::fmt;
use std::sync::{Mutex, OnceLock};

include!("s2_nfc_codec/core_a.rs");
include!("s2_nfc_codec/core_b.rs");
include!("s2_nfc_codec/core_c.rs");
include!("s2_nfc_codec/core_d.rs");
include!("s2_nfc_codec/runtime_a.rs");
include!("s2_nfc_codec/runtime_b.rs");
include!("s2_nfc_codec/facade.rs");
include!("s2_nfc_codec/tests.rs");
