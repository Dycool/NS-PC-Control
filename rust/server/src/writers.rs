use crate::app_state::{ServerContext, MAX_CLIENTS};
use crate::virtual_controller::VirtualController;
use ns_shared::protocol::HoriHidReport;
use std::io;
use std::time::{Duration, Instant};

pub fn write_once(
    context: &ServerContext,
    controllers: &mut [Box<dyn VirtualController>],
    now_us: u64,
) -> io::Result<usize> {
    let mut output = [HoriHidReport::default(); 4];
    let mut next_port = 0_usize;
    for client_index in 0..MAX_CLIENTS {
        let Some(snapshot) = context.snapshot(client_index, now_us) else { continue; };
        if !snapshot.active() { continue; }
        for (pad_index, report) in snapshot.report().pads().iter().enumerate() {
            if next_port >= output.len() { break; }
            if snapshot.pad_present()[pad_index] {
                output[next_port] = *report.input();
                next_port += 1;
            }
        }
    }
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
        if deadline > now { std::thread::sleep(deadline - now); } else { deadline = now; }
    }
    Ok(())
}
