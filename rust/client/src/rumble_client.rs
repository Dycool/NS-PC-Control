use ns_shared::protocol::RumblePacket;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RumbleState { packet: Option<RumblePacket> }

impl RumbleState {
    pub fn update(&mut self, packet: RumblePacket) { self.packet = Some(packet); }
    pub fn current(&self) -> Option<RumblePacket> { self.packet }
    pub fn clear(&mut self) { self.packet = None; }
}
