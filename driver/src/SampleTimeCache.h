// Serves IO reads by device sample time. The HAL calls ReadInput once per
// client per IO cycle with the same sample time; the JitterBuffer has a single
// reader and must be consumed exactly once per frame of device time. This
// cache pulls each new range of device time from the source once, and every
// client reading that range gets the same samples.
//
// IO thread only (libASPL serializes IO calls per device). Real-time safe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace netmic {

template <typename Source>  // Source: void read(float* out, uint32_t frames)
class SampleTimeCache {
public:
    static constexpr uint32_t kFrames = 16384;  // > 2 × the largest HAL IO buffer

    explicit SampleTimeCache(Source& source) : source_(source), ring_(new float[kFrames]()) {}

    // Writes `frames` frames of `channels` interleaved samples for device
    // sample time [sampleTime, sampleTime + frames).
    void read(double sampleTime, float* out, uint32_t frames, uint32_t channels) {
        if (frames > kFrames / 2) {
            for (uint32_t i = 0; i < frames * channels; ++i) out[i] = 0;
            return;
        }
        const int64_t t = static_cast<int64_t>(std::llround(sampleTime));
        const int64_t end = t + frames;
        // First read, or device time jumped (IO restart, clock reset): start fresh at t.
        if (!started_ || end > produced_ + kFrames / 2 || t < produced_ - static_cast<int64_t>(kFrames / 2)) {
            produced_ = t;
            started_ = true;
        }
        while (produced_ < end) {
            const uint32_t idx = index(produced_);
            const uint32_t chunk = static_cast<uint32_t>(std::min<int64_t>(end - produced_, kFrames - idx));
            source_.read(ring_.get() + idx, chunk);
            produced_ += chunk;
        }
        for (uint32_t n = 0; n < frames; ++n) {
            const int64_t pos = t + n;
            const float s = pos >= produced_ - static_cast<int64_t>(kFrames) ? ring_[index(pos)] : 0.0f;
            for (uint32_t c = 0; c < channels; ++c) out[n * channels + c] = s;
        }
    }

    // Forget the timeline (e.g. on StartIO); the next read starts fresh.
    void reset() { started_ = false; }

private:
    static uint32_t index(int64_t pos) {
        const int64_t m = pos % static_cast<int64_t>(kFrames);
        return static_cast<uint32_t>(m < 0 ? m + kFrames : m);
    }

    Source& source_;
    std::unique_ptr<float[]> ring_;
    bool started_ = false;
    int64_t produced_ = 0;  // device time up to which the source has been read
};

}  // namespace netmic
