#include "s2_motion_carrier.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace ns::s2 {
namespace {

// Carrier layout and quaternion model follow hardware-confirmed
// implementation (Apache-2.0), adapted here to NS-PC-Control's
// already normalized 4096-counts/g MotionReport input.
constexpr uint64_t MOTION_PERIOD_US = 3'800;
constexpr uint64_t INTEGRATION_STEP_US = 4'000;
constexpr int GYRO_FP_SHIFT = 6;
constexpr int32_t GYRO_JITTER_LIMIT = 6 << GYRO_FP_SHIFT;
constexpr int32_t GYRO_STILL_LIMIT = 40;
constexpr int32_t GYRO_WARMUP_LIMIT = 40;
constexpr uint16_t GYRO_WARMUP_SAMPLES = 32;
constexpr float GYRO_COUNTS_PER_DPS = 16.384f;
constexpr float DEG_TO_RAD = 0.01745329251994329577f;
constexpr float INV_SQRT2 = 0.70710678118654752440f;
constexpr float COMPONENT_EPSILON = 0.00001f;
constexpr int32_t ACCEL_PDU_PER_COUNT = 68'963;

int32_t fixed_to_counts(int32_t value) {
    // Division truncates toward zero. An arithmetic shift would turn a tiny
    // negative fixed-point residue into a permanent -1-count rotation.
    return value / (1 << GYRO_FP_SHIFT);
}

int32_t bias_step(int32_t delta) {
    int32_t step = delta / 256;
    if (step == 0 && delta >= (1 << GYRO_FP_SHIFT)) step = 1;
    else if (step == 0 && delta <= -(1 << GYRO_FP_SHIFT)) step = -1;
    return step;
}

int16_t clamp_i16(int32_t value) {
    return static_cast<int16_t>(std::clamp(
        value,
        static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
        static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
}

int32_t clamp_i32(int64_t value) {
    return static_cast<int32_t>(std::clamp(
        value,
        static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

void write_i32le(uint8_t* out, int32_t value) {
    const uint32_t u = static_cast<uint32_t>(value);
    out[0] = static_cast<uint8_t>(u);
    out[1] = static_cast<uint8_t>(u >> 8);
    out[2] = static_cast<uint8_t>(u >> 16);
    out[3] = static_cast<uint8_t>(u >> 24);
}

// MotionReport is the established Switch-1 Pro frame (X forward, Y left,
// Z face-up). The hardware-validated Switch-1 seam remounts that
// frame into the Pro Controller 2 carrier as {-Y, +X, +Z}. Acceleration is
// already 4096 counts/g in NS-PC-Control, so it must not be halved here.
void prepare_sample(const MotionCarrierSample samples[3],
                    int16_t accel_out[3],
                    int16_t gyro_out[3]) {
    const int32_t accel_mean[3] = {
        (static_cast<int32_t>(samples[0].ax) + samples[1].ax + samples[2].ax) / 3,
        (static_cast<int32_t>(samples[0].ay) + samples[1].ay + samples[2].ay) / 3,
        (static_cast<int32_t>(samples[0].az) + samples[1].az + samples[2].az) / 3,
    };
    const int16_t newest_gyro[3] = {
        samples[2].gx, samples[2].gy, samples[2].gz
    };

    accel_out[0] = clamp_i16(-accel_mean[1]);
    accel_out[1] = clamp_i16( accel_mean[0]);
    accel_out[2] = clamp_i16( accel_mean[2]);
    gyro_out[0] = clamp_i16(-static_cast<int32_t>(newest_gyro[1]));
    gyro_out[1] = newest_gyro[0];
    gyro_out[2] = newest_gyro[2];
}

void normalize_quaternion(float q[4]) {
    const float norm_sq =
        q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (norm_sq <= 0.0f) return;

    float inverse_norm = 1.5f - 0.5f * norm_sq;
    inverse_norm *= 1.5f - 0.5f * norm_sq * inverse_norm * inverse_norm;
    for (unsigned i = 0; i < 4; ++i) q[i] *= inverse_norm;
}

void integrate_body_rate(float q[4], const float omega[3], float dt_seconds) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float ox = omega[0], oy = omega[1], oz = omega[2];
    const float half_dt = 0.5f * dt_seconds;

    q[0] += half_dt * (w * ox + y * oz - z * oy);
    q[1] += half_dt * (w * oy - x * oz + z * ox);
    q[2] += half_dt * (w * oz + x * oy - y * ox);
    q[3] += half_dt * (-x * ox - y * oy - z * oz);
    normalize_quaternion(q);
}

uint32_t encode_g0(float value) {
    const float scaled = (value * INV_SQRT2 + 0.5f) * 67'108'864.0f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 67'108'863.0f) return 0x03FFFFFFu;
    return static_cast<uint32_t>(scaled + 0.5f);
}

uint32_t encode_g1(float value) {
    const float scaled = (value * INV_SQRT2 + 0.5f) * 33'554'432.0f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 33'554'431.0f) return 0x01FFFFFFu;
    return static_cast<uint32_t>(scaled + 0.5f);
}

uint32_t encode_g2(float value) {
    const float scaled = (value * INV_SQRT2 + 0.5f) * 16'777'216.0f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 16'777'215.0f) return 0x00FFFFFFu;
    return static_cast<uint32_t>(scaled + 0.5f);
}

bool encode_orientation(MotionCarrierState& state, uint32_t orientation[3]) {
    // Wire component order is w/x/y/z; the in-memory order is x/y/z/w.
    const float wire[4] = {
        state.quaternion[3],
        state.quaternion[0],
        state.quaternion[1],
        state.quaternion[2],
    };
    unsigned omitted = state.omitted_component & 3u;
    bool boundary_reached = false;
    for (unsigned i = 0; i < 4; ++i) {
        if (i != omitted && std::fabs(wire[i]) > INV_SQRT2) {
            boundary_reached = true;
            break;
        }
    }
    if (boundary_reached) {
        omitted = 0;
        for (unsigned i = 1; i < 4; ++i) {
            if (std::fabs(wire[i]) > std::fabs(wire[omitted])) omitted = i;
        }
        state.omitted_component = static_cast<uint8_t>(omitted);
    }

    // q and -q describe the same orientation. Keeping the omitted component
    // positive lets the receiver reconstruct it with the positive root.
    const float sign = wire[omitted] < 0.0f ? -1.0f : 1.0f;
    const float c0 = wire[(omitted + 1u) & 3u] * sign;
    const float c1 = wire[(omitted + 2u) & 3u] * sign;
    const float c2 = wire[(omitted + 3u) & 3u] * sign;
    const auto invalid = [](float v) {
        return !std::isfinite(v)
            || v < -INV_SQRT2 - COMPONENT_EPSILON
            || v >  INV_SQRT2 + COMPONENT_EPSILON;
    };
    if (invalid(c0) || invalid(c1) || invalid(c2)) return false;

    orientation[0] = encode_g0(c0);
    orientation[1] = encode_g1(c1);
    orientation[2] = (omitted << 24) | encode_g2(c2);
    return true;
}

void set_orientation(uint8_t pdu[MOTION_CARRIER_SIZE],
                     const uint32_t orientation[3]) {
    const uint32_t g0 = orientation[0] & 0x03FFFFFFu;
    const uint32_t g1 = orientation[1] & 0x03FFFFFFu;
    const uint32_t g2 = orientation[2] & 0x03FFFFFFu;

    pdu[5] = static_cast<uint8_t>(g0);
    pdu[6] = static_cast<uint8_t>(g0 >> 8);
    pdu[7] = static_cast<uint8_t>(g0 >> 16);
    pdu[8] = static_cast<uint8_t>((pdu[8] & 0xFCu) | ((g0 >> 24) & 0x03u));
    pdu[9] = static_cast<uint8_t>(g1);
    pdu[10] = static_cast<uint8_t>(g1 >> 8);
    pdu[11] = static_cast<uint8_t>(g1 >> 16);
    pdu[12] = static_cast<uint8_t>((pdu[12] & 0xFCu) | ((g1 >> 24) & 0x03u));
    pdu[13] = static_cast<uint8_t>(g2);
    pdu[14] = static_cast<uint8_t>(g2 >> 8);
    pdu[15] = static_cast<uint8_t>(g2 >> 16);
    pdu[4] = static_cast<uint8_t>((pdu[4] & 0xFCu) | ((g2 >> 24) & 0x03u));
}

bool build_carrier(MotionCarrierState& state) {
    std::array<uint8_t, MOTION_CARRIER_SIZE> carrier{};
    carrier[0] = static_cast<uint8_t>(state.timing);
    carrier[1] = static_cast<uint8_t>(state.timing >> 8);
    carrier[2] = 0x00;
    carrier[3] = 0x0C; // genuine nominal temperature: 0x0C00

    uint32_t orientation[3]{};
    if (!encode_orientation(state, orientation)) return false;
    set_orientation(carrier.data(), orientation);

    for (unsigned axis = 0; axis < 3; ++axis) {
        write_i32le(
            carrier.data() + 16 + axis * 4,
            clamp_i32(static_cast<int64_t>(state.accel[axis])
                      * ACCEL_PDU_PER_COUNT));
    }
    carrier[28] = 0x00;
    carrier[29] = 0x02;
    state.carrier = carrier;
    state.carrier_valid = true;
    return true;
}

} // namespace

void reset_motion_carrier(MotionCarrierState& state) {
    state = MotionCarrierState{};
    state.quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
}

bool update_motion_carrier(MotionCarrierState& state,
                           const MotionCarrierSample samples[3],
                           uint64_t now_us) {
    if (!samples) return false;

    int16_t accel[3]{};
    int16_t gyro[3]{};
    prepare_sample(samples, accel, gyro);

    if (!state.initialized) {
        state.initialized = true;
        state.last_update_us = now_us;
        std::copy_n(accel, 3, state.accel.begin());
        for (unsigned axis = 0; axis < 3; ++axis) {
            state.gyro_lp[axis] =
                static_cast<int32_t>(gyro[axis]) * (1 << GYRO_FP_SHIFT);
            state.gyro_prev[axis] = gyro[axis];
        }
        return build_carrier(state);
    }

    uint64_t elapsed_us = now_us > state.last_update_us
        ? now_us - state.last_update_us : 0;
    if (elapsed_us < MOTION_PERIOD_US) return false;
    elapsed_us = std::clamp<uint64_t>(elapsed_us, 500, 16'000);
    state.last_update_us = now_us;

    bool steady = true;
    for (unsigned axis = 0; axis < 3; ++axis) {
        int32_t delta = static_cast<int32_t>(gyro[axis]) - state.gyro_prev[axis];
        state.gyro_prev[axis] = gyro[axis];
        delta = std::abs(delta);
        state.gyro_jitter[axis] +=
            ((delta * (1 << GYRO_FP_SHIFT)) - state.gyro_jitter[axis]) >> 3;
        if (state.gyro_jitter[axis] > GYRO_JITTER_LIMIT) steady = false;
    }

    for (unsigned axis = 0; axis < 3; ++axis) {
        state.gyro_lp[axis] +=
            (((static_cast<int32_t>(gyro[axis]) * (1 << GYRO_FP_SHIFT))
              - state.gyro_lp[axis]) >> 2);
        const int32_t before_bias =
            fixed_to_counts(state.gyro_lp[axis] - state.gyro_bias[axis]);
        if (std::abs(before_bias) > GYRO_STILL_LIMIT) steady = false;
    }

    int32_t corrected[3]{};
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (steady) {
            state.gyro_bias[axis] +=
                bias_step(state.gyro_lp[axis] - state.gyro_bias[axis]);
        }
        corrected[axis] = fixed_to_counts(
            (steady ? state.gyro_lp[axis]
                    : static_cast<int32_t>(gyro[axis]) * (1 << GYRO_FP_SHIFT))
            - state.gyro_bias[axis]);
    }

    if (!state.bias_ready) {
        bool warmup_still = true;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (std::abs(static_cast<int32_t>(gyro[axis])) > GYRO_WARMUP_LIMIT
                || state.gyro_jitter[axis]
                    > (GYRO_WARMUP_LIMIT << GYRO_FP_SHIFT)) {
                warmup_still = false;
            }
        }
        if (warmup_still) {
            ++state.bias_warmup_samples;
            for (unsigned axis = 0; axis < 3; ++axis) {
                state.gyro_bias[axis] = state.gyro_lp[axis];
                corrected[axis] = 0;
            }
            if (state.bias_warmup_samples >= GYRO_WARMUP_SAMPLES)
                state.bias_ready = true;
        } else {
            state.bias_warmup_samples = 0;
            state.gyro_bias.fill(0);
            std::fill_n(corrected, 3, 0);
        }
    }

    if (state.bias_ready) {
        const float scale = DEG_TO_RAD / GYRO_COUNTS_PER_DPS;
        const float omega[3] = {
            corrected[0] * scale,
            corrected[1] * scale,
            corrected[2] * scale,
        };
        uint64_t remaining_us = elapsed_us;
        while (remaining_us != 0) {
            const uint64_t step_us = std::min(remaining_us, INTEGRATION_STEP_US);
            integrate_body_rate(
                state.quaternion.data(), omega,
                static_cast<float>(step_us) / 1'000'000.0f);
            remaining_us -= step_us;
        }
    }

    uint16_t count = static_cast<uint16_t>((elapsed_us + 625) / 1'250);
    count = std::clamp<uint16_t>(count, 1, 15);
    state.imu_tick = static_cast<uint16_t>((state.imu_tick + count) & 0x0FFFu);
    state.timing = static_cast<uint16_t>((count << 12) | state.imu_tick);
    std::copy_n(accel, 3, state.accel.begin());
    return build_carrier(state);
}

} // namespace ns::s2
