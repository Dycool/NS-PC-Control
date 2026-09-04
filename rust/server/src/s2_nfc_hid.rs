#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum NfcHidEventReason {
    #[default]
    None,
    TagPresented,
    TagRemoved,
    ScanReady,
    OperationReady,
    WriteComplete,
    Error,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct PendingEvent {
    due_ms: u64,
    reason: NfcHidEventReason,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct NfcHidState {
    state: u8,
    pending: Option<PendingEvent>,
}

impl NfcHidState {
    #[must_use]
    pub const fn current(self) -> u8 {
        self.state
    }

    pub fn signal(&mut self, reason: NfcHidEventReason) {
        self.advance(reason);
    }

    pub fn schedule(&mut self, now_ms: u64, delay_ms: u64, reason: NfcHidEventReason) {
        self.pending = Some(PendingEvent {
            due_ms: now_ms.saturating_add(delay_ms),
            reason,
        });
    }

    pub fn cancel(&mut self) {
        self.pending = None;
    }

    #[must_use]
    pub fn report_state(&mut self, now_ms: u64) -> u8 {
        if let Some(event) = self.pending.filter(|event| now_ms >= event.due_ms) {
            self.advance(event.reason);
        }
        self.state
    }

    pub fn tag_presented(&mut self) {
        self.signal(NfcHidEventReason::TagPresented);
    }

    pub fn tag_removed(&mut self) {
        self.cancel();
        self.signal(NfcHidEventReason::TagRemoved);
    }

    pub fn note_command(&mut self, now_ms: u64, subcommand: u8, tag_is_placed: bool) {
        match subcommand {
            0x03 if tag_is_placed => {
                self.schedule(now_ms, 40, NfcHidEventReason::ScanReady);
            }
            0x06 | 0x1e | 0x21 => {
                self.schedule(now_ms, 40, NfcHidEventReason::OperationReady);
            }
            0x08 | 0x20 => {
                self.schedule(now_ms, 700, NfcHidEventReason::WriteComplete);
            }
            _ => {}
        }
    }

    fn advance(&mut self, _reason: NfcHidEventReason) {
        self.state = self.state.wrapping_add(1) & 0x07;
        self.pending = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_advances_modulo_eight_like_cpp() {
        let mut state = NfcHidState::default();
        for expected in 1..=7 {
            state.signal(NfcHidEventReason::TagPresented);
            assert_eq!(state.current(), expected);
        }
        state.signal(NfcHidEventReason::TagRemoved);
        assert_eq!(state.current(), 0);
    }

    #[test]
    fn scan_and_operation_transitions_use_40ms_delay() {
        let mut state = NfcHidState::default();
        state.note_command(1_000, 0x03, true);
        assert_eq!(state.report_state(1_039), 0);
        assert_eq!(state.report_state(1_040), 1);
        state.note_command(2_000, 0x06, true);
        assert_eq!(state.report_state(2_039), 1);
        assert_eq!(state.report_state(2_040), 2);
    }

    #[test]
    fn write_complete_transition_uses_700ms_delay() {
        let mut state = NfcHidState::default();
        state.note_command(10, 0x20, true);
        assert_eq!(state.report_state(709), 0);
        assert_eq!(state.report_state(710), 1);
    }

    #[test]
    fn scan_without_a_tag_does_not_advance() {
        let mut state = NfcHidState::default();
        state.note_command(100, 0x03, false);
        assert_eq!(state.report_state(10_000), 0);
    }

    #[test]
    fn tag_removal_cancels_delayed_event_before_advancing() {
        let mut state = NfcHidState::default();
        state.note_command(0, 0x20, true);
        state.tag_removed();
        assert_eq!(state.current(), 1);
        assert_eq!(state.report_state(1_000), 1);
    }
}
