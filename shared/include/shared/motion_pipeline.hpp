#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <deque>

namespace ns {

// Timestamped, controller-agnostic motion conditioning. Input values are
// already expressed in the common Nintendo wire units (4096 counts/g and
// 16.384 counts/degree/second). No calibration or learned bias is applied.
class MotionPipeline {
public:
    static constexpr uint64_t OUTPUT_SAMPLE_INTERVAL_US = 5'000;

    void reset();
    void configure(bool has_accel, float accel_rate_hz,
                   bool has_gyro, float gyro_rate_hz);
    void push_accel(uint64_t timestamp_us, float x, float y, float z);
    void push_gyro(uint64_t timestamp_us, float x, float y, float z);

    // Produces three evenly timed samples, oldest to newest. now_us must use
    // the same monotonic clock as push_* timestamps.
    bool sample(uint64_t now_us, MotionReport out[3]);

    uint64_t last_accel_us() const noexcept { return accel_.last_input_us; }
    uint64_t last_gyro_us() const noexcept { return gyro_.last_input_us; }

private:
    struct TimedVec3 {
        uint64_t timestamp_us = 0;
        float v[3]{};
    };

    struct Stream {
        std::deque<TimedVec3> history;
        float filtered[3]{};
        uint64_t last_input_us = 0;
        float rate_hz = 0.0f;
        bool available = false;
        bool filter_ready = false;
    };

    Stream accel_;
    Stream gyro_;

    static void push(Stream& stream, uint64_t timestamp_us,
                     float x, float y, float z, float cutoff_hz);
    static bool interpolate(const Stream& stream, uint64_t timestamp_us,
                            uint64_t stale_after_us, float out[3]);
    static uint64_t stale_timeout_us(const Stream& stream);
    uint64_t interpolation_delay_us() const;
};

} // namespace ns
