#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ns::s2 {

inline constexpr std::size_t MOTION_CARRIER_SIZE = 30;

struct MotionCarrierSample {
    int16_t ax = 0, ay = 0, az = 0;
    int16_t gx = 0, gy = 0, gz = 0;
};

// Translates the project's normalized Switch-1-style IMU samples into the
// hardware-validated Switch 2 length-0x1E quaternion carrier.
//
// One state belongs to one emulated USB controller. The writer thread is its
// sole owner, so no internal synchronization is required.
struct MotionCarrierState {
    std::array<float, 4> quaternion{0.0f, 0.0f, 0.0f, 1.0f}; // x/y/z/w
    std::array<int32_t, 3> gyro_lp{};  // fixed point, six fractional bits
    std::array<int32_t, 3> gyro_bias{};
    std::array<int32_t, 3> gyro_prev{};
    std::array<int32_t, 3> gyro_jitter{};
    std::array<int16_t, 3> accel{};
    std::array<uint8_t, MOTION_CARRIER_SIZE> carrier{};
    uint64_t last_update_us = 0;
    uint16_t imu_tick = 0;
    uint16_t timing = 0;
    uint16_t bias_warmup_samples = 0;
    uint8_t omitted_component = 0; // Switch 2 wire order: w/x/y/z
    bool initialized = false;
    bool bias_ready = false;
    bool carrier_valid = false;
};

void reset_motion_carrier(MotionCarrierState& state);

// Consumes one fresh group of three samples (oldest -> newest). Returns true
// only when a new carrier was produced. Calls closer than the translator's
// ~250 Hz cadence are intentionally ignored.
bool update_motion_carrier(MotionCarrierState& state,
                           const MotionCarrierSample samples[3],
                           uint64_t now_us);

} // namespace ns::s2
