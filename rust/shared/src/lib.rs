#![forbid(unsafe_code)]

//! Shared protocol, cryptography, and deterministic pure logic for NS PC Control.

mod compat;
pub mod control_packets;
pub mod crypto;
pub mod macros;
pub mod protocol;
