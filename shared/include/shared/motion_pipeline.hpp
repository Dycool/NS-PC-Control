#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <deque>

namespace ns {

// Timestamped, controller-agnostic motion conditioning. Input values are
// already expressed in the common Nintendo wire units (4096 counts/g and
// 16.384 counts/degree/second).
//
// The pipeline deliberately emits gyro only once per new physical SDL sensor
// sample. The transport/report loop may run faster than the controller sensor;
// repeating the held angular velocity would make the console integrate the
// same sample more than once and causes overshoot/twitches.
class MotionPipeline {
public:
    static constexpr uint64_t OUTPUT_SAMPLE_INTERVAL_US = 5'000;

    void reset();
    void configure(bool has_accel, float accel_rate_hz,
                   bool has_gyro, float gyro_rate_hz);
    void push_accel(uint64_t timestamp_us, float x, float y, float z);
    void push_gyro(uint64_t timestamp_us, float x, float y, float z);

    // Produces three evenly timed samples, oldest to newest. now_us must use
    // the same monotonic clock as push_* timestamps. When a gyro is available,
    // true is returned only once for each newly received gyro sample. A faster
    // network/USB polling loop therefore receives a held/no-motion frame rather
    // than integrating the same physical sample repeatedly.
    bool sample(uint64_t now_us, MotionReport out[3]);

    uint64_t last_accel_us() const noexcept { return accel_.last_input_us; }
    uint64_t last_gyro_us() const noexcept { return gyro_.last_input_us; }
    bool gyro_bias_ready() const noexcept { return gyro_bias_ready_; }
    float gyro_bias(int axis) const noexcept {
        return (axis >= 0 && axis < 3) ? gyro_bias_[axis] : 0.0f;
    }

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

    float gyro_bias_[3]{};
    double gyro_bias_sum_[3]{};
    uint32_t gyro_bias_sample_count_ = 0;
    uint64_t gyro_stationary_since_us_ = 0;
    uint64_t gyro_bias_last_update_us_ = 0;
    bool gyro_bias_ready_ = false;
    uint64_t last_emitted_gyro_input_us_ = 0;
    uint64_t last_emitted_accel_input_us_ = 0;

    static void push(Stream& stream, uint64_t timestamp_us,
                     float x, float y, float z, float cutoff_hz);
    static bool interpolate(const Stream& stream, uint64_t timestamp_us,
                            uint64_t stale_after_us, float out[3]);
    static uint64_t stale_timeout_us(const Stream& stream);
    bool accel_indicates_stationary(uint64_t timestamp_us) const;
    void update_gyro_bias(uint64_t timestamp_us, const float raw[3]);
};

} // namespace ns
