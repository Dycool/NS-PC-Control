//! Safe application-side Switch 2 Raw Gadget runtime.
//!
//! `ns-backend` remains `#![forbid(unsafe_code)]`. All Linux Raw Gadget
//! ioctls, raw pointers, C ABI calls, and thread-interrupt primitives live in
//! the dependency-free `ns-raw-gadget` crate, which is the repository's sole
//! approved unsafe Rust boundary.

use crate::s2_uac1_audio::AudioControl;
use crate::s2_usb_descriptors::{
    config_descriptor, device_descriptor, hid_descriptor, string_descriptor,
    AUDIO_CAPTURE_ADDRESS, AUDIO_CAPTURE_INTERFACE, AUDIO_PLAYBACK_ADDRESS,
    HID_IN_ADDRESS, HID_OUT_ADDRESS, VENDOR_IN_ADDRESS, VENDOR_OUT_ADDRESS,
};
use crate::switch2_native::{Ep0Reply, NativeController};
use crate::virtual_controller::S2_PRO_REPORT_DESC as S2_PRO_REPORT_DESC_BYTES;
use ns_raw_gadget::{
    current_thread_interrupt_token, install_interrupt_handler, interrupt_thread,
    ControlRequest, EndpointDescriptor, EndpointHandle, EventKind, RawGadget,
    ThreadInterruptToken, UsbSpeed,
};
use ns_shared::protocol::S2_AUDIO_USB_FRAME_BYTES;
use std::collections::VecDeque;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const S2_PRO_REPORT_DESC: &[u8] = &S2_PRO_REPORT_DESC_BYTES;

include!("s2_rawgadget/runtime.rs");
include!("s2_rawgadget/transport.rs");
