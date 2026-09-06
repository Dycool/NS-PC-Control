use crate::app_state::{ServerContext, UsbControllerFamily, CLIENT_STALE_NEUTRAL_US, MAX_CLIENTS};
use crate::legacy_identity::observe_legacy_identity;
use crate::legacy_layout::{LegacyLayout, LegacySlot, LEGACY_PORTS};
use crate::s1_protocol::S1PortRuntime;
use crate::s1_reports::{build_standard_report, pro_timer_from_us};
use crate::virtual_controller::{VirtualController, VIRTUAL_BODY_RGB};
use ns_shared::protocol::{ControllerType, HidReport, HoriHidReport, MotionReport};
use std::io;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

const PRO_REPORT_INTERVAL_US: u64 = 4_000;
const PRO_IDLE_REPORT_INTERVAL_US: u64 = 1_000_000 / 30;
const PRO_RELEASE_NEUTRAL_US: u64 = 250_000;

struct LegacyWriterState {
    family: Option<UsbControllerFamily>,
    layout: LegacyLayout,
    s1: [S1PortRuntime; LEGACY_PORTS],
    last_generation: [u64; MAX_CLIENTS],
    generation_seen_us: [u64; MAX_CLIENTS],
    last_standard_report_us: [u64; LEGACY_PORTS],
    last_idle_neutral_us: [u64; LEGACY_PORTS],
    neutral_burst_until_us: [u64; LEGACY_PORTS],
    previous_hori: [Option<HoriHidReport>; LEGACY_PORTS],
}

impl Default for LegacyWriterState {
    fn default() -> Self {
        Self {
            family: None,
            layout: LegacyLayout::default(),
            s1: std::array::from_fn(|port| S1PortRuntime::new(port, ControllerType::Pro)),
            last_generation: [0; MAX_CLIENTS],
            generation_seen_us: [0; MAX_CLIENTS],
            last_standard_report_us: [0; LEGACY_PORTS],
            last_idle_neutral_us: [0; LEGACY_PORTS],
            neutral_burst_until_us: [0; LEGACY_PORTS],
            previous_hori: [None; LEGACY_PORTS],
        }
    }
}

impl LegacyWriterState {
    fn reset_for_family(&mut self, family: UsbControllerFamily) {
        self.family = Some(family);
        self.layout.reset();
        self.last_generation = [0; MAX_CLIENTS];
        self.generation_seen_us = [0; MAX_CLIENTS];
        self.last_standard_report_us = [0; LEGACY_PORTS];
        self.last_idle_neutral_us = [0; LEGACY_PORTS];
        self.neutral_burst_until_us = [0; LEGACY_PORTS];
        self.previous_hori = [None; LEGACY_PORTS];
        for (port, runtime) in self.s1.iter_mut().enumerate() {
            *runtime = S1PortRuntime::new(port, ControllerType::Pro);
        }
    }

    fn reset_transport(&mut self) {
        for runtime in &mut self.s1 {
            runtime.reset_transport();
        }
        self.last_standard_report_us = [0; LEGACY_PORTS];
        self.last_idle_neutral_us = [0; LEGACY_PORTS];
        self.neutral_burst_until_us = [0; LEGACY_PORTS];
        self.previous_hori = [None; LEGACY_PORTS];
    }

    fn note_generations(
        &mut self,
        snapshots: &[Option<crate::app_state::ClientSnapshot>; MAX_CLIENTS],
        now_us: u64,
    ) {
        for (client, snapshot) in snapshots.iter().enumerate() {
            let Some(snapshot) = snapshot.filter(|snapshot| snapshot.active()) else {
                self.last_generation[client] = 0;
                self.generation_seen_us[client] = 0;
                continue;
            };
            if self.generation_seen_us[client] == 0
                || self.last_generation[client] != snapshot.generation()
            {
                self.last_generation[client] = snapshot.generation();
                self.generation_seen_us[client] = now_us;
            }
        }
    }

    fn client_stale(&self, client: usize, now_us: u64) -> bool {
        let seen = self.generation_seen_us[client];
        seen != 0 && now_us.saturating_sub(seen) > CLIENT_STALE_NEUTRAL_US
    }
}

fn writer_state() -> &'static Mutex<LegacyWriterState> {
    static STATE: OnceLock<Mutex<LegacyWriterState>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(LegacyWriterState::default()))
}

pub fn reset_legacy_writer_transport() -> io::Result<()> {
    writer_state()
        .lock()
        .map_err(|_| io::Error::other("legacy writer state poisoned"))?
        .reset_transport();
    Ok(())
}

#[must_use]
pub fn build_legacy_reports(context: &ServerContext, now_us: u64) -> [HoriHidReport; 4] {
    let snapshots = std::array::from_fn(|client| context.snapshot(client, now_us));
    let mut layout = LegacyLayout::default();
    let slots = layout.reconcile(&snapshots, context.family());
    std::array::from_fn(|port| {
        let slot = slots[port];
        let Some(client) = slot.client_index() else {
            return HoriHidReport::default();
        };
        let Some(snapshot) = snapshots[client] else {
            return HoriHidReport::default();
        };
        if !source_active(&snapshot, slot.subpad()) {
            return HoriHidReport::default();
        }
        let last_present = snapshot.pad_last_present_us()[slot.subpad()];
        if last_present != 0 && now_us.saturating_sub(last_present) > CLIENT_STALE_NEUTRAL_US {
            HoriHidReport::default()
        } else {
            *snapshot.report().pads()[slot.subpad()].input()
        }
    })
}

pub fn write_once(
    context: &ServerContext,
    controllers: &mut [Box<dyn VirtualController>],
    now_us: u64,
) -> io::Result<usize> {
    let family = context.family();
    if family == UsbControllerFamily::Switch2 {
        return Ok(0);
    }

    let snapshots = std::array::from_fn(|client| context.snapshot(client, now_us));
    let mut state = writer_state()
        .lock()
        .map_err(|_| io::Error::other("legacy writer state poisoned"))?;
    if state.family != Some(family) {
        state.reset_for_family(family);
    }
    state.note_generations(&snapshots, now_us);

    let previous_slots = *state.layout.slots();
    let slots = state.layout.reconcile(&snapshots, family);
    observe_legacy_identity(family, &slots, now_us);
    publish_assignments(context, &state.layout, &snapshots, family)?;
    handle_mapping_changes(
        context,
        controllers,
        &mut state,
        previous_slots,
        slots,
        family,
        now_us,
    )?;

    let mut writes = 0_usize;
    for port in 0..controllers.len().min(LEGACY_PORTS) {
        let slot = slots[port];
        let active = slot
            .client_index()
            .and_then(|client| snapshots[client].map(|snapshot| source_active(&snapshot, slot.subpad())))
            .unwrap_or(false);
        let source = source_report(&state, &snapshots, slot, active, now_us);

        if family == UsbControllerFamily::Hori {
            let mut report = if active {
                *source.input()
            } else {
                HoriHidReport::default()
            };
            report.set_vendor(0);
            if state.previous_hori[port] != Some(report) {
                controllers[port].write_report(report)?;
                state.previous_hori[port] = Some(report);
                context.record_hid_write();
                writes += 1;
            }
            continue;
        }

        state.s1[port].set_profile(slot_profile(slot));
        if let Some(reply) = state.s1[port].take_subcommand_reply(
            &source,
            slot.virtual_type(),
            slot.pair_member(),
            now_us,
        ) {
            controllers[port].write_bytes(&reply)?;
            context.record_hid_write();
            writes += 1;
        } else if state.s1[port].full_report_enabled() {
            let release_burst = state.neutral_burst_until_us[port] != 0
                && now_us < state.neutral_burst_until_us[port];
            if state.neutral_burst_until_us[port] != 0
                && now_us >= state.neutral_burst_until_us[port]
            {
                state.neutral_burst_until_us[port] = 0;
            }
            let due = if active || release_burst {
                state.last_standard_report_us[port] == 0
                    || now_us.saturating_sub(state.last_standard_report_us[port])
                        >= PRO_REPORT_INTERVAL_US
            } else {
                state.last_idle_neutral_us[port] == 0
                    || now_us.saturating_sub(state.last_idle_neutral_us[port])
                        >= PRO_IDLE_REPORT_INTERVAL_US
            };
            if due {
                let prepared = prepare_s1_motion_source(&source, slot);
                let report = build_standard_report(
                    &prepared,
                    slot.virtual_type(),
                    slot.pair_member(),
                    state.s1[port].imu_enabled(),
                    pro_timer_from_us(now_us),
                );
                controllers[port].write_bytes(&report)?;
                if active || release_burst {
                    state.last_standard_report_us[port] = now_us;
                } else {
                    state.last_idle_neutral_us[port] = now_us;
                }
                context.record_hid_write();
                writes += 1;
            }
        }

        process_s1_output(context, &mut *controllers[port], &mut state.s1[port], slot, port)?;
    }
    Ok(writes)
}

fn publish_assignments(
    context: &ServerContext,
    layout: &LegacyLayout,
    snapshots: &[Option<crate::app_state::ClientSnapshot>; MAX_CLIENTS],
    family: UsbControllerFamily,
) -> io::Result<()> {
    let assignments = layout.assignments(snapshots, family);
    for (client, snapshot) in snapshots.iter().enumerate() {
        if !snapshot.is_some_and(|snapshot| snapshot.active()) {
            continue;
        }
        for (subpad, assignment) in assignments[client].iter().copied().enumerate() {
            if context.assignment(client, subpad) != Some(assignment) {
                context
                    .publish_assignment(client, subpad, assignment)
                    .map_err(|_| io::Error::other("cannot publish controller assignment"))?;
            }
        }
    }
    Ok(())
}

fn handle_mapping_changes(
    context: &ServerContext,
    controllers: &mut [Box<dyn VirtualController>],
    state: &mut LegacyWriterState,
    previous: [LegacySlot; LEGACY_PORTS],
    current: [LegacySlot; LEGACY_PORTS],
    family: UsbControllerFamily,
    now_us: u64,
) -> io::Result<()> {
    for port in 0..controllers.len().min(LEGACY_PORTS) {
        if previous[port] == current[port] {
            continue;
        }
        if previous[port].client_index().is_some() {
            drain_output(&mut *controllers[port])?;
            if family == UsbControllerFamily::Hori {
                let mut neutral = HoriHidReport::default();
                neutral.set_vendor(0);
                controllers[port].write_report(neutral)?;
                state.previous_hori[port] = Some(neutral);
                context.record_hid_write();
            } else {
                state.neutral_burst_until_us[port] = now_us.saturating_add(PRO_RELEASE_NEUTRAL_US);
            }
        }
        state.s1[port].set_profile(slot_profile(current[port]));
        if let Some(client) = current[port].client_index() {
            let _ = context.publish_controller_status(
                client,
                current[port].subpad(),
                0,
                Some(VIRTUAL_BODY_RGB[port]),
            );
        }
    }
    Ok(())
}

fn process_s1_output(
    context: &ServerContext,
    controller: &mut dyn VirtualController,
    runtime: &mut S1PortRuntime,
    slot: LegacySlot,
    port: usize,
) -> io::Result<()> {
    let mut buffer = [0_u8; 64];
    for _ in 0..16 {
        let Some(size) = controller.poll_output(&mut buffer)? else {
            break;
        };
        let effects = runtime.process_output(&buffer[..size], slot.subpad());
        if let Some(report) = effects.immediate_report {
            controller.write_bytes(&report)?;
            context.record_hid_write();
        }
        if let Some(rumble) = effects.rumble
            && let Some(client) = slot.client_index()
        {
            let _ = context.publish_rumble(client, slot.subpad(), rumble);
        }
        if let Some(lights) = effects.player_lights
            && let Some(client) = slot.client_index()
        {
            let _ = context.publish_controller_status(
                client,
                slot.subpad(),
                lights,
                Some(VIRTUAL_BODY_RGB[port]),
            );
        }
    }
    Ok(())
}

fn source_report(
    state: &LegacyWriterState,
    snapshots: &[Option<crate::app_state::ClientSnapshot>; MAX_CLIENTS],
    slot: LegacySlot,
    active: bool,
    now_us: u64,
) -> HidReport {
    let Some(client) = slot.client_index() else {
        return HidReport::default();
    };
    let Some(snapshot) = snapshots[client] else {
        return HidReport::default();
    };
    let source = snapshot.report().pads()[slot.subpad()];
    if !active || state.client_stale(client, now_us) {
        source.neutral_preserving_requested_profile()
    } else {
        source
    }
}

fn source_active(snapshot: &crate::app_state::ClientSnapshot, subpad: usize) -> bool {
    let presence_seen = snapshot.pad_present().iter().any(|present| *present)
        || snapshot.pad_last_present_us().iter().any(|stamp| *stamp != 0);
    if presence_seen {
        snapshot.pad_present()[subpad]
    } else {
        *snapshot.report().pads()[subpad].input() != HoriHidReport::default()
    }
}

fn slot_profile(slot: LegacySlot) -> ControllerType {
    match slot.virtual_type() {
        ControllerType::JoyconL | ControllerType::JoyconLS2 => ControllerType::JoyconL,
        ControllerType::JoyconR | ControllerType::JoyconRS2 => ControllerType::JoyconR,
        ControllerType::Hori => ControllerType::Hori,
        _ => ControllerType::Pro,
    }
}

fn prepare_s1_motion_source(source: &HidReport, slot: LegacySlot) -> HidReport {
    if slot.pair_member() && !slot.pair_right() {
        return HidReport::new(*source.input(), [MotionReport::default(); 3], false, source.reserved());
    }
    if !matches!(slot.virtual_type(), ControllerType::JoyconR | ControllerType::JoyconRS2)
        || !source.has_motion()
    {
        return *source;
    }
    let motion = source.motion().map(|sample| {
        let [ax, ay, az] = sample.accel();
        let [gx, gy, gz] = sample.gyro();
        MotionReport::new(
            [ax, ay.saturating_neg(), az.saturating_neg()],
            [gx, gy.saturating_neg(), gz.saturating_neg()],
        )
    });
    HidReport::new(*source.input(), motion, true, source.reserved())
}

fn drain_output(controller: &mut dyn VirtualController) -> io::Result<()> {
    let mut buffer = [0_u8; 64];
    for _ in 0..32 {
        if controller.poll_output(&mut buffer)?.is_none() {
            break;
        }
    }
    Ok(())
}

pub fn run_writer_loop(
    context: &ServerContext,
    controllers: &mut [Box<dyn VirtualController>],
    frequency_hz: u32,
) -> io::Result<()> {
    let frequency = frequency_hz.max(1);
    let period = Duration::from_nanos(1_000_000_000_u64 / u64::from(frequency));
    let origin = Instant::now();
    let mut deadline = Instant::now();
    while context.is_running() {
        let now_us = u64::try_from(origin.elapsed().as_micros()).unwrap_or(u64::MAX);
        write_once(context, controllers, now_us)?;
        deadline += period;
        let now = Instant::now();
        if deadline > now {
            std::thread::sleep(deadline - now);
        } else {
            deadline = now;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::virtual_controller::MemoryController;
    use ns_shared::protocol::{Hat, MultiReport, BTN_A, EXT_PAD_PRESENT};
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    fn context_with_report(
        family: UsbControllerFamily,
        profile: ControllerType,
        now_us: u64,
    ) -> ServerContext {
        let context = ServerContext::default();
        context.set_family(family, now_us).expect("family");
        let client = context
            .register_udp(
                SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 40_001),
                now_us,
            )
            .expect("client");
        let input = HoriHidReport::new(
            BTN_A,
            Hat::Neutral,
            [255, 0, 128, 128],
            EXT_PAD_PRESENT,
        );
        let report = HidReport::new(
            input,
            [MotionReport::default(); 3],
            false,
            [0, 0, profile as u8],
        );
        context
            .update_udp_report(
                client,
                1,
                MultiReport::new([
                    report,
                    HidReport::default(),
                    HidReport::default(),
                    HidReport::default(),
                ]),
                now_us,
            )
            .expect("report");
        context
    }

    #[test]
    fn stale_controls_neutralize_without_erasing_requested_profile() {
        let received_us = 1_000_000;
        let context = context_with_report(
            UsbControllerFamily::Switch1,
            ControllerType::JoyconL,
            received_us,
        );
        let output = build_legacy_reports(&context, received_us + CLIENT_STALE_NEUTRAL_US + 1);
        assert_eq!(output[0], HoriHidReport::default());
        let snapshot = context
            .snapshot(0, received_us + CLIENT_STALE_NEUTRAL_US + 1)
            .expect("snapshot");
        assert_eq!(
            snapshot.report().pads()[0].requested_profile_raw(),
            ControllerType::JoyconL as u8
        );
    }

    #[test]
    fn hori_writer_uses_eight_byte_report_and_assignment() {
        let now = 2_000_000;
        let context = context_with_report(UsbControllerFamily::Hori, ControllerType::Hori, now);
        let mut controllers: Vec<Box<dyn VirtualController>> =
            (0..4).map(|_| Box::new(MemoryController::default()) as Box<dyn VirtualController>).collect();
        let writes = write_once(&context, &mut controllers, now).expect("write");
        assert!(writes >= 1);
        assert_eq!(context.assignment(0, 0).expect("assignment").console_port_mask(), 1);
    }
}
