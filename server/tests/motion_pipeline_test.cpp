#include "shared/motion_pipeline.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

void test_consume_once() {
    ns::MotionPipeline pipeline;
    pipeline.configure(true, 100.0f, true, 100.0f);

    constexpr uint64_t t0 = 1'000'000;
    pipeline.push_accel(t0, 0.0f, 0.0f, 4096.0f);
    pipeline.push_gyro(t0, 200.0f, 0.0f, 0.0f);

    ns::MotionReport samples[3]{};
    assert(pipeline.sample(t0 + 10'000, samples));
    assert(samples[2].gx != 0);

    // A faster transport poll must not expose the same physical gyro sample a
    // second time.
    assert(!pipeline.sample(t0 + 12'000, samples));
    for (const auto& sample : samples) {
        assert(sample.gx == 0 && sample.gy == 0 && sample.gz == 0);
    }

    pipeline.push_gyro(t0 + 20'000, 220.0f, 0.0f, 0.0f);
    assert(pipeline.sample(t0 + 30'000, samples));
    assert(samples[2].gx != 0);
}

void test_stationary_bias_and_soft_deadzone() {
    ns::MotionPipeline pipeline;
    pipeline.configure(true, 100.0f, true, 100.0f);

    constexpr uint64_t t0 = 2'000'000;
    ns::MotionReport samples[3]{};
    for (int i = 0; i < 100; ++i) {
        const uint64_t t = t0 + static_cast<uint64_t>(i) * 10'000;
        pipeline.push_accel(t, 0.0f, 0.0f, 4096.0f);
        pipeline.push_gyro(t, 10.0f, -6.0f, 4.0f);
        (void)pipeline.sample(t + 10'000, samples);
    }

    assert(pipeline.gyro_bias_ready());
    assert(std::abs(pipeline.gyro_bias(0) - 10.0f) < 0.75f);
    assert(std::abs(pipeline.gyro_bias(1) + 6.0f) < 0.75f);
    assert(std::abs(pipeline.gyro_bias(2) - 4.0f) < 0.75f);

    const uint64_t still_t = t0 + 1'010'000;
    pipeline.push_accel(still_t, 0.0f, 0.0f, 4096.0f);
    pipeline.push_gyro(still_t, 11.0f, -5.0f, 4.5f);
    assert(pipeline.sample(still_t + 10'000, samples));
    assert(samples[2].gx == 0 && samples[2].gy == 0 && samples[2].gz == 0);

    const uint64_t move_t = still_t + 10'000;
    pipeline.push_accel(move_t, 0.0f, 0.0f, 4096.0f);
    pipeline.push_gyro(move_t, 110.0f, -6.0f, 4.0f);
    assert(pipeline.sample(move_t + 10'000, samples));
    assert(samples[2].gx > 20);
}

} // namespace

int main() {
    test_consume_once();
    test_stationary_bias_and_soft_deadzone();
    std::cout << "motion pipeline tests passed\n";
    return 0;
}
