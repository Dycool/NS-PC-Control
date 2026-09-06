#![forbid(unsafe_code)]

//! Shared protocol, cryptography, and deterministic pure logic for NS PC Control.

mod compat;
mod protocol_ext;
pub mod aes;
pub mod control_packets;
pub mod crypto;
pub mod joycon_mouse;
pub mod macros;
pub mod protocol;
pub mod sdl_input;
