use crate::app_state::{ServerContext, CLIENT_STALE_NEUTRAL_US, MAX_CLIENTS};
use crate::virtual_controller::VirtualController;
use ns_shared::protocol::HoriHidReport;
use std::io;
use std::time::{Duration, Instant};

#[must_use]
pub fn build_legacy_reports(context: &ServerContext, now_us: u64) -> [HoriHidReport; 4] {
    let mut output = [HoriHidReport::default(); 4];
    let mut next_port = 0_usize;
    for client_index in 0..MAX_CLIENTS {
        let Some(snapshot) = context.snapshot(client_index, now_us) else {
            continue;
        };
        if !snapshot.active() {
            continue;
        }
        for (pad_index, report) in snapshot.report().pads().iter().enumerate() {
            if next_port >= output.len() {
                break;
            }
            if !snapshot.pad_present()[pad_index] {
                continue;
            }

            // Match the C++ writer: stale controls go neutral, but the source
            // session remains intact. Requested controller identity is
            // configuration and must not collapse back to Pro after 350 ms.
            let last_present_us = snapshot.pad_last_present_us()[pad_index];
            let stale = last_present_us != 0
                && now_us.saturating_sub(last_present_us) > CLIENT_STALE_NEUTRAL_US;
            output[next_port] = if stale {
                HoriHidReport::default()
            } else {
                *report.input()
            };
            next_port += 1;
        }
    }
    output
}

pub fn write_once(
    context: &ServerContext,
    controllers: &mut [Box<dyn VirtualController>],
    now_us: u64,
) -> io::Result<usize> {
    let output = build_legacy_reports(context, now_us);
    let mut writes = 0_usize;
    for (controller, report) in controllers.iter_mut().zip(output) {
        controller.write_report(report)?;
        context.record_hid_write();
        writes += 1;
    }
    Ok(writes)
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
    use ns_shared::protocol::{
        ControllerType, Hat, HidReport, HoriHidReport, MotionReport, MultiReport, BTN_A,
        EXT_PAD_PRESENT,
    };
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    #[test]
    fn stale_controls_neutralize_without_erasing_requested_profile() {
        let context = ServerContext::default();
        let received_us = 1_000_000;
        let client = context
            .register_udp(
                SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 40_001),
                received_us,
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
            [0, 0, ControllerType::JoyconL as u8],
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
                received_us,
            )
            .expect("report");

        let output = build_legacy_reports(&context, received_us + CLIENT_STALE_NEUTRAL_US + 1);
        assert_eq!(output[0], HoriHidReport::default());
        let snapshot = context
            .snapshot(client, received_us + CLIENT_STALE_NEUTRAL_US + 1)
            .expect("snapshot");
        assert_eq!(
            snapshot.report().pads()[0].requested_profile_raw(),
            ControllerType::JoyconL as u8
        );
    }
}
