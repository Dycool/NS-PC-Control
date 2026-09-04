use crate::udp_protocol::UdpClient;
use ns_shared::protocol::MultiReport;
use std::io;
use std::time::{Duration, Instant};

pub struct StreamRuntime { interval: Duration, next_deadline: Instant }

impl StreamRuntime {
    pub fn new(hz: u32) -> Self {
        let hz = hz.max(1);
        Self { interval: Duration::from_nanos(1_000_000_000_u64 / u64::from(hz)), next_deadline: Instant::now() }
    }
    pub fn tick(&mut self, client: &mut UdpClient, report: MultiReport) -> io::Result<()> {
        let now = Instant::now();
        if now < self.next_deadline { std::thread::sleep(self.next_deadline - now); }
        client.send_report(report, 0)?;
        self.next_deadline += self.interval;
        let after = Instant::now();
        if self.next_deadline < after { self.next_deadline = after; }
        Ok(())
    }
}

impl Default for StreamRuntime { fn default() -> Self { Self::new(250) } }
