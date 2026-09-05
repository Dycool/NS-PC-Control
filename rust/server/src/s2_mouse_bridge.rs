use crate::s2_mouse::MouseStreamState;
use crate::s2_reports::JoyconMouseInput;
use ns_shared::joycon_mouse::{
    JoyconMousePacket, JOYCON_MOUSE_MAGIC, JOYCON_MOUSE_PACKET_SIZE,
};
use std::sync::{Mutex, OnceLock};

const S2_MOUSE_FEATURE: u32 = 0x10;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct SessionKey {
    client_index: usize,
    connected_us: u64,
}

#[derive(Default)]
pub struct S2MouseBridge {
    session: Option<SessionKey>,
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
        connected_us: u64,
        now_us: u64,
    ) -> bool {
        if bytes.len() != JOYCON_MOUSE_PACKET_SIZE
            || bytes
                .get(..4)
                .and_then(|prefix| prefix.try_into().ok())
                .map(u32::from_le_bytes)
                != Some(JOYCON_MOUSE_MAGIC)
        {
            return false;
        }
        let Some(packet) = JoyconMousePacket::decode_authenticated(bytes, key) else {
            // A correctly-sized NSJM packet belongs to this protocol even if
            // authentication or validation fails. Consume it fail-closed so it
            // cannot fall through and be interpreted as another packet type.
            return true;
        };
        let session = SessionKey {
            client_index,
            connected_us,
        };
        if self.session != Some(session) {
            self.streams.fill(MouseStreamState::default());
            self.session = Some(session);
        }
        let stream = &mut self.streams[usize::from(packet.subpad())];
        let _ = stream.update(packet, now_us);
        true
    }

    #[must_use]
    pub fn consume_p1(
        &mut self,
        client_index: usize,
        connected_us: u64,
        now_us: u64,
        enabled_features: u32,
    ) -> JoyconMouseInput {
        let session = SessionKey {
            client_index,
            connected_us,
        };
        if self.session != Some(session) {
            return JoyconMouseInput::default();
        }
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

fn global_bridge() -> &'static Mutex<S2MouseBridge> {
    static BRIDGE: OnceLock<Mutex<S2MouseBridge>> = OnceLock::new();
    BRIDGE.get_or_init(|| Mutex::new(S2MouseBridge::default()))
}

#[must_use]
pub fn handle_global_authenticated_datagram(
    bytes: &[u8],
    key: &[u8; 32],
    client_index: usize,
    connected_us: u64,
    now_us: u64,
) -> bool {
    global_bridge()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .handle_authenticated_datagram(bytes, key, client_index, connected_us, now_us)
}

#[must_use]
pub fn consume_global_p1(
    client_index: usize,
    connected_us: u64,
    now_us: u64,
    enabled_features: u32,
) -> JoyconMouseInput {
    global_bridge()
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .consume_p1(client_index, connected_us, now_us, enabled_features)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::joycon_mouse::{
        JOYCON_MOUSE_FLAG_ACTIVE, JOYCON_MOUSE_FLAG_RIGHT_BUTTON,
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
        assert!(bridge.handle_authenticated_datagram(&encoded, &key, 2, 50, 10_000));
        let disabled = bridge.consume_p1(2, 50, 10_000, 0);
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
            50,
            11_000,
        ));
        let enabled = bridge.consume_p1(2, 50, 11_000, S2_MOUSE_FEATURE);
        assert!(enabled.active());
    }

    #[test]
    fn reused_client_slot_is_isolated_by_connection_epoch() {
        let key = [9; 32];
        let packet = JoyconMousePacket::new(JOYCON_MOUSE_FLAG_ACTIVE, 0, 1, 100, 0, 0, 1);
        let mut bridge = S2MouseBridge::default();
        assert!(bridge.handle_authenticated_datagram(
            &packet.encode_authenticated(&key),
            &key,
            0,
            100,
            1,
        ));
        let sample = bridge.consume_p1(0, 101, 2, S2_MOUSE_FEATURE);
        assert!(!sample.active());
    }

    #[test]
    fn unrelated_47_byte_packet_is_not_consumed_without_mouse_magic() {
        let key = [1; 32];
        let mut bridge = S2MouseBridge::default();
        assert!(!bridge.handle_authenticated_datagram(
            &[0; JOYCON_MOUSE_PACKET_SIZE],
            &key,
            0,
            1,
            1,
        ));
    }
}
