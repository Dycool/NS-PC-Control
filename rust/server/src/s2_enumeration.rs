use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU8, Ordering};
use std::sync::Mutex;

pub const SWITCH2_MAX_ENUMERATION_RETRIES: u32 = 3;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(u8)]
pub enum EnumerationState {
    #[default]
    WaitingForHost = 0,
    UsbConfigured = 1,
    NativeHandshake = 2,
    Streaming = 3,
    Recovering = 4,
}

pub struct EnumerationTracker {
    state: AtomicU8,
    failures: AtomicU32,
    recovery_requested: AtomicBool,
    recovery_running: AtomicBool,
    automatic_recovery_enabled: AtomicBool,
    recovery_reason: Mutex<String>,
}

impl Default for EnumerationTracker {
    fn default() -> Self {
        Self {
            state: AtomicU8::new(EnumerationState::WaitingForHost as u8),
            failures: AtomicU32::new(0),
            recovery_requested: AtomicBool::new(false),
            recovery_running: AtomicBool::new(false),
            automatic_recovery_enabled: AtomicBool::new(true),
            recovery_reason: Mutex::new(String::new()),
        }
    }
}

impl EnumerationTracker {
    pub fn state(&self) -> EnumerationState {
        match self.state.load(Ordering::Acquire) {
            1 => EnumerationState::UsbConfigured,
            2 => EnumerationState::NativeHandshake,
            3 => EnumerationState::Streaming,
            4 => EnumerationState::Recovering,
            _ => EnumerationState::WaitingForHost,
        }
    }
    pub fn failure_count(&self) -> u32 { self.failures.load(Ordering::Acquire) }
    pub fn gadget_started(&self) { self.set_state(EnumerationState::WaitingForHost); }
    pub fn usb_configured(&self) { if !self.recovery_active() { self.set_state(EnumerationState::UsbConfigured); } }
    pub fn native_handshake(&self) { if !self.recovery_active() && self.state() == EnumerationState::UsbConfigured { self.set_state(EnumerationState::NativeHandshake); } }
    pub fn streaming_validated(&self, _report_id: u8) {
        if self.recovery_active() { return; }
        self.failures.store(0, Ordering::Release);
        self.automatic_recovery_enabled.store(true, Ordering::Release);
        self.set_state(EnumerationState::Streaming);
    }
    pub fn bus_reset(&self) { if !self.recovery_active() { self.set_state(EnumerationState::WaitingForHost); } }
    pub fn client_connected(&self) { self.failures.store(0, Ordering::Release); self.automatic_recovery_enabled.store(true, Ordering::Release); }
    pub fn client_disconnected(&self) { self.failures.store(0, Ordering::Release); }
    pub fn request_reenumeration(&self, reason: &str) -> RecoveryDecision {
        if !self.automatic_recovery_enabled.load(Ordering::Acquire) { return RecoveryDecision::Suppressed; }
        if self.recovery_running.load(Ordering::Acquire)
            || self.recovery_requested.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire).is_err()
        { return RecoveryDecision::Coalesced; }
        let failures = self.failures.fetch_add(1, Ordering::AcqRel) + 1;
        *self.recovery_reason.lock().unwrap_or_else(|poison| poison.into_inner()) = reason.to_string();
        if failures >= SWITCH2_MAX_ENUMERATION_RETRIES {
            self.recovery_requested.store(false, Ordering::Release);
            self.automatic_recovery_enabled.store(false, Ordering::Release);
            self.failures.store(0, Ordering::Release);
            self.set_state(EnumerationState::WaitingForHost);
            RecoveryDecision::RetryLimitReached
        } else {
            self.set_state(EnumerationState::Recovering);
            RecoveryDecision::Requested
        }
    }
    pub fn begin_service(&self) -> Option<String> {
        if !self.recovery_requested.load(Ordering::Acquire) || self.recovery_running.swap(true, Ordering::AcqRel) { return None; }
        if !self.recovery_requested.swap(false, Ordering::AcqRel) { self.recovery_running.store(false, Ordering::Release); return None; }
        Some(self.recovery_reason.lock().unwrap_or_else(|poison| poison.into_inner()).clone())
    }
    pub fn finish_service(&self, started: bool) {
        self.recovery_running.store(false, Ordering::Release);
        if started { self.gadget_started(); } else { self.set_state(EnumerationState::WaitingForHost); }
    }
    fn recovery_active(&self) -> bool { self.recovery_requested.load(Ordering::Acquire) || self.recovery_running.load(Ordering::Acquire) }
    fn set_state(&self, state: EnumerationState) { self.state.store(state as u8, Ordering::Release); }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RecoveryDecision { Requested, Coalesced, Suppressed, RetryLimitReached }

#[cfg(test)]
mod tests {
    use super::{EnumerationState, EnumerationTracker, RecoveryDecision};
    #[test]
    fn tracks_handshake_and_retry_limit() {
        let tracker = EnumerationTracker::default();
        tracker.usb_configured();
        tracker.native_handshake();
        assert_eq!(tracker.state(), EnumerationState::NativeHandshake);
        tracker.streaming_validated(5);
        assert_eq!(tracker.state(), EnumerationState::Streaming);
        assert_eq!(tracker.request_reenumeration("one"), RecoveryDecision::Requested);
        assert_eq!(tracker.request_reenumeration("dup"), RecoveryDecision::Coalesced);
        let reason = tracker.begin_service().expect("pending recovery");
        assert_eq!(reason, "one");
        tracker.finish_service(false);
        assert_eq!(tracker.request_reenumeration("two"), RecoveryDecision::Requested);
        let _ = tracker.begin_service();
        tracker.finish_service(false);
        assert_eq!(tracker.request_reenumeration("three"), RecoveryDecision::RetryLimitReached);
    }
}
