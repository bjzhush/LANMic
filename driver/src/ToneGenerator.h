// Stage 2 test signal: a sine computed from the device sample time, so every
// client sees the same continuous waveform. Real-time safe.
#pragma once

#include <cstdint>

namespace netmic {

class ToneGenerator {
public:
    ToneGenerator(double frequency, float amplitude, double sampleRate)
        : frequency_(frequency), amplitude_(amplitude), sampleRate_(sampleRate) {}

    // Fills `frames` interleaved frames of `channels` Float32 samples; frame n
    // is at device sample time `sampleTime + n`.
    void fill(float* out, uint32_t frames, uint32_t channels, double sampleTime) const;

private:
    double frequency_;
    float amplitude_;
    double sampleRate_;
};

}  // namespace netmic
