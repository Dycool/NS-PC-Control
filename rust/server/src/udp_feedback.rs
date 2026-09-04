use crate::app_state::{ServerContext, UdpFeedbackBatch};
use ns_shared::protocol::RumblePacket;
use std::collections::VecDeque;
use std::io;
use std::net::{SocketAddr, UdpSocket};
use std::sync::Mutex;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FeedbackEvent {
    target: SocketAddr,
    packet: RumblePacket,
}

impl FeedbackEvent {
    #[must_use]
    pub const fn new(target: SocketAddr, packet: RumblePacket) -> Self {
        Self { target, packet }
    }

    #[must_use]
    pub const fn target(&self) -> SocketAddr {
        self.target
    }

    #[must_use]
    pub const fn packet(&self) -> RumblePacket {
        self.packet
    }
}

#[derive(Default)]
pub struct FeedbackQueue {
    queue: Mutex<VecDeque<FeedbackEvent>>,
}

impl FeedbackQueue {
    pub fn push(&self, event: FeedbackEvent) {
        self.queue
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .push_back(event);
    }

    pub fn pop(&self) -> Option<FeedbackEvent> {
        self.queue
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .pop_front()
    }
}

pub fn flush_feedback_to_udp(
    socket: &UdpSocket,
    context: &ServerContext,
    client_index: usize,
    now_us: u64,
) -> io::Result<usize> {
    let Some(batch) = context.take_udp_feedback(client_index, now_us) else {
        return Ok(0);
    };
    send_batch(socket, &batch)
}

fn send_batch(socket: &UdpSocket, batch: &UdpFeedbackBatch) -> io::Result<usize> {
    let mut sent_packets = 0usize;
    for packet in batch.rumble() {
        send_exact(socket, &packet.encode(), batch.target())?;
        sent_packets += 1;
    }
    for packet in batch.assignments() {
        send_exact(socket, &packet.encode(), batch.target())?;
        sent_packets += 1;
    }
    for packet in batch.controller_status() {
        send_exact(socket, &packet.encode(), batch.target())?;
        sent_packets += 1;
    }
    if let Some(packet) = batch.roster() {
        send_exact(socket, &packet.encode(), batch.target())?;
        sent_packets += 1;
    }
    for packet in batch.amiibo_requests() {
        send_exact(socket, &packet.encode(), batch.target())?;
        sent_packets += 1;
    }
    for packet in batch.amiibo_data() {
        let encoded = packet.encode();
        let packet_size = 7 + packet.data().len();
        send_exact(socket, &encoded[..packet_size], batch.target())?;
        sent_packets += 1;
    }
    Ok(sent_packets)
}

fn send_exact(socket: &UdpSocket, bytes: &[u8], target: SocketAddr) -> io::Result<()> {
    let sent = socket.send_to(bytes, target)?;
    if sent == bytes.len() {
        Ok(())
    } else {
        Err(io::Error::new(
            io::ErrorKind::WriteZero,
            format!("short UDP datagram write: {sent}/{}", bytes.len()),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::flush_feedback_to_udp;
    use crate::app_state::{ClientAssignmentState, ServerContext};
    use ns_shared::control_packets::ClientAssignmentPacket;
    use ns_shared::protocol::{ControllerType, MultiReport};
    use std::net::{Ipv4Addr, SocketAddr, UdpSocket};
    use std::time::Duration;

    #[test]
    fn flush_sends_wire_packets_to_registered_udp_client() {
        let receiver = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).expect("receiver");
        receiver
            .set_read_timeout(Some(Duration::from_secs(1)))
            .expect("timeout");
        let sender = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).expect("sender");
        let target: SocketAddr = receiver.local_addr().expect("target");
        let context = ServerContext::default();
        let slot = context.register_udp(target, 1_000).expect("slot");
        context
            .update_udp_report(slot, 1, MultiReport::default(), 1_000)
            .expect("input");
        context.enable_udp_feedback(slot).expect("feedback");
        context
            .publish_assignment(
                slot,
                0,
                ClientAssignmentState::new(1, 0, ControllerType::Pro, ControllerType::Pro),
            )
            .expect("assignment");

        let sent = flush_feedback_to_udp(&sender, &context, slot, 1_000).expect("flush");
        assert!(sent >= 2, "assignment and roster/state should be emitted");

        let mut buffer = [0u8; 512];
        let mut found_assignment = false;
        for _ in 0..sent {
            let (size, _) = receiver.recv_from(&mut buffer).expect("packet");
            if size == 16 && ClientAssignmentPacket::decode(&buffer[..size]).is_ok() {
                found_assignment = true;
            }
        }
        assert!(found_assignment);
    }
}
