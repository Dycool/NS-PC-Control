#include "s2_motion_carrier.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int32_t read_i32le(const uint8_t* p) {
    const uint32_t value =
        static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<int32_t>(value);
}

std::array<uint32_t, 3> read_orientation(const uint8_t* pdu) {
    return {
        static_cast<uint32_t>(pdu[5])
            | (static_cast<uint32_t>(pdu[6]) << 8)
            | (static_cast<uint32_t>(pdu[7]) << 16)
            | (static_cast<uint32_t>(pdu[8] & 0x03u) << 24),
        static_cast<uint32_t>(pdu[9])
            | (static_cast<uint32_t>(pdu[10]) << 8)
            | (static_cast<uint32_t>(pdu[11]) << 16)
            | (static_cast<uint32_t>(pdu[12] & 0x03u) << 24),
        static_cast<uint32_t>(pdu[13])
            | (static_cast<uint32_t>(pdu[14]) << 8)
            | (static_cast<uint32_t>(pdu[15]) << 16)
            | (static_cast<uint32_t>(pdu[4] & 0x03u) << 24),
    };
}

} // namespace

int main() {
    ns::s2::MotionCarrierState state;
    ns::s2::reset_motion_carrier(state);

    ns::s2::MotionCarrierSample samples[3]{};
    for (auto& sample : samples) sample.az = 4096;

    check(ns::s2::update_motion_carrier(state, samples, 1'000),
          "first sample builds a carrier");
    check(state.carrier_valid, "carrier is marked valid");
    check(state.carrier[0] == 0 && state.carrier[1] == 0,
          "first carrier starts at timing zero");
    check(state.carrier[2] == 0x00 && state.carrier[3] == 0x0C,
          "nominal temperature is 0x0C00");

    const auto identity = read_orientation(state.carrier.data());
    check(identity[0] == 0x02000000u, "identity G0 is centered");
    check(identity[1] == 0x01000000u, "identity G1 is centered");
    check(identity[2] == 0x00800000u, "identity G2 is centered in state 0");
    check(read_i32le(state.carrier.data() + 16) == 0, "rest accel X is zero");
    check(read_i32le(state.carrier.data() + 20) == 0, "rest accel Y is zero");
    check(read_i32le(state.carrier.data() + 24) == 282'472'448,
          "one g uses the genuine carrier scale");
    check(state.carrier[28] == 0x00 && state.carrier[29] == 0x02,
          "genuine constant tail is present");

    const auto held = state.carrier;
    check(!ns::s2::update_motion_carrier(state, samples, 2'000),
          "sub-cadence duplicate is ignored");
    check(state.carrier == held, "ignored duplicate does not retime the carrier");

    uint64_t now_us = 5'000;
    for (unsigned i = 0; i < 40; ++i) {
        ns::s2::update_motion_carrier(state, samples, now_us);
        now_us += 4'000;
    }
    check(state.bias_ready, "stationary warmup acquires zero-rate bias");

    const auto before_motion = state.carrier;
    for (auto& sample : samples) sample.gx = 328; // about +20 deg/s
    for (unsigned i = 0; i < 125; ++i) {
        ns::s2::update_motion_carrier(state, samples, now_us);
        now_us += 4'000;
    }
    check(state.carrier != before_motion, "fresh gyro changes the quaternion carrier");
    check(std::fabs(state.quaternion[1]) > 0.05f,
          "Switch-1 X gyro maps onto the Pro2 Y carrier axis");

    ns::s2::reset_motion_carrier(state);
    for (auto& sample : samples) {
        sample = {};
        sample.ay = -4096;
    }
    check(ns::s2::update_motion_carrier(state, samples, now_us),
          "mouse posture builds a carrier");
    check(read_i32le(state.carrier.data() + 16) == 282'472'448,
          "negative S1 Y maps to positive Pro2 X gravity");

    if (failures != 0) return 1;
    std::puts("s2_motion_carrier: all tests passed");
    return 0;
}
