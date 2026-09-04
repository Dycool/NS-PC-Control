use ns_shared::protocol::RumblePacket;
use std::collections::VecDeque;
use std::net::SocketAddr;
use std::sync::Mutex;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FeedbackEvent { target: SocketAddr, packet: RumblePacket }
impl FeedbackEvent {
    pub fn new(target: SocketAddr, packet: RumblePacket) -> Self { Self { target, packet } }
    pub fn target(&self) -> SocketAddr { self.target }
    pub fn packet(&self) -> RumblePacket { self.packet }
}

#[derive(Default)]
pub struct FeedbackQueue { queue: Mutex<VecDeque<FeedbackEvent>> }
impl FeedbackQueue {
    pub fn push(&self, event: FeedbackEvent) { self.queue.lock().unwrap_or_else(|poison| poison.into_inner()).push_back(event); }
    pub fn pop(&self) -> Option<FeedbackEvent> { self.queue.lock().unwrap_or_else(|poison| poison.into_inner()).pop_front() }
}
