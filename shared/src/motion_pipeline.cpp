#include "shared/motion_pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace ns {
namespace {

constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float ACCEL_CUTOFF_HZ = 30.0f;
constexpr float GYRO_CUTOFF_HZ = 40.0f;
constexpr float GYRO_NOISE_FLOOR = 32.0f;
constexpr uint64_t HISTORY_US = 250'000;
constexpr uint64_t MIN_STALE_US = 50'000;
constexpr uint64_t MAX_STALE_US = 150'000;

int16_t motion_i16(float value) {
    return static_cast<int16_t>(std::clamp<long>(std::lround(value), -32768L, 32767L));
}

int16_t gyro_i16(float value) {
    if (std::abs(value) <= GYRO_NOISE_FLOOR) return 0;
    return motion_i16(value);
}

} // namespace

void MotionPipeline::reset() {
    accel_ = Stream{};
    gyro_ = Stream{};
}

void MotionPipeline::configure(bool has_accel, float accel_rate_hz,
                               bool has_gyro, float gyro_rate_hz) {
    accel_.available = has_accel;
    accel_.rate_hz = accel_rate_hz > 0.0f ? accel_rate_hz : 0.0f;
    gyro_.available = has_gyro;
    gyro_.rate_hz = gyro_rate_hz > 0.0f ? gyro_rate_hz : 0.0f;
    if (!has_accel) {
        accel_.history.clear();
        accel_.filter_ready = false;
        accel_.last_input_us = 0;
    }
    if (!has_gyro) {
        gyro_.history.clear();
        gyro_.filter_ready = false;
        gyro_.last_input_us = 0;
    }
}

void MotionPipeline::push(Stream& stream, uint64_t timestamp_us,
                          float x, float y, float z, float cutoff_hz) {
    if (!stream.available || timestamp_us == 0) return;
    if (stream.last_input_us != 0 && timestamp_us <= stream.last_input_us) return;

    const float input[3] = {x, y, z};
    if (!stream.filter_ready) {
        std::copy_n(input, 3, stream.filtered);
        stream.filter_ready = true;
    } else {
        const float dt = std::clamp(
            static_cast<float>(timestamp_us - stream.last_input_us) / 1'000'000.0f,
            0.0001f, 0.1000f);
        const float alpha = 1.0f - std::exp(-TWO_PI * cutoff_hz * dt);
        for (int axis = 0; axis < 3; ++axis)
            stream.filtered[axis] += alpha * (input[axis] - stream.filtered[axis]);
    }

    stream.last_input_us = timestamp_us;
    stream.history.push_back(TimedVec3{
        .timestamp_us = timestamp_us,
        .v = {stream.filtered[0], stream.filtered[1], stream.filtered[2]},
    });
    while (!stream.history.empty()
            && timestamp_us - stream.history.front().timestamp_us > HISTORY_US) {
        stream.history.pop_front();
    }
}

void MotionPipeline::push_accel(uint64_t timestamp_us, float x, float y, float z) {
    push(accel_, timestamp_us, x, y, z, ACCEL_CUTOFF_HZ);
}

void MotionPipeline::push_gyro(uint64_t timestamp_us, float x, float y, float z) {
    push(gyro_, timestamp_us, x, y, z, GYRO_CUTOFF_HZ);
}

uint64_t MotionPipeline::stale_timeout_us(const Stream& stream) {
    if (stream.rate_hz <= 0.0f) return MIN_STALE_US;
    const uint64_t four_periods = static_cast<uint64_t>(4'000'000.0f / stream.rate_hz);
    return std::clamp(four_periods, MIN_STALE_US, MAX_STALE_US);
}

bool MotionPipeline::interpolate(const Stream& stream, uint64_t timestamp_us,
                                 uint64_t stale_after_us, float out[3]) {
    if (!stream.available || stream.history.empty()) return false;
    const TimedVec3& newest = stream.history.back();
    if (timestamp_us > newest.timestamp_us
            && timestamp_us - newest.timestamp_us > stale_after_us) {
        return false;
    }

    if (timestamp_us <= stream.history.front().timestamp_us) {
        std::copy_n(stream.history.front().v, 3, out);
        return true;
    }
    if (timestamp_us >= newest.timestamp_us) {
        std::copy_n(newest.v, 3, out);
        return true;
    }

    auto after = std::lower_bound(
        stream.history.begin(), stream.history.end(), timestamp_us,
        [](const TimedVec3& sample, uint64_t time) { return sample.timestamp_us < time; });
    if (after == stream.history.end()) {
        std::copy_n(newest.v, 3, out);
        return true;
    }
    const TimedVec3& b = *after;
    const TimedVec3& a = *(after - 1);
    const uint64_t span = b.timestamp_us - a.timestamp_us;
    const float t = span == 0 ? 1.0f
        : static_cast<float>(timestamp_us - a.timestamp_us) / static_cast<float>(span);
    for (int axis = 0; axis < 3; ++axis) out[axis] = a.v[axis] + t * (b.v[axis] - a.v[axis]);
    return true;
}

uint64_t MotionPipeline::interpolation_delay_us() const {
    float fastest_rate = 0.0f;
    if (accel_.available) fastest_rate = std::max(fastest_rate, accel_.rate_hz);
    if (gyro_.available) fastest_rate = std::max(fastest_rate, gyro_.rate_hz);
    if (fastest_rate <= 0.0f) return OUTPUT_SAMPLE_INTERVAL_US;
    const uint64_t period = static_cast<uint64_t>(1'000'000.0f / fastest_rate);
    return std::clamp(period, 2'000ULL, 10'000ULL);
}

bool MotionPipeline::sample(uint64_t now_us, MotionReport out[3]) {
    if (!out) return false;
    for (int i = 0; i < 3; ++i) out[i].reset();

    const uint64_t delay = interpolation_delay_us();
    const uint64_t newest_target = now_us > delay ? now_us - delay : 0;
    const auto stream_is_fresh = [now_us](const Stream& stream) {
        return stream.available && stream.last_input_us != 0
            && (stream.last_input_us >= now_us
                || now_us - stream.last_input_us <= stale_timeout_us(stream));
    };
    const bool accel_live = stream_is_fresh(accel_);
    const bool gyro_live = stream_is_fresh(gyro_);
    bool any_fresh = false;
    for (int i = 0; i < 3; ++i) {
        const uint64_t age = static_cast<uint64_t>(2 - i) * OUTPUT_SAMPLE_INTERVAL_US;
        const uint64_t target = newest_target > age ? newest_target - age : 0;
        float accel[3] = {0.0f, 0.0f, 4096.0f};
        float gyro[3]{};
        const bool accel_fresh = accel_live
            && interpolate(accel_, target, stale_timeout_us(accel_), accel);
        const bool gyro_fresh = gyro_live
            && interpolate(gyro_, target, stale_timeout_us(gyro_), gyro);
        any_fresh = any_fresh || accel_fresh || gyro_fresh;

        out[i].ax = motion_i16(accel[0]);
        out[i].ay = motion_i16(accel[1]);
        out[i].az = motion_i16(accel[2]);
        out[i].gx = gyro_i16(gyro[0]);
        out[i].gy = gyro_i16(gyro[1]);
        out[i].gz = gyro_i16(gyro[2]);
    }
    return any_fresh;
}

} // namespace ns
