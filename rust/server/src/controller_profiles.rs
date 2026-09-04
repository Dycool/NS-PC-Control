use crate::app_state::UsbControllerFamily;
use ns_shared::protocol::{ControllerType, HidReport};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProfileShape {
    Pro,
    JoyconL,
    JoyconR,
    Pair,
}

#[must_use]
pub const fn profile_shape(profile: ControllerType) -> ProfileShape {
    match profile {
        ControllerType::JoyconL | ControllerType::JoyconLS2 => ProfileShape::JoyconL,
        ControllerType::JoyconR | ControllerType::JoyconRS2 => ProfileShape::JoyconR,
        ControllerType::JoyconPair | ControllerType::JoyconPairS2 => ProfileShape::Pair,
        ControllerType::Default | ControllerType::Pro | ControllerType::ProS2 | ControllerType::Hori => {
            ProfileShape::Pro
        }
    }
}

#[must_use]
pub const fn coerce_profile_to_family(
    profile: ControllerType,
    family: UsbControllerFamily,
) -> ControllerType {
    match (family, profile_shape(profile)) {
        (UsbControllerFamily::Switch2, ProfileShape::JoyconL) => ControllerType::JoyconLS2,
        (UsbControllerFamily::Switch2, ProfileShape::JoyconR) => ControllerType::JoyconRS2,
        (UsbControllerFamily::Switch2, ProfileShape::Pair) => ControllerType::JoyconPairS2,
        (UsbControllerFamily::Switch2, ProfileShape::Pro) => ControllerType::ProS2,
        (UsbControllerFamily::Hori, _) => ControllerType::Hori,
        (UsbControllerFamily::Switch1, ProfileShape::JoyconL) => ControllerType::JoyconL,
        (UsbControllerFamily::Switch1, ProfileShape::JoyconR) => ControllerType::JoyconR,
        (UsbControllerFamily::Switch1, ProfileShape::Pair) => ControllerType::JoyconPair,
        (UsbControllerFamily::Switch1, ProfileShape::Pro) => ControllerType::Pro,
    }
}

#[must_use]
pub fn raw_profile_from_report(report: &HidReport) -> ControllerType {
    match report.controller_type().unwrap_or(ControllerType::Pro) {
        ControllerType::Default => ControllerType::Pro,
        profile => profile,
    }
}

#[must_use]
pub fn requested_profile_from_report(
    report: &HidReport,
    family: UsbControllerFamily,
) -> ControllerType {
    coerce_profile_to_family(raw_profile_from_report(report), family)
}

#[must_use]
pub fn report_requests_pair(report: &HidReport) -> bool {
    matches!(
        raw_profile_from_report(report),
        ControllerType::JoyconPair | ControllerType::JoyconPairS2
    )
}

#[must_use]
pub const fn switch2_pid_low(profile: ControllerType) -> u8 {
    match profile_shape(profile) {
        ProfileShape::JoyconL => 0x67,
        ProfileShape::JoyconR => 0x66,
        ProfileShape::Pro | ProfileShape::Pair => 0x69,
    }
}

#[must_use]
pub const fn switch2_report_id(profile: ControllerType) -> u8 {
    match profile_shape(profile) {
        ProfileShape::JoyconL => 0x07,
        ProfileShape::JoyconR => 0x08,
        ProfileShape::Pro | ProfileShape::Pair => 0x09,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ns_shared::protocol::{HoriHidReport, MotionReport};

    fn report(profile: ControllerType) -> HidReport {
        HidReport::new(
            HoriHidReport::default(),
            [MotionReport::default(); 3],
            false,
            [0, 0, profile as u8],
        )
    }

    #[test]
    fn family_coercion_matches_cpp_shape_rules() {
        let cases = [
            (ControllerType::Default, ControllerType::ProS2),
            (ControllerType::Pro, ControllerType::ProS2),
            (ControllerType::Hori, ControllerType::ProS2),
            (ControllerType::JoyconL, ControllerType::JoyconLS2),
            (ControllerType::JoyconRS2, ControllerType::JoyconRS2),
            (ControllerType::JoyconPair, ControllerType::JoyconPairS2),
        ];
        for (input, expected) in cases {
            assert_eq!(coerce_profile_to_family(input, UsbControllerFamily::Switch2), expected);
        }
        assert_eq!(
            coerce_profile_to_family(ControllerType::JoyconLS2, UsbControllerFamily::Switch1),
            ControllerType::JoyconL
        );
        assert_eq!(
            coerce_profile_to_family(ControllerType::JoyconPairS2, UsbControllerFamily::Switch1),
            ControllerType::JoyconPair
        );
        assert_eq!(
            coerce_profile_to_family(ControllerType::JoyconR, UsbControllerFamily::Hori),
            ControllerType::Hori
        );
    }

    #[test]
    fn default_wire_profile_is_pro_like_exactly_as_cpp() {
        assert_eq!(raw_profile_from_report(&report(ControllerType::Default)), ControllerType::Pro);
        assert_eq!(
            requested_profile_from_report(&report(ControllerType::Default), UsbControllerFamily::Switch2),
            ControllerType::ProS2
        );
    }

    #[test]
    fn switch2_identity_mapping_matches_factory_pid_and_reports() {
        assert_eq!(switch2_pid_low(ControllerType::ProS2), 0x69);
        assert_eq!(switch2_pid_low(ControllerType::JoyconLS2), 0x67);
        assert_eq!(switch2_pid_low(ControllerType::JoyconRS2), 0x66);
        assert_eq!(switch2_report_id(ControllerType::ProS2), 0x09);
        assert_eq!(switch2_report_id(ControllerType::JoyconLS2), 0x07);
        assert_eq!(switch2_report_id(ControllerType::JoyconRS2), 0x08);
    }
}
