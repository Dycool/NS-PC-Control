use ns_shared::crypto::derive_key;
use ns_shared::protocol::{
    MultiReport, Packet as InputPacket, RumblePacket, FLAG_DISCONNECT,
    PACKET_SIZE as INPUT_PACKET_SIZE,
};
use std::io;
use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};
use std::time::Instant;

pub struct UdpClient {
    socket: UdpSocket,
    server: SocketAddr,
    key: [u8; 32],
    sequence: u32,
    origin: Instant,
}

impl UdpClient {
    pub fn connect(server: impl ToSocketAddrs, secret: &str) -> io::Result<Self> {
        let server = server.to_socket_addrs()?.next().ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "server address did not resolve"))?;
        let socket = UdpSocket::bind("0.0.0.0:0")?;
        socket.connect(server)?;
        socket.set_nonblocking(true)?;
        Ok(Self { socket, server, key: derive_key(secret), sequence: 0, origin: Instant::now() })
    }
    pub fn server(&self) -> SocketAddr { self.server }
    pub fn send_report(&mut self, report: MultiReport, packet_flags: u8) -> io::Result<usize> {
        let mut packet = InputPacket::default();
        packet.set_sequence(self.sequence);
        packet.set_timestamp_us(self.elapsed_us());
        packet.set_flags(packet_flags);
        *packet.report_mut() = report;
        let wire = packet.encode_authenticated(&self.key).map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "could not encode input packet"))?;
        self.sequence = self.sequence.wrapping_add(1);
        self.socket.send(&wire)
    }
    pub fn disconnect(&mut self) -> io::Result<usize> { self.send_report(MultiReport::default(), FLAG_DISCONNECT) }
    pub fn receive_rumble(&self) -> io::Result<Option<RumblePacket>> {
        let mut buffer = [0_u8; INPUT_PACKET_SIZE];
        match self.socket.recv(&mut buffer) {
            Ok(size) if size == RumblePacket::WIRE_SIZE => RumblePacket::from_wire(&buffer[..size]).map(Some).map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid rumble packet")),
            Ok(_) => Ok(None),
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(None),
            Err(error) => Err(error),
        }
    }
    pub fn key(&self) -> &[u8; 32] { &self.key }
    fn elapsed_us(&self) -> u64 { u64::try_from(self.origin.elapsed().as_micros()).unwrap_or(u64::MAX) }
}
