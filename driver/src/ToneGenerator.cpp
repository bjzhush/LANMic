#include "ToneGenerator.h"

#include <cmath>

namespace netmic {

void ToneGenerator::fill(float* out, uint32_t frames, uint32_t channels, double sampleTime) const {
    // Phase in cycles, reduced so precision does not degrade as sample time grows.
    const double cyclesPerFrame = frequency_ / sampleRate_;
    const double start = std::fmod(sampleTime * cyclesPerFrame, 1.0);
    for (uint32_t n = 0; n < frames; ++n) {
        const float s = amplitude_ * static_cast<float>(std::sin(2 * M_PI * (start + n * cyclesPerFrame)));
        for (uint32_t c = 0; c < channels; ++c) out[n * channels + c] = s;
    }
}

}  // namespace netmic
