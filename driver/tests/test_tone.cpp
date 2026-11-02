// Stage 2 test tone: level, frequency, and phase continuity across IO buffers.
#include <cmath>
#include <vector>

#include "ToneGenerator.h"
#include "test_harness.h"

TEST(tone_level_and_frequency) {
    netmic::ToneGenerator tone(440.0, 0.25f, 48000.0);
    std::vector<float> buf(48000);
    tone.fill(buf.data(), 48000, 1, 0);
    float peak = 0;
    int crossings = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        peak = std::max(peak, std::fabs(buf[i]));
        if (i > 0 && buf[i - 1] < 0 && buf[i] >= 0) ++crossings;
    }
    CHECK_MSG(std::fabs(20 * std::log10(peak) - (-12.04f)) < 0.01f, "peak %f", peak);
    CHECK_MSG(crossings >= 439 && crossings <= 440, "%d upward zero crossings in 1 s", crossings);
}

TEST(tone_continuous_across_buffers_and_channels) {
    // Same signal whether produced in one call or in odd-sized chunks at
    // consecutive sample times, far into the run (≈ 6 days of sample time).
    netmic::ToneGenerator tone(440.0, 0.25f, 48000.0);
    const double t0 = 48000.0 * 86400 * 6 + 0.0;
    std::vector<float> whole(1000), parts(2000);
    tone.fill(whole.data(), 1000, 1, t0);
    uint32_t done = 0;
    for (uint32_t chunk : {1u, 511u, 7u, 481u}) {
        tone.fill(parts.data() + 2 * done, chunk, 2, t0 + done);
        done += chunk;
    }
    CHECK(done == 1000);
    float maxErr = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        maxErr = std::max(maxErr, std::fabs(whole[i] - parts[2 * i]));
        maxErr = std::max(maxErr, std::fabs(whole[i] - parts[2 * i + 1]));
    }
    CHECK_MSG(maxErr < 1e-6f, "max error %g", maxErr);
}
