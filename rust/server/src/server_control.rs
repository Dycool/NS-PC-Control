use crate::app_state::{requested_virtual_slots, ServerContext, UsbControllerFamily, MAX_CLIENTS};
use ns_shared::crypto::hmac_verify;
use ns_shared::protocol::{
    GadgetFamily, GadgetModeReply, GadgetModeRequest, GadgetModeResult, ServerBackend,
    ServerInfoProbe, ServerInfoReply, GADGET_MODE_MAGIC, GADGET_MODE_REQUEST_AUTH_SIZE,
    GADGET_MODE_REQUEST_SIZE, GADGET_MODE_VERSION, PRO_UDP_HZ, PRO_UDP_INTERVAL_MS,
    SERVER_INFO_FLAG_HORI_MODE, SERVER_INFO_FLAG_S2_AUDIO, SERVER_INFO_FLAG_SERVER_FULL,
    SERVER_INFO_FLAG_SWITCH2_MODE, SERVER_INFO_PROBE_SIZE, SERVER_INFO_VERSION,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ControlDatagram {
    Reply {
        payload: Vec<u8>,
        restart: Option<UsbControllerFamily>,
    },
    Consumed,
}

#[must_use]
pub fn configured_client_capacity(context: &ServerContext) -> usize {
    if context.family() == UsbControllerFamily::Switch2 {
        1
    } else {
        MAX_CLIENTS
    }
}

#[must_use]
pub fn configured_virtual_port_count(context: &ServerContext) -> usize {
    if context.family() == UsbControllerFamily::Switch2 {
        1
    } else {
        4
    }
}

#[must_use]
pub fn free_virtual_slot_count(context: &ServerContext, now_us: u64) -> usize {
    let capacity = configured_virtual_port_count(context);
    if context.family() == UsbControllerFamily::Switch2 {
        return capacity.saturating_sub(usize::from(context.active_client_count(now_us) != 0));
    }

    let mut requested = 0usize;
    for client_index in 0..MAX_CLIENTS {
        let Some(snapshot) = context.snapshot(client_index, now_us) else {
            continue;
        };
        if !snapshot.active() {
            continue;
        }
        let mut any_present = false;
        for (subpad, report) in snapshot.report().pads().iter().enumerate() {
            let present = snapshot.pad_present()[subpad];
            any_present |= present;
            requested = requested.saturating_add(requested_virtual_slots(report, present));
        }
        if !any_present {
            requested = requested.saturating_add(requested_virtual_slots(
                &snapshot.report().pads()[0],
                true,
            ));
        }
    }
    capacity.saturating_sub(requested.min(capacity))
}

#[must_use]
pub fn inspect_control_datagram(
    context: &ServerContext,
    key: &[u8],
    bytes: &[u8],
    requester_slot: Option<usize>,
    now_us: u64,
) -> Option<ControlDatagram> {
    if bytes.len() == SERVER_INFO_PROBE_SIZE {
        if bytes.get(4).copied() != Some(SERVER_INFO_VERSION)
            || ServerInfoProbe::decode(bytes).is_err()
        {
            return None;
        }
        let active_clients = context.active_client_count(now_us);
        let max_clients = configured_client_capacity(context);
        let free_slots = free_virtual_slot_count(context, now_us);
        let mut flags = 0u8;
        match context.family() {
            UsbControllerFamily::Switch2 => {
                flags |= SERVER_INFO_FLAG_SWITCH2_MODE | SERVER_INFO_FLAG_S2_AUDIO;
            }
            UsbControllerFamily::Hori => flags |= SERVER_INFO_FLAG_HORI_MODE,
            UsbControllerFamily::Switch1 => {}
        }
        if free_slots == 0 || active_clients >= max_clients {
            flags |= SERVER_INFO_FLAG_SERVER_FULL;
        }

        let mut payload = ServerInfoReply::new(
            ServerBackend::Pro,
            PRO_UDP_INTERVAL_MS,
            PRO_UDP_HZ,
        )
        .encode();
        payload[10] = flags;
        payload[11] = u8::try_from(active_clients.min(max_clients)).unwrap_or(u8::MAX);
        payload[12] = u8::try_from(max_clients).unwrap_or(u8::MAX);
        payload[13] = u8::try_from(free_slots).unwrap_or(u8::MAX);
        return Some(ControlDatagram::Reply {
            payload: payload.to_vec(),
            restart: None,
        });
    }

    if bytes.len() != GADGET_MODE_REQUEST_SIZE
        || bytes.get(..4).and_then(|prefix| prefix.try_into().ok()).map(u32::from_le_bytes)
            != Some(GADGET_MODE_MAGIC)
    {
        return None;
    }
    if bytes.get(4).copied() != Some(GADGET_MODE_VERSION)
        || !hmac_verify(
            key,
            &bytes[..GADGET_MODE_REQUEST_AUTH_SIZE],
            &bytes[GADGET_MODE_REQUEST_AUTH_SIZE..],
        )
    {
        return Some(ControlDatagram::Consumed);
    }
    let Ok(request) = GadgetModeRequest::decode(bytes) else {
        return Some(ControlDatagram::Consumed);
    };
    let requested = family_from_wire(request.family());
    let active_clients = context.active_client_count(now_us);
    let requester_is_active = requester_slot
        .and_then(|slot| context.snapshot(slot, now_us))
        .is_some_and(|snapshot| snapshot.active());
    let other_clients = active_clients.saturating_sub(usize::from(requester_is_active));
    let (result, active_family, restart) = if other_clients != 0 {
        (GadgetModeResult::ServerFull, context.family(), None)
    } else if requested == context.family() {
        (GadgetModeResult::Unchanged, context.family(), None)
    } else {
        (GadgetModeResult::Restarting, requested, Some(requested))
    };
    let reply = GadgetModeReply::new(
        result,
        family_to_wire(active_family),
        u8::try_from(if result == GadgetModeResult::ServerFull {
            active_clients
        } else {
            0
        })
        .unwrap_or(u8::MAX),
    )
    .encode();
    Some(ControlDatagram::Reply {
        payload: reply.to_vec(),
        restart,
    })
}

#[must_use]
pub const fn family_from_wire(family: GadgetFamily) -> UsbControllerFamily {
    match family {
        GadgetFamily::Switch1 => UsbControllerFamily::Switch1,
        GadgetFamily::Switch2 => UsbControllerFamily::Switch2,
        GadgetFamily::Hori => UsbControllerFamily::Hori,
    }
}

#[must_use]
pub const fn family_to_wire(family: UsbControllerFamily) -> GadgetFamily {
    match family {
        UsbControllerFamily::Switch1 => GadgetFamily::Switch1,
        UsbControllerFamily::Switch2 => GadgetFamily::Switch2,
        UsbControllerFamily::Hori => GadgetFamily::Hori,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::crypto::{derive_key, hmac_sha256};
    use ns_shared::protocol::{DEFAULT_SECRET, MultiReport};
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    fn address(port: u16) -> SocketAddr {
        SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), port)
    }

    fn gadget_request(family: GadgetFamily, key: &[u8]) -> [u8; GADGET_MODE_REQUEST_SIZE] {
        let mut request = GadgetModeRequest::new(family, 7);
        let digest = hmac_sha256(key, &request.authenticated_bytes());
        request.set_hmac(digest[..16].try_into().expect("truncated HMAC"));
        request.encode()
    }

    #[test]
    fn server_info_reports_cpp_pro_cadence_and_family() {
        let context = ServerContext::default();
        let key = derive_key(DEFAULT_SECRET);
        let ControlDatagram::Reply { payload, restart } = inspect_control_datagram(
            &context,
            &key,
            &ServerInfoProbe::encode(),
            None,
            10,
        )
        .expect("handled") else {
            panic!("reply");
        };
        assert!(restart.is_none());
        let decoded = ServerInfoReply::decode(&payload).expect("server info");
        assert_eq!(decoded.backend(), ServerBackend::Pro);
        assert_eq!(decoded.cadence(), (PRO_UDP_INTERVAL_MS, PRO_UDP_HZ));
        assert_eq!(payload[10] & SERVER_INFO_FLAG_SWITCH2_MODE, 0);
        assert_eq!(payload[12], 4);
        assert_eq!(payload[13], 4);
    }

    #[test]
    fn family_request_exempts_its_own_live_session() {
        let context = ServerContext::default();
        let key = derive_key(DEFAULT_SECRET);
        let slot = context.register_udp(address(30_001), 100).expect("slot");
        context
            .update_udp_report(slot, 1, MultiReport::default(), 100)
            .expect("report");
        let request = gadget_request(GadgetFamily::Switch2, &key);
        let ControlDatagram::Reply { payload, restart } = inspect_control_datagram(
            &context,
            &key,
            &request,
            Some(slot),
            100,
        )
        .expect("handled") else {
            panic!("reply");
        };
        let reply = GadgetModeReply::decode(&payload).expect("mode reply");
        assert_eq!(reply.result(), GadgetModeResult::Restarting);
        assert_eq!(restart, Some(UsbControllerFamily::Switch2));
    }

    #[test]
    fn family_request_is_blocked_by_another_client() {
        let context = ServerContext::default();
        let key = derive_key(DEFAULT_SECRET);
        let requester = context.register_udp(address(30_001), 100).expect("requester");
        context
            .update_udp_report(requester, 1, MultiReport::default(), 100)
            .expect("requester report");
        let other = context.register_udp(address(30_002), 100).expect("other");
        context
            .update_udp_report(other, 1, MultiReport::default(), 100)
            .expect("other report");
        let request = gadget_request(GadgetFamily::Switch2, &key);
        let ControlDatagram::Reply { payload, restart } = inspect_control_datagram(
            &context,
            &key,
            &request,
            Some(requester),
            100,
        )
        .expect("handled") else {
            panic!("reply");
        };
        let reply = GadgetModeReply::decode(&payload).expect("mode reply");
        assert_eq!(reply.result(), GadgetModeResult::ServerFull);
        assert!(restart.is_none());
    }
}
