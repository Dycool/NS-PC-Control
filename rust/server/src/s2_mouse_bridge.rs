use crate::s2_mouse::MouseStreamState;
use crate::s2_reports::JoyconMouseInput;
use ns_shared::joycon_mouse::{JoyconMousePacket, JOYCON_MOUSE_PACKET_SIZE};

const S2_MOUSE_FEATURE: u32 = 0x10;

#[derive(Default)]
pub struct S2MouseBridge {
    client_index: Option<usize>,
    streams: [MouseStreamState; 4],
}

impl S2MouseBridge {
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    #[must_use]
    pub fn handle_authenticated_datagram(
        &mut self,
        bytes: &[u8],
        key: &[u8; 32],
        client_index: usize,
        now_us: u64,
    ) -> bool {
        if bytes.len() != JOYCON_MOUSE_PACKET_SIZE {
            return false;
        }
        let Some(packet) = JoyconMousePacket::decode_authenticated(bytes, key) else {
            // The magic/size family belongs to this protocol even if
            // authentication or validation fails. Consume it fail-closed so it
            // cannot fall through and be interpreted as another packet type.
            return true;
        };
        if self.client_index != Some(client_index) {
            self.streams.fill(MouseStreamState::default());
            self.client_index = Some(client_index);
        }
        let stream = &mut self.streams[usize::from(packet.subpad())];
        let _ = stream.update(packet, now_us);
        true
    }

    pub fn retain_client(&mut self, client_index: Option<usize>) {
        if self.client_index != client_index {
            self.reset();
            self.client_index = client_index;
        }
    }

    #[must_use]
    pub fn consume_p1(&mut self, now_us: u64, enabled_features: u32) -> JoyconMouseInput {
        let sample = self.streams[0].consume(now_us, enabled_features & S2_MOUSE_FEATURE != 0);
        JoyconMouseInput::new(
            sample.dx,
            sample.dy,
            sample.scroll_y,
            sample.left_down,
            sample.right_down,
            sample.active,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::joycon_mouse::{
        JoyconMousePacket, JOYCON_MOUSE_FLAG_ACTIVE, JOYCON_MOUSE_FLAG_RIGHT_BUTTON,
    };

    #[test]
    fn only_authenticated_packet_updates_live_sample() {
        let key = [0x44; 32];
        let packet = JoyconMousePacket::new(
            JOYCON_MOUSE_FLAG_ACTIVE | JOYCON_MOUSE_FLAG_RIGHT_BUTTON,
            0,
            1,
            16,
            -8,
            120,
            1_000,
        );
        let encoded = packet.encode_authenticated(&key);
        let mut bridge = S2MouseBridge::default();
        assert!(bridge.handle_authenticated_datagram(&encoded, &key, 2, 10_000));
        let disabled = bridge.consume_p1(10_000, 0);
        assert!(!disabled.active());

        assert!(bridge.handle_authenticated_datagram(
            &JoyconMousePacket::new(
                JOYCON_MOUSE_FLAG_ACTIVE | JOYCON_MOUSE_FLAG_RIGHT_BUTTON,
                0,
                2,
                16,
                -8,
                120,
                5_000,
            )
            .encode_authenticated(&key),
            &key,
            2,
            11_000,
        ));
        let enabled = bridge.consume_p1(11_000, S2_MOUSE_FEATURE);
        assert!(enabled.active());
    }

    #[test]
    fn client_change_drops_pending_stream_state() {
        let key = [9; 32];
        let packet = JoyconMousePacket::new(JOYCON_MOUSE_FLAG_ACTIVE, 0, 1, 100, 0, 0, 1);
        let mut bridge = S2MouseBridge::default();
        assert!(bridge.handle_authenticated_datagram(
            &packet.encode_authenticated(&key),
            &key,
            0,
            1,
        ));
        bridge.retain_client(Some(1));
        let sample = bridge.consume_p1(2, S2_MOUSE_FEATURE);
        assert!(!sample.active());
    }
}
