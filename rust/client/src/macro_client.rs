use ns_shared::macros::Runtime;
use ns_shared::protocol::HoriHidReport;
use std::fs;
use std::io;
use std::path::Path;

#[derive(Default)]
pub struct MacroClient { runtime: Runtime }

impl MacroClient {
    pub fn load_file(&mut self, path: &Path, now_us: u64) -> io::Result<()> {
        let text = fs::read_to_string(path)?;
        self.runtime.start_text(&text, now_us).map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error.to_string()))
    }
    pub fn start_text(&mut self, text: &str, now_us: u64) -> Result<(), String> {
        self.runtime.start_text(text, now_us).map_err(|error| error.to_string())
    }
    pub fn report(&mut self, now_us: u64) -> Option<HoriHidReport> { self.runtime.report(now_us) }
    pub fn is_running(&mut self, now_us: u64) -> bool { self.runtime.is_running(now_us, 120) }
}
