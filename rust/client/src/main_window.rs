use std::io;
use std::process::Command;

pub fn open_web_ui(url: &str) -> io::Result<()> {
    #[cfg(target_os = "windows")]
    let status = Command::new("cmd").args(["/C", "start", "", url]).status()?;
    #[cfg(target_os = "macos")]
    let status = Command::new("open").arg(url).status()?;
    #[cfg(all(unix, not(target_os = "macos")))]
    let status = Command::new("xdg-open").arg(url).status()?;
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    let status = return Err(io::Error::new(io::ErrorKind::Unsupported, "no browser launcher is available for this platform"));
    if status.success() { Ok(()) } else { Err(io::Error::other("browser launcher returned a failure status")) }
}
