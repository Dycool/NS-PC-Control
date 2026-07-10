#include "shared/motion_pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace ns {
namespace {

constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float ACCEL_CUTOFF_HZ = 30.0f;
constexpr float GYRO_CUTOFF_HZ = 35.0f;
constexpr float GYRO_SOFT_DEADZONE = 6.0f;       // ~0.37 degrees/second
constexpr float GYRO_STATIONARY_LIMIT = 48.0f;  // ~2.93 degrees/second
constexpr float ACCEL_ONE_G = 4096.0f;
constexpr float ACCEL_STATIONARY_TOLERANCE = 0.08f * ACCEL_ONE_G;
constexpr uint64_t BIAS_STARTUP_US = 750'000;
constexpr uint32_t BIAS_MIN_SAMPLES = 20;
constexpr float BIAS_ADAPT_TIME_CONSTANT_S = 12.0f;
constexpr uint64_t HISTORY_US = 250'000;
constexpr uint64_t MIN_STALE_US = 50'000;
constexpr uint64_t MAX_STALE_US = 150'000;

int16_t motion_i16(float value) {
    return static_cast<int16_t>(std::clamp<long>(std::lround(value), -32768L, 32767L));
}

float soft_deadzone(float value) {
    const float magnitude = std::abs(value);
    if (magnitude <= GYRO_SOFT_DEADZONE) return 0.0f;
    return std::copysign(magnitude - GYRO_SOFT_DEADZONE, value);
}

int16_t gyro_i16(float value) {
    return motion_i16(soft_deadzone(value));
}

float vec_magnitude(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void MotionPipeline::reset() {
    accel_ = Stream{};
    gyro_ = Stream{};
    std::fill_n(gyro_bias_, 3, 0.0f);
    std::fill_n(gyro_bias_sum_, 3, 0.0);
    gyro_bias_sample_count_ = 0;
    gyro_stationary_since_us_ = 0;
    gyro_bias_last_update_us_ = 0;
    gyro_bias_ready_ = false;
    last_emitted_gyro_input_us_ = 0;
    last_emitted_accel_input_us_ = 0;
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
        last_emitted_accel_input_us_ = 0;
    }
    if (!has_gyro) {
        gyro_.history.clear();
        gyro_.filter_ready = false;
        gyro_.last_input_us = 0;
        last_emitted_gyro_input_us_ = 0;
        std::fill_n(gyro_bias_, 3, 0.0f);
        std::fill_n(gyro_bias_sum_, 3, 0.0);
        gyro_bias_sample_count_ = 0;
        gyro_stationary_since_us_ = 0;
        gyro_bias_last_update_us_ = 0;
        gyro_bias_ready_ = false;
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

bool MotionPipeline::accel_indicates_stationary(uint64_t timestamp_us) const {
    if (!accel_.available) return true;
    if (!accel_.filter_ready || accel_.last_input_us == 0) return false;
    if (timestamp_us > accel_.last_input_us
            && timestamp_us - accel_.last_input_us > stale_timeout_us(accel_)) {
        return false;
    }
    const float magnitude = vec_magnitude(accel_.filtered);
    return std::abs(magnitude - ACCEL_ONE_G) <= ACCEL_STATIONARY_TOLERANCE;
}

void MotionPipeline::update_gyro_bias(uint64_t timestamp_us, const float raw[3]) {
    float corrected_for_test[3] = {
        raw[0] - gyro_bias_[0],
        raw[1] - gyro_bias_[1],
        raw[2] - gyro_bias_[2],
    };
    const bool stationary = accel_indicates_stationary(timestamp_us)
        && vec_magnitude(corrected_for_test) <= GYRO_STATIONARY_LIMIT;

    if (!stationary) {
        gyro_stationary_since_us_ = 0;
        gyro_bias_sample_count_ = 0;
        std::fill_n(gyro_bias_sum_, 3, 0.0);
        gyro_bias_last_update_us_ = timestamp_us;
        return;
    }

    if (gyro_stationary_since_us_ == 0) {
        gyro_stationary_since_us_ = timestamp_us;
        gyro_bias_sample_count_ = 0;
        std::fill_n(gyro_bias_sum_, 3, 0.0);
    }

    if (!gyro_bias_ready_) {
        for (int axis = 0; axis < 3; ++axis) gyro_bias_sum_[axis] += raw[axis];
        ++gyro_bias_sample_count_;
        if (timestamp_us - gyro_stationary_since_us_ >= BIAS_STARTUP_US
                && gyro_bias_sample_count_ >= BIAS_MIN_SAMPLES) {
            for (int axis = 0; axis < 3; ++axis) {
                gyro_bias_[axis] = static_cast<float>(
                    gyro_bias_sum_[axis] / static_cast<double>(gyro_bias_sample_count_));
            }
            gyro_bias_ready_ = true;
            gyro_bias_last_update_us_ = timestamp_us;
        }
        return;
    }

    const uint64_t dt_us = gyro_bias_last_update_us_ == 0
        ? 0 : timestamp_us - gyro_bias_last_update_us_;
    gyro_bias_last_update_us_ = timestamp_us;
    if (dt_us == 0) return;
    const float dt_s = std::min(static_cast<float>(dt_us) / 1'000'000.0f, 0.1f);
    const float alpha = 1.0f - std::exp(-dt_s / BIAS_ADAPT_TIME_CONSTANT_S);
    for (int axis = 0; axis < 3; ++axis)
        gyro_bias_[axis] += alpha * (raw[axis] - gyro_bias_[axis]);
}

void MotionPipeline::push_gyro(uint64_t timestamp_us, float x, float y, float z) {
    if (!gyro_.available || timestamp_us == 0) return;
    if (gyro_.last_input_us != 0 && timestamp_us <= gyro_.last_input_us) return;

    const float raw[3] = {x, y, z};
    update_gyro_bias(timestamp_us, raw);

    float corrected[3] = {x, y, z};
    if (gyro_bias_ready_) {
        for (int axis = 0; axis < 3; ++axis) corrected[axis] -= gyro_bias_[axis];
    } else if (gyro_stationary_since_us_ != 0) {
        // During a confirmed stationary startup calibration, do not expose the
        // offset we are currently measuring as real angular velocity.
        std::fill_n(corrected, 3, 0.0f);
    }
    push(gyro_, timestamp_us, corrected[0], corrected[1], corrected[2], GYRO_CUTOFF_HZ);
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

bool MotionPipeline::sample(uint64_t now_us, MotionReport out[3]) {
    if (!out) return false;
    for (int i = 0; i < 3; ++i) out[i].reset();

    const auto stream_is_fresh = [now_us](const Stream& stream) {
        return stream.available && stream.last_input_us != 0
            && (stream.last_input_us >= now_us
                || now_us - stream.last_input_us <= stale_timeout_us(stream));
    };
    const bool accel_live = stream_is_fresh(accel_);
    const bool gyro_live = stream_is_fresh(gyro_);
    const bool new_gyro = gyro_live && gyro_.last_input_us != last_emitted_gyro_input_us_;
    const bool new_accel = accel_live && accel_.last_input_us != last_emitted_accel_input_us_;

    // Gyro is the time-integrated quantity. When present, it is the pacing
    // source: one physical gyro sample may be exposed only once even if the
    // client/network/report loop polls this function more frequently.
    const bool emit = gyro_.available ? new_gyro : new_accel;
    if (!emit) return false;

    // Anchor the newest output slot to the newly consumed physical sample.
    // Delaying by one sensor period and then marking that sample consumed would
    // discard its newest portion when the transport polls faster than the IMU.
    const uint64_t newest_target = new_gyro
        ? gyro_.last_input_us
        : accel_.last_input_us;
    for (int i = 0; i < 3; ++i) {
        const uint64_t age = static_cast<uint64_t>(2 - i) * OUTPUT_SAMPLE_INTERVAL_US;
        const uint64_t target = newest_target > age ? newest_target - age : 0;
        float accel[3] = {0.0f, 0.0f, 4096.0f};
        float gyro[3]{};
        if (accel_live) (void)interpolate(accel_, target, stale_timeout_us(accel_), accel);
        if (new_gyro) (void)interpolate(gyro_, target, stale_timeout_us(gyro_), gyro);

        out[i].ax = motion_i16(accel[0]);
        out[i].ay = motion_i16(accel[1]);
        out[i].az = motion_i16(accel[2]);
        out[i].gx = gyro_i16(gyro[0]);
        out[i].gy = gyro_i16(gyro[1]);
        out[i].gz = gyro_i16(gyro[2]);
    }

    if (new_gyro) last_emitted_gyro_input_us_ = gyro_.last_input_us;
    if (new_accel) last_emitted_accel_input_us_ = accel_.last_input_us;
    return true;
}

} // namespace ns
