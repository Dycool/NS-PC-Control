use std::fs;
use std::io::{self, BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

pub struct WebServer { root: PathBuf, port: u16 }
impl WebServer {
    pub fn new(root: impl Into<PathBuf>, port: u16) -> Self { Self { root: root.into(), port } }
    pub fn serve(&self, running: Arc<AtomicBool>) -> io::Result<()> {
        let listener = TcpListener::bind(("0.0.0.0", self.port))?;
        listener.set_nonblocking(true)?;
        while running.load(Ordering::Acquire) {
            match listener.accept() {
                Ok((stream, _)) => { let _ = self.handle_connection(stream); }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(20)),
                Err(error) => return Err(error),
            }
        }
        Ok(())
    }
    fn handle_connection(&self, mut stream: TcpStream) -> io::Result<()> {
        stream.set_read_timeout(Some(Duration::from_secs(2)))?;
        let mut reader = BufReader::new(stream.try_clone()?);
        let mut request_line = String::new();
        reader.read_line(&mut request_line)?;
        let mut parts = request_line.split_whitespace();
        if parts.next() != Some("GET") { return write_response(&mut stream, 405, "text/plain; charset=utf-8", b"Method Not Allowed"); }
        let request_path = parts.next().unwrap_or("/");
        let path = safe_join(&self.root, request_path).unwrap_or_else(|| self.root.join("index.html"));
        let path = if path.is_dir() { path.join("index.html") } else { path };
        match fs::read(&path) {
            Ok(body) => write_response(&mut stream, 200, mime_for(&path), &body),
            Err(_) => {
                let fallback = self.root.join("index.html");
                match fs::read(&fallback) {
                    Ok(body) => write_response(&mut stream, 200, "text/html; charset=utf-8", &body),
                    Err(_) => write_response(&mut stream, 404, "text/plain; charset=utf-8", b"Not Found"),
                }
            }
        }
    }
}

fn safe_join(root: &Path, request_path: &str) -> Option<PathBuf> {
    let path = request_path.split('?').next().unwrap_or("/").trim_start_matches('/');
    let relative = Path::new(path);
    if relative.components().any(|component| matches!(component, Component::ParentDir | Component::RootDir | Component::Prefix(_))) { return None; }
    Some(root.join(if path.is_empty() { "index.html" } else { path }))
}
fn mime_for(path: &Path) -> &'static str {
    match path.extension().and_then(|extension| extension.to_str()).unwrap_or_default() {
        "html" => "text/html; charset=utf-8", "css" => "text/css; charset=utf-8", "js" => "text/javascript; charset=utf-8", "json" => "application/json", "svg" => "image/svg+xml", "png" => "image/png", "ico" => "image/x-icon", _ => "application/octet-stream",
    }
}
fn write_response(stream: &mut TcpStream, status: u16, content_type: &str, body: &[u8]) -> io::Result<()> {
    let reason = match status { 200 => "OK", 404 => "Not Found", 405 => "Method Not Allowed", _ => "Error" };
    write!(stream, "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len())?;
    stream.write_all(body)
}

#[cfg(test)]
mod tests {
    use super::safe_join;
    use std::path::Path;
    #[test]
    fn rejects_directory_traversal() {
        assert!(safe_join(Path::new("webapp"), "/js/core.js").is_some());
        assert!(safe_join(Path::new("webapp"), "/../secret").is_none());
    }
}
