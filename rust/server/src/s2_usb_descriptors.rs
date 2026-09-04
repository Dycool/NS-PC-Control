use crate::virtual_controller::S2_PRO_REPORT_DESC;
use ns_shared::protocol::S2_AUDIO_USB_FRAME_BYTES;

pub const HID_INTERFACE: u8 = 0;
pub const VENDOR_INTERFACE: u8 = 1;
pub const AUDIO_CONTROL_INTERFACE: u8 = 2;
pub const AUDIO_PLAYBACK_INTERFACE: u8 = 3;
pub const AUDIO_CAPTURE_INTERFACE: u8 = 4;
pub const HID_IN_ADDRESS: u8 = 0x81;
pub const HID_OUT_ADDRESS: u8 = 0x01;
pub const VENDOR_OUT_ADDRESS: u8 = 0x02;
pub const VENDOR_IN_ADDRESS: u8 = 0x82;
pub const AUDIO_PLAYBACK_ADDRESS: u8 = 0x03;
pub const AUDIO_CAPTURE_ADDRESS: u8 = 0x84;

fn push_u16(out: &mut Vec<u8>, value: u16) { out.extend_from_slice(&value.to_le_bytes()); }

fn interface(out: &mut Vec<u8>, number: u8, alternate: u8, endpoints: u8, class: u8, subclass: u8, protocol: u8) {
    out.extend_from_slice(&[9, 4, number, alternate, endpoints, class, subclass, protocol, 0]);
}

fn iad(out: &mut Vec<u8>, first: u8, count: u8, class: u8, subclass: u8, protocol: u8) {
    out.extend_from_slice(&[8, 0x0b, first, count, class, subclass, protocol, 0]);
}

fn endpoint(out: &mut Vec<u8>, address: u8, attributes: u8, max_packet: u16, interval: u8) {
    out.extend_from_slice(&[7, 5, address, attributes]);
    push_u16(out, max_packet);
    out.push(interval);
}

fn audio_input_terminal(out: &mut Vec<u8>, id: u8, terminal_type: u16, channels: u8, channel_config: u16) {
    out.extend_from_slice(&[12, 0x24, 0x02, id]);
    push_u16(out, terminal_type);
    out.extend_from_slice(&[0, channels]);
    push_u16(out, channel_config);
    out.extend_from_slice(&[0, 0]);
}

fn audio_output_terminal(out: &mut Vec<u8>, id: u8, terminal_type: u16, source: u8) {
    out.extend_from_slice(&[9, 0x24, 0x03, id]);
    push_u16(out, terminal_type);
    out.extend_from_slice(&[0, source, 0]);
}

fn audio_streaming_interface(out: &mut Vec<u8>, number: u8, terminal: u8, endpoint_address: u8) {
    interface(out, number, 0, 0, 0x01, 0x02, 0);
    interface(out, number, 1, 1, 0x01, 0x02, 0);
    out.extend_from_slice(&[7,0x24,0x01,terminal,0,0x01,0x00]);
    out.extend_from_slice(&[11,0x24,0x02,0x01,0x02,0x02,0x10,0x01,0x80,0xbb,0x00]);
    endpoint(out, endpoint_address, 0x0d, S2_AUDIO_USB_FRAME_BYTES as u16, 1);
    out.extend_from_slice(&[7,0x25,0x01,0,0,0,0]);
}

#[must_use]
pub fn device_descriptor() -> [u8; 18] {
    [
        18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, 64,
        0x7e, 0x05, 0x69, 0x20, 0x00, 0x02, 1, 2, 3, 1,
    ]
}

#[must_use]
pub fn hid_descriptor() -> [u8; 9] {
    let length = S2_PRO_REPORT_DESC.len() as u16;
    [9,0x21,0x11,0x01,0,1,0x22,length as u8,(length >> 8) as u8]
}

#[must_use]
pub fn config_descriptor() -> Vec<u8> {
    let mut body = Vec::new();
    iad(&mut body, HID_INTERFACE, 1, 0x03, 0, 0);
    interface(&mut body, HID_INTERFACE, 0, 2, 0x03, 0, 0);
    body.extend_from_slice(&hid_descriptor());
    endpoint(&mut body, HID_IN_ADDRESS, 0x03, 64, 4);
    endpoint(&mut body, HID_OUT_ADDRESS, 0x03, 64, 4);

    iad(&mut body, VENDOR_INTERFACE, 1, 0xff, 0, 0);
    interface(&mut body, VENDOR_INTERFACE, 0, 2, 0xff, 0, 0);
    endpoint(&mut body, VENDOR_OUT_ADDRESS, 0x02, 64, 0);
    endpoint(&mut body, VENDOR_IN_ADDRESS, 0x02, 64, 0);

    iad(&mut body, AUDIO_CONTROL_INTERFACE, 3, 0x01, 0x01, 0);
    interface(&mut body, AUDIO_CONTROL_INTERFACE, 0, 0, 0x01, 0x01, 0);
    let ac_total = 10 + 12 * 2 + 10 + 9 + 9 * 2;
    body.extend_from_slice(&[10,0x24,0x01,0x00,0x01]);
    push_u16(&mut body, ac_total);
    body.extend_from_slice(&[2,AUDIO_PLAYBACK_INTERFACE,AUDIO_CAPTURE_INTERFACE]);
    audio_input_terminal(&mut body, 1, 0x0101, 2, 0x0003);
    body.extend_from_slice(&[10,0x24,0x06,2,1,1,0x03,0,0,0]);
    audio_output_terminal(&mut body, 3, 0x0302, 2);
    audio_input_terminal(&mut body, 4, 0x0201, 1, 0);
    body.extend_from_slice(&[9,0x24,0x06,5,4,1,0x03,0,0]);
    audio_output_terminal(&mut body, 6, 0x0101, 5);
    audio_streaming_interface(&mut body, AUDIO_PLAYBACK_INTERFACE, 1, AUDIO_PLAYBACK_ADDRESS);
    audio_streaming_interface(&mut body, AUDIO_CAPTURE_INTERFACE, 6, AUDIO_CAPTURE_ADDRESS);

    let total = 9 + body.len();
    let mut out = Vec::with_capacity(total);
    out.extend_from_slice(&[9,2,total as u8,(total >> 8) as u8,5,1,0,0xc0,250]);
    out.extend_from_slice(&body);
    out
}

#[must_use]
pub fn string_descriptor(index: u8, serial: &str) -> Option<Vec<u8>> {
    if index == 0 { return Some(vec![4,3,0x09,0x04]); }
    let text = match index {
        1 => "Nintendo",
        2 => "Switch 2 Pro Controller",
        3 => serial,
        _ => return None,
    };
    let text = &text[..text.len().min(126)];
    let mut out = Vec::with_capacity(2 + text.len() * 2);
    out.extend_from_slice(&[(2 + text.len() * 2) as u8, 3]);
    for byte in text.bytes() { out.extend_from_slice(&[byte, 0]); }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_device_identity_is_nintendo_switch2_pro() {
        let device = device_descriptor();
        assert_eq!(&device[8..12], &[0x7e,0x05,0x69,0x20]);
        assert_eq!(&device[4..7], &[0xef,0x02,0x01]);
    }

    #[test]
    fn config_exposes_hid_vendor_and_bidirectional_uac1() {
        let config = config_descriptor();
        assert_eq!(usize::from(u16::from_le_bytes([config[2],config[3]])), config.len());
        assert_eq!(config[4], 5);
        for endpoint_address in [HID_IN_ADDRESS,HID_OUT_ADDRESS,VENDOR_OUT_ADDRESS,VENDOR_IN_ADDRESS,AUDIO_PLAYBACK_ADDRESS,AUDIO_CAPTURE_ADDRESS] {
            assert!(config.windows(3).any(|window| window == [7,5,endpoint_address]));
        }
        assert!(config.windows(3).any(|window| window == [9,4,AUDIO_PLAYBACK_INTERFACE]));
        assert!(config.windows(3).any(|window| window == [9,4,AUDIO_CAPTURE_INTERFACE]));
    }

    #[test]
    fn report_descriptor_length_matches_hid_descriptor() {
        let hid = hid_descriptor();
        assert_eq!(u16::from_le_bytes([hid[7],hid[8]]) as usize, S2_PRO_REPORT_DESC.len());
    }
}
