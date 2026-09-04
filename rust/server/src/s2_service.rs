use crate::app_state::{ServerContext, CLIENT_STALE_NEUTRAL_US, MAX_CLIENTS};
use crate::controller_profiles::{
    report_requests_pair, requested_profile_from_report, switch2_pid_low,
};
use crate::s2_rawgadget::{RawGadgetConfiguration, RawGadgetRuntime};
use crate::s2_reports::{S2ReportBuilder, S2ReportContext};
use crate::s2_rumble::decode_s2_rumble;
use crate::switch2_native::NativeCommandError;
use ns_shared::protocol::{
    ControllerType, HidReport, RumblePacket, EXT_STATUS_MOTION_FRESH,
    EXT_STATUS_MOTION_FRESH_VALID,
};
use std::io;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct S2Source {
    client_index: usize,
    subpad: usize,
    profile: ControllerType,
    report: HidReport,
    generation: u64,
    fresh_motion: bool,
}

impl S2Source {
    #[must_use]
    pub const fn client_index(self) -> usize {
        self.client_index
    }

    #[must_use]
    pub const fn subpad(self) -> usize {
        self.subpad
    }

    #[must_use]
    pub const fn profile(self) -> ControllerType {
        self.profile
    }

    #[must_use]
    pub const fn report(self) -> HidReport {
        self.report
    }

    #[must_use]
    pub const fn generation(self) -> u64 {
        self.generation
    }

    #[must_use]
    pub const fn fresh_motion(self) -> bool {
        self.fresh_motion
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum S2SourceDecision {
    None,
    UnsupportedPair { client_index: usize, subpad: usize },
    Source(S2Source),
}

#[derive(Default)]
pub struct S2SourceTracker {
    owner: Option<(usize, usize)>,
    generation: u64,
    generation_seen_us: u64,
}

impl S2SourceTracker {
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    #[must_use]
    pub fn resolve(&mut self, context: &ServerContext, now_us: u64) -> S2SourceDecision {
        for client_index in 0..MAX_CLIENTS {
            let Some(snapshot) = context.snapshot(client_index, now_us) else {
                continue;
            };
            if !snapshot.active() {
                continue;
            }

            // The C++ admission path makes native Switch 2 mode one-client,
            // one-source and forcibly clears P2/P3/P4. Preserve that contract
            // here even if a malformed/legacy source reaches the writer.
            let subpad = 0;
            let Some(source_report) = snapshot.report().pad(subpad).copied() else {
                continue;
            };
            if report_requests_pair(&source_report) {
                return S2SourceDecision::UnsupportedPair {
                    client_index,
                    subpad,
                };
            }
            let profile = requested_profile_from_report(
                &source_report,
                crate::app_state::UsbControllerFamily::Switch2,
            );
            let owner = (client_index, subpad);
            let owner_changed = self.owner != Some(owner);
            let generation_changed = owner_changed || self.generation != snapshot.generation();
            if owner_changed {
                self.owner = Some(owner);
                self.generation = snapshot.generation();
                self.generation_seen_us = now_us;
            } else if generation_changed {
                self.generation = snapshot.generation();
                self.generation_seen_us = now_us;
            }

            let controls_present = snapshot.pad_present()[subpad];
            let stale = !controls_present
                || now_us.saturating_sub(self.generation_seen_us) > CLIENT_STALE_NEUTRAL_US;
            let report = if stale {
                source_report.neutral_preserving_requested_profile()
            } else {
                source_report
            };
            let status = source_report.status_bytes();
            let motion_fresh_flag = status[1] & EXT_STATUS_MOTION_FRESH_VALID == 0
                || status[1] & EXT_STATUS_MOTION_FRESH != 0;
            let fresh_motion = !stale
                && source_report.has_motion()
                && generation_changed
                && motion_fresh_flag;

            return S2SourceDecision::Source(S2Source {
                client_index,
                subpad,
                profile,
                report,
                generation: snapshot.generation(),
                fresh_motion,
            });
        }
        self.reset();
        S2SourceDecision::None
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum S2TickOutcome {
    Idle,
    Submitted,
    UnsupportedPair { client_index: usize, subpad: usize },
    ReenumerateForIdentity { profile: ControllerType },
    ReenumerateForProtocol,
}

pub struct S2LiveService {
    runtime: RawGadgetRuntime,
    source: S2SourceTracker,
    builder: S2ReportBuilder,
    enumerated_profile: ControllerType,
    owner: Option<(usize, usize)>,
    rumble_active: bool,
}

impl S2LiveService {
    pub fn setup(configuration: &RawGadgetConfiguration) -> io::Result<Self> {
        Ok(Self {
            runtime: RawGadgetRuntime::setup(configuration)?,
            source: S2SourceTracker::default(),
            builder: S2ReportBuilder::default(),
            enumerated_profile: ControllerType::ProS2,
            owner: None,
            rumble_active: false,
        })
    }

    #[must_use]
    pub const fn enumerated_profile(&self) -> ControllerType {
        self.enumerated_profile
    }

    #[must_use]
    pub fn runtime(&self) -> &RawGadgetRuntime {
        &self.runtime
    }

    pub fn tick(
        &mut self,
        context: &ServerContext,
        now_us: u64,
        report_context: S2ReportContext,
    ) -> S2TickOutcome {
        if self.service_vendor(now_us) {
            return S2TickOutcome::ReenumerateForProtocol;
        }

        let decision = self.source.resolve(context, now_us);
        let source = match decision {
            S2SourceDecision::None => {
                self.owner = None;
                self.rumble_active = false;
                self.drain_output_without_owner();
                return S2TickOutcome::Idle;
            }
            S2SourceDecision::UnsupportedPair {
                client_index,
                subpad,
            } => {
                self.owner = None;
                self.rumble_active = false;
                self.drain_output_without_owner();
                return S2TickOutcome::UnsupportedPair {
                    client_index,
                    subpad,
                };
            }
            S2SourceDecision::Source(source) => source,
        };

        let new_owner = (source.client_index(), source.subpad());
        if self.owner != Some(new_owner) {
            self.owner = Some(new_owner);
            self.rumble_active = false;
            self.builder.reset();
        }
        self.service_rumble(context, source.client_index(), source.subpad());

        if source.profile() != self.enumerated_profile {
            self.runtime.native().set_pid(switch2_pid_low(source.profile()));
            self.enumerated_profile = source.profile();
            self.builder.reset();
            return S2TickOutcome::ReenumerateForIdentity {
                profile: source.profile(),
            };
        }

        let native = self.runtime.native();
        if !self.runtime.io_ready() || !native.streaming_enabled() {
            return S2TickOutcome::Idle;
        }
        let flags = native.runtime_flags();
        let selected_report = native.selected_report();
        let report = self.builder.build(
            &source.report(),
            selected_report,
            flags.imu_enabled(),
            source.fresh_motion(),
            now_us,
            report_context,
        );
        if self.runtime.submit_input_report(&report) {
            S2TickOutcome::Submitted
        } else {
            S2TickOutcome::Idle
        }
    }

    fn service_vendor(&self, now_us: u64) -> bool {
        let native = self.runtime.native();
        let mut reenumerate = false;
        while let Some(request) = self.runtime.poll_vendor_report() {
            match native.handle_vendor_command(&request, now_us / 1_000) {
                Ok(response) => {
                    let _ = self.runtime.submit_vendor_report(&response, &request);
                }
                Err(
                    NativeCommandError::TruncatedStreamingCommand
                    | NativeCommandError::UnsupportedReportId(_),
                ) => {
                    reenumerate = true;
                }
                Err(
                    NativeCommandError::ShortOptionalPacket
                    | NativeCommandError::FirmwareUpdateRejected(_),
                ) => {}
            }
        }
        reenumerate
    }

    fn service_rumble(&mut self, context: &ServerContext, client_index: usize, subpad: usize) {
        while let Some(packet) = self.runtime.poll_output_report() {
            let Some(decoded) = decode_s2_rumble(&packet) else {
                continue;
            };
            let neutral = decoded.left() == 0 && decoded.right() == 0;
            if neutral && !self.rumble_active {
                continue;
            }
            let rumble = RumblePacket::new(
                u8::try_from(subpad).unwrap_or(0),
                if neutral { 0 } else { decoded.left() },
                if neutral { 0 } else { decoded.right() },
                if neutral { 0 } else { 1 },
            );
            let _ = context.publish_rumble(client_index, subpad, rumble);
            self.rumble_active = !neutral;
        }
    }

    fn drain_output_without_owner(&self) {
        while self.runtime.poll_output_report().is_some() {}
    }

    pub fn teardown(&self) {
        self.runtime.teardown();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::protocol::{HoriHidReport, MotionReport, MultiReport, EXT_PAD_PRESENT};
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    fn context_with_profile(profile: ControllerType, now_us: u64) -> (ServerContext, usize) {
        let context = ServerContext::default();
        context
            .set_family(crate::app_state::UsbControllerFamily::Switch2, now_us)
            .expect("family");
        let client = context
            .register_udp(
                SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 40_000),
                now_us,
            )
            .expect("client");
        let mut input = HoriHidReport::default();
        input.set_vendor(EXT_PAD_PRESENT);
        let report = HidReport::new(
            input,
            [MotionReport::default(); 3],
            false,
            [0, 0, profile as u8],
        );
        context
            .update_udp_report(client, 1, MultiReport::new([report, HidReport::default(), HidReport::default(), HidReport::default()]), now_us)
            .expect("report");
        (context, client)
    }

    #[test]
    fn source_selection_is_p1_only_and_coerces_to_s2() {
        let now = 1_000_000;
        let (context, client) = context_with_profile(ControllerType::JoyconL, now);
        let mut tracker = S2SourceTracker::default();
        let S2SourceDecision::Source(source) = tracker.resolve(&context, now) else {
            panic!("source");
        };
        assert_eq!(source.client_index(), client);
        assert_eq!(source.subpad(), 0);
        assert_eq!(source.profile(), ControllerType::JoyconLS2);
        assert_eq!(source.report().requested_profile_raw(), ControllerType::JoyconL as u8);
    }

    #[test]
    fn stale_controls_neutralize_but_keep_raw_requested_identity() {
        let now = 2_000_000;
        let (context, _) = context_with_profile(ControllerType::JoyconR, now);
        let mut tracker = S2SourceTracker::default();
        let _ = tracker.resolve(&context, now);
        let S2SourceDecision::Source(source) = tracker.resolve(
            &context,
            now + CLIENT_STALE_NEUTRAL_US + 1,
        ) else {
            panic!("source");
        };
        assert_eq!(source.profile(), ControllerType::JoyconRS2);
        assert_eq!(source.report().input().buttons(), 0);
        assert_eq!(source.report().input().axes(), [128; 4]);
        assert_eq!(source.report().requested_profile_raw(), ControllerType::JoyconR as u8);
    }

    #[test]
    fn switch2_pair_request_is_rejected_before_mapping() {
        let now = 3_000_000;
        let (context, client) = context_with_profile(ControllerType::JoyconPair, now);
        let mut tracker = S2SourceTracker::default();
        assert_eq!(
            tracker.resolve(&context, now),
            S2SourceDecision::UnsupportedPair {
                client_index: client,
                subpad: 0,
            }
        );
    }
}
