use ns_shared::protocol::HoriHidReport;
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

pub trait VirtualController: Send {
    fn write_report(&mut self, report: HoriHidReport) -> io::Result<()>;
    fn path(&self) -> &Path;
}

pub struct HidGadgetController {
    path: PathBuf,
    file: File,
}

impl HidGadgetController {
    pub fn open(path: impl Into<PathBuf>) -> io::Result<Self> {
        let path = path.into();
        let file = OpenOptions::new().write(true).open(&path)?;
        Ok(Self { path, file })
    }
}

impl VirtualController for HidGadgetController {
    fn write_report(&mut self, report: HoriHidReport) -> io::Result<()> {
        self.file.write_all(&report.to_wire())
    }

    fn path(&self) -> &Path { &self.path }
}

#[derive(Default)]
pub struct MemoryController {
    last_report: HoriHidReport,
    writes: u64,
}

impl MemoryController {
    pub fn last_report(&self) -> HoriHidReport { self.last_report }
    pub fn writes(&self) -> u64 { self.writes }
}

impl VirtualController for MemoryController {
    fn write_report(&mut self, report: HoriHidReport) -> io::Result<()> {
        self.last_report = report;
        self.writes = self.writes.wrapping_add(1);
        Ok(())
    }

    fn path(&self) -> &Path { Path::new("memory://controller") }
}
