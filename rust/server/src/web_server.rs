use crate::app_state::{ServerContext, UdpFeedbackBatch};
use ns_shared::protocol::{
    MultiReport, FLAG_DISCONNECT, FLAG_SINGLE_PAD, PACKET_SIZE, PROTO_MAGIC, WEB_PACKET_SIZE,
    WEB_PROTO_VERSION, WEB_PROTO_VERSION_3,
};
use std::fs;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

const WEBSOCKET_GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const WEBSOCKET_PROTOCOL: &str = "nspc-protocol";
const MAX_HTTP_HEADER_BYTES: usize = 32 * 1024;
const MAX_WEBSOCKET_PAYLOAD: usize = 64 * 1024;

pub struct WebServer {
    root: PathBuf,
    port: u16,
    context: Arc<ServerContext>,
}

impl WebServer {
    #[must_use]
    pub fn new(root: impl Into<PathBuf>, port: u16, context: Arc<ServerContext>) -> Self {
        Self {
            root: root.into(),
            port,
            context,
        }
    }

    pub fn serve(&self, running: Arc<AtomicBool>) -> io::Result<()> {
        let listener = TcpListener::bind(("0.0.0.0", self.port))?;
        listener.set_nonblocking(true)?;
        while running.load(Ordering::Acquire) {
            match listener.accept() {
                Ok((stream, _)) => {
                    let root = self.root.clone();
                    let context = Arc::clone(&self.context);
                    let connection_running = Arc::clone(&running);
                    thread::spawn(move || {
                        let _ = handle_connection(stream, &root, &context, &connection_running);
                    });
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(20));
                }
                Err(error) => return Err(error),
            }
        }
        Ok(())
    }
}

fn handle_connection(
    stream: TcpStream,
    root: &Path,
    context: &ServerContext,
    running: &AtomicBool,
) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    let peer = stream.peer_addr()?;
    let mut reader = BufReader::new(stream);
    let request = read_http_request(&mut reader)?;
    if request.method != "GET" {
        return write_response(
            reader.get_mut(),
            405,
            "text/plain; charset=utf-8",
            b"Method Not Allowed",
        );
    }

    if request.is_websocket_upgrade() {
        let key = request
            .header("sec-websocket-key")
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing WebSocket key"))?;
        let accept = websocket_accept(key);
        let protocol = request
            .header("sec-websocket-protocol")
            .and_then(select_websocket_protocol);
        write_websocket_handshake(reader.get_mut(), &accept, protocol)?;
        let stream = reader.into_inner();
        return websocket_loop(stream, peer, context, running);
    }

    serve_static(reader.get_mut(), root, &request.path)
}

#[derive(Debug)]
struct HttpRequest {
    method: String,
    path: String,
    headers: Vec<(String, String)>,
}

impl HttpRequest {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(key, _)| key.eq_ignore_ascii_case(name))
            .map(|(_, value)| value.as_str())
    }

    fn is_websocket_upgrade(&self) -> bool {
        self.header("upgrade")
            .is_some_and(|value| value.eq_ignore_ascii_case("websocket"))
            && self.header("connection").is_some_and(|value| {
                value
                    .split(',')
                    .any(|token| token.trim().eq_ignore_ascii_case("upgrade"))
            })
            && self
                .header("sec-websocket-version")
                .is_some_and(|value| value.trim() == "13")
    }
}

fn read_http_request(reader: &mut BufReader<TcpStream>) -> io::Result<HttpRequest> {
    let mut total = 0usize;
    let mut request_line = String::new();
    let bytes = reader.read_line(&mut request_line)?;
    total += bytes;
    if bytes == 0 || total > MAX_HTTP_HEADER_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid HTTP request line",
        ));
    }
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or_default().to_owned();
    let path = parts.next().unwrap_or("/").to_owned();
    let version = parts.next().unwrap_or_default();
    if !version.starts_with("HTTP/1.") {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unsupported HTTP version",
        ));
    }

    let mut headers = Vec::new();
    loop {
        let mut line = String::new();
        let bytes = reader.read_line(&mut line)?;
        total = total.saturating_add(bytes);
        if total > MAX_HTTP_HEADER_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "HTTP headers exceed size limit",
            ));
        }
        if bytes == 0 || line == "\r\n" || line == "\n" {
            break;
        }
        let line = line.trim_end_matches(['\r', '\n']);
        if let Some((name, value)) = line.split_once(':') {
            headers.push((name.trim().to_owned(), value.trim().to_owned()));
        }
    }
    Ok(HttpRequest {
        method,
        path,
        headers,
    })
}

fn select_websocket_protocol(value: &str) -> Option<&'static str> {
    value
        .split(',')
        .any(|protocol| protocol.trim() == WEBSOCKET_PROTOCOL)
        .then_some(WEBSOCKET_PROTOCOL)
}

fn write_websocket_handshake(
    stream: &mut TcpStream,
    accept: &str,
    protocol: Option<&str>,
) -> io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n"
    )?;
    if let Some(protocol) = protocol {
        write!(stream, "Sec-WebSocket-Protocol: {protocol}\r\n")?;
    }
    write!(stream, "\r\n")?;
    stream.flush()
}

fn websocket_loop(
    mut stream: TcpStream,
    peer: std::net::SocketAddr,
    context: &ServerContext,
    running: &AtomicBool,
) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_millis(100)))?;
    stream.set_write_timeout(Some(Duration::from_secs(1)))?;
    let origin = Instant::now();
    let mut slot = None;

    while running.load(Ordering::Acquire) && context.is_running() {
        match read_websocket_frame(&mut stream) {
            Ok(Some(frame)) => match frame.opcode {
                0x2 => {
                    if let Some(packet) = parse_web_input_packet(&frame.payload) {
                        let now_us = elapsed_us(&origin);
                        let client_slot = match slot {
                            Some(slot) => slot,
                            None => {
                                let allocated = context
                                    .register_udp(peer, now_us)
                                    .map_err(|_| io::Error::other("server client capacity is full"))?;
                                context
                                    .enable_udp_feedback(allocated)
                                    .map_err(|_| io::Error::other("cannot enable WebSocket feedback"))?;
                                slot = Some(allocated);
                                allocated
                            }
                        };
                        if packet.flags & FLAG_DISCONNECT != 0 {
                            let _ = context.disconnect(client_slot);
                            write_websocket_frame(&mut stream, 0x8, &[])?;
                            return Ok(());
                        }
                        let _ = context.update_udp_report(
                            client_slot,
                            packet.sequence,
                            packet.report,
                            now_us,
                        );
                    }
                }
                0x8 => {
                    write_websocket_frame(&mut stream, 0x8, &frame.payload[..frame.payload.len().min(125)])?;
                    break;
                }
                0x9 => {
                    write_websocket_frame(&mut stream, 0xA, &frame.payload)?;
                }
                0xA | 0x1 => {}
                _ => {
                    write_websocket_close(&mut stream, 1003, "unsupported frame")?;
                    break;
                }
            },
            Ok(None) => break,
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {}
            Err(error) => return Err(error),
        }

        if let Some(client_slot) = slot {
            let now_us = elapsed_us(&origin);
            if let Some(batch) = context.take_udp_feedback(client_slot, now_us) {
                write_feedback_batch(&mut stream, &batch)?;
            }
        }
    }

    if let Some(client_slot) = slot {
        let _ = context.disconnect(client_slot);
    }
    Ok(())
}

#[derive(Clone, Copy, Debug)]
struct WebInputPacket {
    flags: u8,
    sequence: u32,
    report: MultiReport,
}

fn parse_web_input_packet(bytes: &[u8]) -> Option<WebInputPacket> {
    if bytes.len() != WEB_PACKET_SIZE && bytes.len() != PACKET_SIZE {
        return None;
    }
    if u32::from_le_bytes(bytes[..4].try_into().ok()?) != PROTO_MAGIC {
        return None;
    }
    if bytes[4] != WEB_PROTO_VERSION && bytes[4] != WEB_PROTO_VERSION_3 {
        return None;
    }
    let flags = bytes[5];
    let sequence = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
    let mut pads = *MultiReport::decode(&bytes[20..212]).ok()?.pads();
    if flags & FLAG_SINGLE_PAD != 0 {
        pads[1..].fill(Default::default());
        pads[0].set_pad_present(true);
    }
    Some(WebInputPacket {
        flags,
        sequence,
        report: MultiReport::new(pads),
    })
}

fn write_feedback_batch(stream: &mut TcpStream, batch: &UdpFeedbackBatch) -> io::Result<()> {
    for packet in batch.rumble() {
        write_websocket_frame(stream, 0x2, &packet.encode())?;
    }
    for packet in batch.controller_status() {
        write_websocket_frame(stream, 0x2, &packet.encode())?;
    }
    for packet in batch.assignments() {
        write_websocket_frame(stream, 0x2, &packet.encode())?;
    }
    for packet in batch.amiibo_requests() {
        write_websocket_frame(stream, 0x2, &packet.encode())?;
    }
    for packet in batch.amiibo_data() {
        let encoded = packet.encode();
        write_websocket_frame(stream, 0x2, &encoded[..7 + packet.data().len()])?;
    }
    if let Some(roster) = batch.roster() {
        write_websocket_frame(stream, 0x2, &roster.encode())?;
    }
    Ok(())
}

#[derive(Debug)]
struct WebSocketFrame {
    opcode: u8,
    payload: Vec<u8>,
}

fn read_websocket_frame(stream: &mut TcpStream) -> io::Result<Option<WebSocketFrame>> {
    let mut header = [0u8; 2];
    match stream.read_exact(&mut header) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(error),
    }
    let final_frame = header[0] & 0x80 != 0;
    let opcode = header[0] & 0x0f;
    let masked = header[1] & 0x80 != 0;
    if !final_frame || !masked {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "fragmented or unmasked client WebSocket frame",
        ));
    }
    let mut payload_len = u64::from(header[1] & 0x7f);
    if payload_len == 126 {
        let mut extended = [0u8; 2];
        stream.read_exact(&mut extended)?;
        payload_len = u64::from(u16::from_be_bytes(extended));
    } else if payload_len == 127 {
        let mut extended = [0u8; 8];
        stream.read_exact(&mut extended)?;
        payload_len = u64::from_be_bytes(extended);
    }
    let payload_len = usize::try_from(payload_len).map_err(|_| {
        io::Error::new(io::ErrorKind::InvalidData, "WebSocket payload size overflows usize")
    })?;
    if payload_len > MAX_WEBSOCKET_PAYLOAD || (opcode & 0x08 != 0 && payload_len > 125) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "WebSocket payload exceeds size limit",
        ));
    }
    let mut mask = [0u8; 4];
    stream.read_exact(&mut mask)?;
    let mut payload = vec![0u8; payload_len];
    stream.read_exact(&mut payload)?;
    for (index, byte) in payload.iter_mut().enumerate() {
        *byte ^= mask[index % mask.len()];
    }
    Ok(Some(WebSocketFrame { opcode, payload }))
}

fn write_websocket_frame(stream: &mut TcpStream, opcode: u8, payload: &[u8]) -> io::Result<()> {
    if payload.len() > MAX_WEBSOCKET_PAYLOAD {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "WebSocket output payload exceeds size limit",
        ));
    }
    let mut header = Vec::with_capacity(10);
    header.push(0x80 | (opcode & 0x0f));
    match payload.len() {
        0..=125 => header.push(payload.len() as u8),
        126..=65_535 => {
            header.push(126);
            header.extend_from_slice(&(payload.len() as u16).to_be_bytes());
        }
        _ => {
            header.push(127);
            header.extend_from_slice(&(payload.len() as u64).to_be_bytes());
        }
    }
    stream.write_all(&header)?;
    stream.write_all(payload)?;
    stream.flush()
}

fn write_websocket_close(stream: &mut TcpStream, code: u16, reason: &str) -> io::Result<()> {
    let reason = reason.as_bytes();
    let reason = &reason[..reason.len().min(123)];
    let mut payload = Vec::with_capacity(2 + reason.len());
    payload.extend_from_slice(&code.to_be_bytes());
    payload.extend_from_slice(reason);
    write_websocket_frame(stream, 0x8, &payload)
}

fn websocket_accept(key: &str) -> String {
    let mut input = Vec::with_capacity(key.trim().len() + WEBSOCKET_GUID.len());
    input.extend_from_slice(key.trim().as_bytes());
    input.extend_from_slice(WEBSOCKET_GUID.as_bytes());
    base64_encode(&sha1(&input))
}

fn sha1(input: &[u8]) -> [u8; 20] {
    let mut message = input.to_vec();
    let bit_length = (input.len() as u64).wrapping_mul(8);
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend_from_slice(&bit_length.to_be_bytes());

    let mut h0 = 0x6745_2301u32;
    let mut h1 = 0xefcd_ab89u32;
    let mut h2 = 0x98ba_dcfeu32;
    let mut h3 = 0x1032_5476u32;
    let mut h4 = 0xc3d2_e1f0u32;

    let (chunks, remainder) = message.as_chunks::<64>();
    debug_assert!(remainder.is_empty());
    for chunk in chunks {
        let mut words = [0u32; 80];
        for (index, word) in words[..16].iter_mut().enumerate() {
            let start = index * 4;
            *word = u32::from_be_bytes(
                chunk[start..start + 4]
                    .try_into()
                    .expect("SHA-1 word has four bytes"),
            );
        }
        for index in 16..80 {
            words[index] = (words[index - 3]
                ^ words[index - 8]
                ^ words[index - 14]
                ^ words[index - 16])
                .rotate_left(1);
        }
        let mut a = h0;
        let mut b = h1;
        let mut c = h2;
        let mut d = h3;
        let mut e = h4;
        for (index, word) in words.iter().copied().enumerate() {
            let (function, constant) = match index {
                0..=19 => ((b & c) | ((!b) & d), 0x5a82_7999),
                20..=39 => (b ^ c ^ d, 0x6ed9_eba1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8f1b_bcdc),
                _ => (b ^ c ^ d, 0xca62_c1d6),
            };
            let temp = a
                .rotate_left(5)
                .wrapping_add(function)
                .wrapping_add(e)
                .wrapping_add(constant)
                .wrapping_add(word);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = temp;
        }
        h0 = h0.wrapping_add(a);
        h1 = h1.wrapping_add(b);
        h2 = h2.wrapping_add(c);
        h3 = h3.wrapping_add(d);
        h4 = h4.wrapping_add(e);
    }

    let mut digest = [0u8; 20];
    for (index, word) in [h0, h1, h2, h3, h4].iter().copied().enumerate() {
        digest[index * 4..index * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    digest
}

fn base64_encode(input: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut output = String::with_capacity(input.len().div_ceil(3) * 4);
    for chunk in input.chunks(3) {
        let first = chunk[0];
        let second = chunk.get(1).copied().unwrap_or(0);
        let third = chunk.get(2).copied().unwrap_or(0);
        output.push(TABLE[usize::from(first >> 2)] as char);
        output.push(TABLE[usize::from(((first & 0x03) << 4) | (second >> 4))] as char);
        if chunk.len() > 1 {
            output.push(TABLE[usize::from(((second & 0x0f) << 2) | (third >> 6))] as char);
        } else {
            output.push('=');
        }
        if chunk.len() > 2 {
            output.push(TABLE[usize::from(third & 0x3f)] as char);
        } else {
            output.push('=');
        }
    }
    output
}

fn elapsed_us(origin: &Instant) -> u64 {
    u64::try_from(origin.elapsed().as_micros()).unwrap_or(u64::MAX)
}

fn serve_static(stream: &mut TcpStream, root: &Path, request_path: &str) -> io::Result<()> {
    let path = safe_join(root, request_path).unwrap_or_else(|| root.join("index.html"));
    let path = if path.is_dir() {
        path.join("index.html")
    } else {
        path
    };
    match fs::read(&path) {
        Ok(body) => write_response(stream, 200, mime_for(&path), &body),
        Err(_) => {
            let fallback = root.join("index.html");
            match fs::read(&fallback) {
                Ok(body) => write_response(stream, 200, "text/html; charset=utf-8", &body),
                Err(_) => write_response(
                    stream,
                    404,
                    "text/plain; charset=utf-8",
                    b"Not Found",
                ),
            }
        }
    }
}

fn safe_join(root: &Path, request_path: &str) -> Option<PathBuf> {
    let path = request_path
        .split('?')
        .next()
        .unwrap_or("/")
        .trim_start_matches('/');
    let relative = Path::new(path);
    if relative.components().any(|component| {
        matches!(
            component,
            Component::ParentDir | Component::RootDir | Component::Prefix(_)
        )
    }) {
        return None;
    }
    Some(root.join(if path.is_empty() { "index.html" } else { path }))
}

fn mime_for(path: &Path) -> &'static str {
    match path
        .extension()
        .and_then(|extension| extension.to_str())
        .unwrap_or_default()
    {
        "html" => "text/html; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "js" => "text/javascript; charset=utf-8",
        "json" => "application/json",
        "svg" => "image/svg+xml",
        "png" => "image/png",
        "ico" => "image/x-icon",
        _ => "application/octet-stream",
    }
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        404 => "Not Found",
        405 => "Method Not Allowed",
        _ => "Error",
    };
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

#[cfg(test)]
mod tests {
    use super::{base64_encode, parse_web_input_packet, safe_join, sha1, websocket_accept};
    use ns_shared::protocol::{MultiReport, PROTO_MAGIC, WEB_PACKET_SIZE, WEB_PROTO_VERSION};
    use std::path::Path;

    #[test]
    fn rejects_directory_traversal() {
        assert!(safe_join(Path::new("webapp"), "/js/core.js").is_some());
        assert!(safe_join(Path::new("webapp"), "/../secret").is_none());
    }

    #[test]
    fn websocket_accept_matches_rfc6455_example() {
        assert_eq!(
            websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        );
        assert_eq!(
            base64_encode(&sha1(b"abc")),
            "qZk+NkcGgWq6PiVxeFDCbJzQ2J0="
        );
    }

    #[test]
    fn accepts_cpp_web_packet_layout_without_hmac() {
        let mut packet = [0u8; WEB_PACKET_SIZE];
        packet[..4].copy_from_slice(&PROTO_MAGIC.to_le_bytes());
        packet[4] = WEB_PROTO_VERSION;
        packet[8..12].copy_from_slice(&42u32.to_le_bytes());
        packet[20..].copy_from_slice(&MultiReport::default().encode());
        let parsed = parse_web_input_packet(&packet).expect("web packet");
        assert_eq!(parsed.sequence, 42);
    }
}
