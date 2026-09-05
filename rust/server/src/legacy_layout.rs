use crate::app_state::{ClientAssignmentState, ClientSnapshot, UsbControllerFamily, MAX_CLIENTS};
use crate::controller_profiles::{profile_shape, requested_profile_from_report, ProfileShape};
use ns_shared::protocol::{ControllerType, HoriHidReport};

pub const LEGACY_PORTS: usize = 4;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct LegacySlot {
    client_index: Option<usize>,
    subpad: usize,
    virtual_type: ControllerType,
    pair_member: bool,
    pair_right: bool,
}

impl LegacySlot {
    #[must_use]
    pub const fn client_index(self) -> Option<usize> {
        self.client_index
    }

    #[must_use]
    pub const fn subpad(self) -> usize {
        self.subpad
    }

    #[must_use]
    pub const fn virtual_type(self) -> ControllerType {
        self.virtual_type
    }

    #[must_use]
    pub const fn pair_member(self) -> bool {
        self.pair_member
    }

    #[must_use]
    pub const fn pair_right(self) -> bool {
        self.pair_right
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct SourceRequest {
    client_index: usize,
    subpad: usize,
    profile: ControllerType,
}

#[derive(Clone, Debug)]
pub struct LegacyLayout {
    slots: [LegacySlot; LEGACY_PORTS],
}

impl Default for LegacyLayout {
    fn default() -> Self {
        Self {
            slots: [LegacySlot::default(); LEGACY_PORTS],
        }
    }
}

impl LegacyLayout {
    #[must_use]
    pub const fn slots(&self) -> &[LegacySlot; LEGACY_PORTS] {
        &self.slots
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn reconcile(
        &mut self,
        snapshots: &[Option<ClientSnapshot>; MAX_CLIENTS],
        family: UsbControllerFamily,
    ) -> [LegacySlot; LEGACY_PORTS] {
        let port_count = if family == UsbControllerFamily::Switch2 { 1 } else { LEGACY_PORTS };
        let mut ordered = Vec::new();
        let mut pairs = Vec::new();
        let mut singles = Vec::new();

        for (client_index, snapshot) in snapshots.iter().enumerate() {
            let Some(snapshot) = snapshot.filter(|snapshot| snapshot.active()) else {
                continue;
            };
            let pad_present = snapshot.pad_present();
            let presence_seen = pad_present.iter().any(|present| *present)
                || snapshot.pad_last_present_us().iter().any(|stamp| *stamp != 0);
            let mut any_request = false;

            for (subpad, (report, present)) in snapshot
                .report()
                .pads()
                .iter()
                .zip(pad_present)
                .enumerate()
            {
                let active = if presence_seen {
                    present
                } else {
                    *report.input() != HoriHidReport::default()
                };
                if !active {
                    continue;
                }
                let profile = requested_profile_from_report(report, family);
                let request = SourceRequest {
                    client_index,
                    subpad,
                    profile,
                };
                ordered.push(request);
                if profile_shape(profile) == ProfileShape::Pair {
                    pairs.push(request);
                } else {
                    singles.push(request);
                }
                any_request = true;
            }

            if !any_request {
                // Exact C++ idle behavior: reserve P1's configured identity so
                // enumeration sees the intended controller before live input.
                let profile = requested_profile_from_report(&snapshot.report().pads()[0], family);
                let request = SourceRequest {
                    client_index,
                    subpad: 0,
                    profile,
                };
                ordered.push(request);
                if profile_shape(profile) == ProfileShape::Pair {
                    pairs.push(request);
                } else {
                    singles.push(request);
                }
            }
        }

        let previous = self.slots;
        let mut next = [LegacySlot::default(); LEGACY_PORTS];
        for slot in next.iter_mut().take(port_count) {
            slot.virtual_type = idle_virtual_type(family);
        }

        if family == UsbControllerFamily::Switch2 {
            if let Some(request) = ordered.first().copied()
                && profile_shape(request.profile) != ProfileShape::Pair
            {
                next[0] = single_slot(request, family);
            }
        } else {
            // C++ allocates pairs first and only in contiguous [0,1]/[2,3]
            // groups, preserving the previous pair base whenever possible.
            for request in pairs {
                let mut base = existing_pair_base(&previous, &next, request, port_count);
                if base.is_none() {
                    let preferred = request.subpad.saturating_mul(2);
                    if pair_base_free(&next, preferred, port_count) {
                        base = Some(preferred);
                    }
                }
                if base.is_none() {
                    base = (0..port_count.saturating_sub(1))
                        .step_by(2)
                        .find(|candidate| pair_base_free(&next, *candidate, port_count));
                }
                if let Some(base) = base {
                    next[base] = pair_slot(request, family, false);
                    next[base + 1] = pair_slot(request, family, true);
                }
            }

            for request in singles {
                let mut port = existing_single_port(&previous, &next, request, port_count);
                if port.is_none() && request.subpad < port_count && slot_free(&next, request.subpad) {
                    port = Some(request.subpad);
                }
                if port.is_none() {
                    port = (0..port_count).find(|candidate| slot_free(&next, *candidate));
                }
                if let Some(port) = port {
                    next[port] = single_slot(request, family);
                }
            }
        }

        self.slots = next;
        next
    }

    #[must_use]
    pub fn assignments(
        &self,
        snapshots: &[Option<ClientSnapshot>; MAX_CLIENTS],
        family: UsbControllerFamily,
    ) -> [[ClientAssignmentState; 4]; MAX_CLIENTS] {
        let mut assignments = [[ClientAssignmentState::default(); 4]; MAX_CLIENTS];
        for (client_index, snapshot) in snapshots.iter().enumerate() {
            let Some(snapshot) = snapshot.filter(|snapshot| snapshot.active()) else {
                continue;
            };
            for (subpad, assignment) in assignments[client_index].iter_mut().enumerate() {
                *assignment = ClientAssignmentState::new(
                    0,
                    0xff,
                    requested_profile_from_report(&snapshot.report().pads()[subpad], family),
                    ControllerType::Default,
                );
            }
        }

        for (port, slot) in self.slots.iter().copied().enumerate() {
            let Some(client_index) = slot.client_index else {
                continue;
            };
            let assignment = &mut assignments[client_index][slot.subpad];
            let mask = assignment.console_port_mask() | (1_u8 << port);
            let primary = if assignment.primary_console_port() == 0xff {
                u8::try_from(port).unwrap_or(0xff)
            } else {
                assignment.primary_console_port()
            };
            let virtual_type = if slot.pair_member {
                if family == UsbControllerFamily::Switch2 {
                    ControllerType::JoyconPairS2
                } else {
                    ControllerType::JoyconPair
                }
            } else {
                slot.virtual_type
            };
            *assignment = ClientAssignmentState::new(
                mask,
                primary,
                assignment.requested_type(),
                virtual_type,
            );
        }
        assignments
    }
}

fn idle_virtual_type(family: UsbControllerFamily) -> ControllerType {
    if family == UsbControllerFamily::Hori {
        ControllerType::Hori
    } else if family == UsbControllerFamily::Switch2 {
        ControllerType::ProS2
    } else {
        ControllerType::Pro
    }
}

fn single_slot(request: SourceRequest, family: UsbControllerFamily) -> LegacySlot {
    LegacySlot {
        client_index: Some(request.client_index),
        subpad: request.subpad,
        virtual_type: requested_profile_from_shape(request.profile, family),
        pair_member: false,
        pair_right: false,
    }
}

fn pair_slot(request: SourceRequest, family: UsbControllerFamily, right: bool) -> LegacySlot {
    let virtual_type = match (family, right) {
        (UsbControllerFamily::Switch2, false) => ControllerType::JoyconLS2,
        (UsbControllerFamily::Switch2, true) => ControllerType::JoyconRS2,
        (_, false) => ControllerType::JoyconL,
        (_, true) => ControllerType::JoyconR,
    };
    LegacySlot {
        client_index: Some(request.client_index),
        subpad: request.subpad,
        virtual_type,
        pair_member: true,
        pair_right: right,
    }
}

fn requested_profile_from_shape(profile: ControllerType, family: UsbControllerFamily) -> ControllerType {
    match (family, profile_shape(profile)) {
        (UsbControllerFamily::Hori, _) => ControllerType::Hori,
        (UsbControllerFamily::Switch2, ProfileShape::JoyconL) => ControllerType::JoyconLS2,
        (UsbControllerFamily::Switch2, ProfileShape::JoyconR) => ControllerType::JoyconRS2,
        (UsbControllerFamily::Switch2, _) => ControllerType::ProS2,
        (UsbControllerFamily::Switch1, ProfileShape::JoyconL) => ControllerType::JoyconL,
        (UsbControllerFamily::Switch1, ProfileShape::JoyconR) => ControllerType::JoyconR,
        (UsbControllerFamily::Switch1, _) => ControllerType::Pro,
    }
}

fn slot_free(slots: &[LegacySlot; LEGACY_PORTS], port: usize) -> bool {
    slots.get(port).is_some_and(|slot| slot.client_index.is_none())
}

fn pair_base_free(slots: &[LegacySlot; LEGACY_PORTS], base: usize, port_count: usize) -> bool {
    base + 1 < port_count && slot_free(slots, base) && slot_free(slots, base + 1)
}

fn existing_pair_base(
    previous: &[LegacySlot; LEGACY_PORTS],
    next: &[LegacySlot; LEGACY_PORTS],
    request: SourceRequest,
    port_count: usize,
) -> Option<usize> {
    (0..port_count.saturating_sub(1)).step_by(2).find(|base| {
        let left = previous[*base];
        let right = previous[*base + 1];
        left.client_index == Some(request.client_index)
            && left.subpad == request.subpad
            && left.pair_member
            && !left.pair_right
            && right.client_index == Some(request.client_index)
            && right.subpad == request.subpad
            && right.pair_member
            && right.pair_right
            && pair_base_free(next, *base, port_count)
    })
}

fn existing_single_port(
    previous: &[LegacySlot; LEGACY_PORTS],
    next: &[LegacySlot; LEGACY_PORTS],
    request: SourceRequest,
    port_count: usize,
) -> Option<usize> {
    (0..port_count).find(|port| {
        let old = previous[*port];
        old.client_index == Some(request.client_index)
            && old.subpad == request.subpad
            && !old.pair_member
            && slot_free(next, *port)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app_state::ServerContext;
    use ns_shared::protocol::{HidReport, MotionReport, MultiReport};
    use std::net::{IpAddr, Ipv4Addr, SocketAddr};

    fn snapshots(
        profiles: &[(usize, ControllerType)],
        now_us: u64,
    ) -> ([Option<ClientSnapshot>; MAX_CLIENTS], ServerContext) {
        let context = ServerContext::default();
        let client = context
            .register_udp(
                SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 45_000),
                now_us,
            )
            .expect("client");
        let mut pads = [HidReport::default(); 4];
        for (subpad, profile) in profiles.iter().copied() {
            let mut input = HoriHidReport::default();
            input.set_pad_present(true);
            pads[subpad] = HidReport::new(
                input,
                [MotionReport::default(); 3],
                false,
                [0, 0, profile as u8],
            );
        }
        context
            .update_udp_report(client, 1, MultiReport::new(pads), now_us)
            .expect("report");
        let snapshots = std::array::from_fn(|index| context.snapshot(index, now_us));
        (snapshots, context)
    }

    #[test]
    fn pair_consumes_contiguous_group_before_singles() {
        let now = 1_000_000;
        let (snapshots, _context) = snapshots(
            &[(0, ControllerType::Pro), (1, ControllerType::JoyconPair)],
            now,
        );
        let mut layout = LegacyLayout::default();
        let slots = layout.reconcile(&snapshots, UsbControllerFamily::Switch1);
        assert_eq!(slots[0].client_index(), Some(0));
        assert_eq!(slots[0].subpad(), 1);
        assert!(slots[0].pair_member());
        assert!(!slots[0].pair_right());
        assert_eq!(slots[1].subpad(), 1);
        assert!(slots[1].pair_right());
        assert_eq!(slots[2].subpad(), 0);
    }

    #[test]
    fn existing_single_port_is_preserved_when_another_pad_appears() {
        let now = 2_000_000;
        let (first, context) = snapshots(&[(2, ControllerType::Pro)], now);
        let mut layout = LegacyLayout::default();
        let slots = layout.reconcile(&first, UsbControllerFamily::Switch1);
        assert_eq!(slots[2].subpad(), 2);

        let mut pads = *first[0].expect("snapshot").report();
        let mut input = HoriHidReport::default();
        input.set_pad_present(true);
        *pads.pad_mut(0).expect("pad 0") = HidReport::new(
            input,
            [MotionReport::default(); 3],
            false,
            [0, 0, ControllerType::Pro as u8],
        );
        context.update_udp_report(0, 2, pads, now + 1).expect("update");
        let second = std::array::from_fn(|index| context.snapshot(index, now + 1));
        let slots = layout.reconcile(&second, UsbControllerFamily::Switch1);
        assert_eq!(slots[2].subpad(), 2);
        assert_eq!(slots[0].subpad(), 0);
    }

    #[test]
    fn assignment_for_pair_reports_two_port_mask_and_pair_type() {
        let now = 3_000_000;
        let (snapshots, _context) = snapshots(&[(0, ControllerType::JoyconPair)], now);
        let mut layout = LegacyLayout::default();
        layout.reconcile(&snapshots, UsbControllerFamily::Switch1);
        let assignments = layout.assignments(&snapshots, UsbControllerFamily::Switch1);
        assert_eq!(assignments[0][0].console_port_mask(), 0b0011);
        assert_eq!(assignments[0][0].primary_console_port(), 0);
        assert_eq!(assignments[0][0].requested_type(), ControllerType::JoyconPair);
        assert_eq!(assignments[0][0].virtual_type(), ControllerType::JoyconPair);
    }

    #[test]
    fn idle_client_reserves_p1_identity() {
        let context = ServerContext::default();
        let now = 4_000_000;
        let client = context
            .register_udp(
                SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 45_001),
                now,
            )
            .expect("client");
        let report = HidReport::new(
            HoriHidReport::default(),
            [MotionReport::default(); 3],
            false,
            [0, 0, ControllerType::JoyconR as u8],
        );
        context
            .update_udp_report(
                client,
                1,
                MultiReport::new([report, HidReport::default(), HidReport::default(), HidReport::default()]),
                now,
            )
            .expect("report");
        let snapshots = std::array::from_fn(|index| context.snapshot(index, now));
        let mut layout = LegacyLayout::default();
        let slots = layout.reconcile(&snapshots, UsbControllerFamily::Switch1);
        assert_eq!(slots[0].virtual_type(), ControllerType::JoyconR);
    }
}
