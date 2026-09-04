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

impl RawGadgetRuntime {
    /// Start the native transport with the logical Switch 2 controller identity
    /// already installed before the EP0/event worker can observe the host.
    ///
    /// This mirrors the C++ re-enumeration contract: factory memory 0x13014 and
    /// the EP0 identity must agree from the first request after reconnect.
    pub fn setup_with_pid(
        configuration: &RawGadgetConfiguration,
        pid_low: u8,
    ) -> io::Result<Self> {
        ensure_raw_gadget_device(configuration.device_path())?;
        unbind_legacy_gadget(&configuration.legacy_gadget_dir)?;
        let udc = first_udc_name(configuration.udc_root())?;
        install_interrupt_handler()?;
        let gadget = Arc::new(RawGadget::open(
            configuration.device_path(),
            &udc,
            &udc,
            UsbSpeed::Full,
        )?);
        let native = Arc::new(NativeController::default());
        native.reset();
        native.set_pid(pid_low);
        native.enumeration().gadget_started();
        let inner = Arc::new(Inner {
            gadget,
            native,
            running: AtomicBool::new(true),
            generation: AtomicU64::new(1),
            state: Mutex::new(RuntimeState {
                state: GadgetState::DeviceInitialized,
                ..RuntimeState::default()
            }),
            queues: Mutex::new(Queues::default()),
            input_cv: Condvar::new(),
            vendor_cv: Condvar::new(),
            audio_cv: Condvar::new(),
            worker_tokens: Mutex::new([None; WORKER_COUNT]),
            motion: Mutex::new(MotionTiming::default()),
            serial: configuration.serial.clone(),
            origin: Instant::now(),
        });
        let workers = spawn_workers(&inner);
        Ok(Self {
            inner,
            workers: Mutex::new(workers),
        })
    }

    /// Interrupt one blocking worker by index. This is used by recovery paths
    /// that need to drain a specific endpoint without tearing down the entire
    /// native transport.
    pub fn interrupt_endpoint_worker(&self, worker_index: usize) {
        if worker_index < WORKER_COUNT {
            interrupt_worker(&self.inner, worker_index);
        }
    }
}
