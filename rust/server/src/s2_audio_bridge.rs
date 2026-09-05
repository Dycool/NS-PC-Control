use crate::s2_rawgadget::RawGadgetRuntime;
use crate::udp_audio::{AudioCapabilities, AudioPcmPacket, AUDIO_PCM_SIZE};
use ns_shared::protocol::{
    S2_AUDIO_CAP_MICROPHONE, S2_AUDIO_CAP_PLAYBACK, S2_AUDIO_DIR_CLIENT_TO_CONSOLE,
    S2_AUDIO_DIR_CONSOLE_TO_CLIENT, S2_AUDIO_PCM_BYTES, S2_AUDIO_UDP_FRAMES,
    S2_AUDIO_USB_FRAME_BYTES,
};
use std::io;
use std::net::{IpAddr, SocketAddr, UdpSocket};
use std::time::Duration;

pub const AUDIO_CAPABILITY_TIMEOUT_US: u64 = 5_000_000;
const CONSOLE_SILENCE_RESET_US: u64 = 5_000;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct AudioEndpointState {
    endpoint: Option<SocketAddr>,
    capabilities: u8,
    last_seen_us: u64,
    output_sequence: u32,
    last_capabilities_sequence: u32,
    last_microphone_sequence: u32,
    have_capabilities_sequence: bool,
    have_microphone_sequence: bool,
}

pub struct S2AudioBridge {
    socket: UdpSocket,
    key: [u8; 32],
    endpoint: AudioEndpointState,
    playback_batch: [u8; S2_AUDIO_PCM_BYTES],
    playback_frames: usize,
    last_console_frame_us: u64,
}

impl S2AudioBridge {
    pub fn bind(bind_address: &str, port: u16, key: [u8; 32]) -> io::Result<Self> {
        let socket = UdpSocket::bind((bind_address, port))?;
        socket.set_nonblocking(true)?;
        Ok(Self {
            socket,
            key,
            endpoint: AudioEndpointState::default(),
            playback_batch: [0; S2_AUDIO_PCM_BYTES],
            playback_frames: 0,
            last_console_frame_us: 0,
        })
    }

    pub fn tick(
        &mut self,
        runtime: &RawGadgetRuntime,
        active_pro_ips: &[IpAddr],
        now_us: u64,
    ) -> io::Result<()> {
        self.expire_endpoint(active_pro_ips, now_us);
        self.receive_client_audio(runtime, active_pro_ips, now_us)?;
        self.drain_console_audio(runtime, active_pro_ips, now_us);
        Ok(())
    }

    #[must_use]
    pub fn headset_state(
        &mut self,
        report_timer: u8,
        active_pro_ips: &[IpAddr],
        now_us: u64,
    ) -> u8 {
        self.expire_endpoint(active_pro_ips, now_us);
        headset_state_from_capabilities(self.endpoint.capabilities, report_timer)
    }

    #[must_use]
    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.socket.local_addr()
    }

    fn receive_client_audio(
        &mut self,
        runtime: &RawGadgetRuntime,
        active_pro_ips: &[IpAddr],
        now_us: u64,
    ) -> io::Result<()> {
        let mut buffer = [0u8; AUDIO_PCM_SIZE];
        loop {
            match self.socket.recv_from(&mut buffer) {
                Ok((size, sender)) => {
                    self.handle_client_datagram(runtime, active_pro_ips, now_us, sender, &buffer[..size]);
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
                Err(error) => return Err(error),
            }
        }
    }

    fn handle_client_datagram(
        &mut self,
        runtime: &RawGadgetRuntime,
        active_pro_ips: &[IpAddr],
        now_us: u64,
        sender: SocketAddr,
        bytes: &[u8],
    ) {
        if !active_pro_ips.contains(&sender.ip()) {
            return;
        }

        if let Some(packet) = AudioCapabilities::decode(bytes, &self.key) {
            let new_endpoint = self.endpoint.endpoint != Some(sender);
            if !new_endpoint
                && self.endpoint.have_capabilities_sequence
                && !sequence_is_newer(packet.sequence(), self.endpoint.last_capabilities_sequence)
            {
                return;
            }
            if new_endpoint {
                self.endpoint.output_sequence = 0;
                self.endpoint.have_microphone_sequence = false;
                self.endpoint.last_microphone_sequence = 0;
            }
            self.endpoint.endpoint = Some(sender);
            self.endpoint.capabilities = sanitize_capabilities(packet.flags());
            self.endpoint.last_seen_us = now_us;
            self.endpoint.last_capabilities_sequence = packet.sequence();
            self.endpoint.have_capabilities_sequence = true;
            return;
        }

        let Some(packet) = AudioPcmPacket::decode(bytes, &self.key) else {
            return;
        };
        if packet.direction() != S2_AUDIO_DIR_CLIENT_TO_CONSOLE
            || self.endpoint.endpoint != Some(sender)
            || self.endpoint.capabilities & S2_AUDIO_CAP_MICROPHONE == 0
            || now_us.saturating_sub(self.endpoint.last_seen_us) > AUDIO_CAPABILITY_TIMEOUT_US
            || (self.endpoint.have_microphone_sequence
                && !sequence_is_newer(packet.sequence(), self.endpoint.last_microphone_sequence))
        {
            return;
        }
        self.endpoint.last_microphone_sequence = packet.sequence();
        self.endpoint.have_microphone_sequence = true;
        self.endpoint.last_seen_us = now_us;
        let _ = runtime.queue_microphone_audio(packet.pcm());
    }

    fn drain_console_audio(
        &mut self,
        runtime: &RawGadgetRuntime,
        active_pro_ips: &[IpAddr],
        now_us: u64,
    ) {
        if self.playback_frames != 0
            && self.last_console_frame_us != 0
            && now_us.saturating_sub(self.last_console_frame_us) >= CONSOLE_SILENCE_RESET_US
        {
            self.playback_frames = 0;
        }

        while let Some(frame) = runtime.pop_console_audio(Duration::ZERO) {
            self.last_console_frame_us = now_us;
            let offset = self.playback_frames * S2_AUDIO_USB_FRAME_BYTES;
            self.playback_batch[offset..offset + S2_AUDIO_USB_FRAME_BYTES].copy_from_slice(&frame);
            self.playback_frames += 1;
            if self.playback_frames == S2_AUDIO_UDP_FRAMES {
                self.playback_frames = 0;
                self.send_console_batch(active_pro_ips, now_us);
            }
        }
    }

    fn send_console_batch(&mut self, active_pro_ips: &[IpAddr], now_us: u64) {
        self.expire_endpoint(active_pro_ips, now_us);
        if self.endpoint.capabilities & S2_AUDIO_CAP_PLAYBACK == 0 {
            return;
        }
        let Some(endpoint) = self.endpoint.endpoint else {
            return;
        };
        let sequence = self.endpoint.output_sequence;
        self.endpoint.output_sequence = self.endpoint.output_sequence.wrapping_add(1);
        let packet = AudioPcmPacket::new(
            S2_AUDIO_DIR_CONSOLE_TO_CLIENT,
            sequence,
            now_us,
            self.playback_batch,
        );
        let encoded = packet.encode(&self.key);
        let _ = self.socket.send_to(&encoded, endpoint);
    }

    fn expire_endpoint(&mut self, active_pro_ips: &[IpAddr], now_us: u64) {
        let valid = self.endpoint.endpoint.is_some_and(|endpoint| {
            now_us.saturating_sub(self.endpoint.last_seen_us) <= AUDIO_CAPABILITY_TIMEOUT_US
                && active_pro_ips.contains(&endpoint.ip())
        });
        if !valid {
            self.endpoint = AudioEndpointState::default();
        }
    }
}

#[must_use]
pub const fn sanitize_capabilities(capabilities: u8) -> u8 {
    let mut capabilities = capabilities & (S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE);
    if capabilities & S2_AUDIO_CAP_PLAYBACK == 0 {
        capabilities &= !S2_AUDIO_CAP_MICROPHONE;
    }
    capabilities
}

#[must_use]
pub const fn headset_state_from_capabilities(capabilities: u8, report_timer: u8) -> u8 {
    let blink = if report_timer & 0x08 != 0 { 0x08 } else { 0 };
    if capabilities & S2_AUDIO_CAP_MICROPHONE != 0 {
        0x07 | blink
    } else if capabilities & S2_AUDIO_CAP_PLAYBACK != 0 {
        0x05 | blink
    } else {
        0
    }
}

#[must_use]
const fn sequence_is_newer(sequence: u32, previous: u32) -> bool {
    let delta = sequence.wrapping_sub(previous);
    delta != 0 && delta < 0x8000_0000
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn microphone_without_playback_is_rejected_like_cpp() {
        assert_eq!(sanitize_capabilities(S2_AUDIO_CAP_MICROPHONE), 0);
        assert_eq!(
            sanitize_capabilities(S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE),
            S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE
        );
    }

    #[test]
    fn headset_state_matches_cpp_timer_bit() {
        assert_eq!(headset_state_from_capabilities(0, 0), 0);
        assert_eq!(headset_state_from_capabilities(S2_AUDIO_CAP_PLAYBACK, 0), 0x05);
        assert_eq!(headset_state_from_capabilities(S2_AUDIO_CAP_PLAYBACK, 8), 0x0d);
        assert_eq!(
            headset_state_from_capabilities(
                S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE,
                0
            ),
            0x07
        );
        assert_eq!(
            headset_state_from_capabilities(
                S2_AUDIO_CAP_PLAYBACK | S2_AUDIO_CAP_MICROPHONE,
                8
            ),
            0x0f
        );
    }

    #[test]
    fn sequence_comparison_is_wrap_aware() {
        assert!(sequence_is_newer(1, 0));
        assert!(sequence_is_newer(0, u32::MAX));
        assert!(!sequence_is_newer(10, 10));
        assert!(!sequence_is_newer(9, 10));
    }
}
