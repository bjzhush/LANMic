#include "JitterBuffer.h"

#include <algorithm>
#include <cmath>

namespace netmic {

namespace {

inline float toFloat(int16_t s) { return static_cast<float>(s) * (1.0f / 32768.0f); }
inline float toFloat(float s) { return s; }

}  // namespace

JitterBuffer::JitterBuffer(const JitterBufferConfig& config)
    : config_(config), ring_(new std::atomic<float>[config.capacityFrames]) {
    for (uint32_t i = 0; i < config_.capacityFrames; ++i) ring_[i].store(0.0f, std::memory_order_relaxed);
}

void JitterBuffer::write(uint32_t streamId, uint64_t samplePos, const int16_t* pcm, uint32_t frames) {
    writeImpl(streamId, samplePos, pcm, frames);
}

void JitterBuffer::write(uint32_t streamId, uint64_t samplePos, const float* samples, uint32_t frames) {
    writeImpl(streamId, samplePos, samples, frames);
}

void JitterBuffer::resetTo(int64_t base) {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    readPos_.store(base, std::memory_order_release);
    writePos_.store(base, std::memory_order_release);
    statResets_.fetch_add(1, std::memory_order_relaxed);
}

void JitterBuffer::clear() {
    resetRequested_.store(false, std::memory_order_relaxed);
    resetTo(writePos_.load(std::memory_order_relaxed) + config_.capacityFrames);
    anchored_ = false;
}

template <typename Sample>
void JitterBuffer::writeImpl(uint32_t streamId, uint64_t samplePos, const Sample* samples, uint32_t frames) {
    const int64_t cap = config_.capacityFrames;
    if (frames == 0 || frames > cap || samples == nullptr) return;

    int64_t w = writePos_.load(std::memory_order_relaxed);  // only this thread stores it (besides resetTo)

    // Starts a fresh mapping with this packet at the head of an empty buffer.
    // The new base is pushed a full capacity past the old write position so it
    // can never equal a stale reader position (keeps the reader's CAS ABA-free).
    auto anchor = [&] {
        resetTo(w + cap);
        anchorBase_ = w + cap;
        anchorSample_ = samplePos;
        streamId_ = streamId;
        anchored_ = true;
    };

    if (resetRequested_.exchange(false, std::memory_order_acq_rel) || !anchored_ || streamId != streamId_) {
        anchor();
        w = writePos_.load(std::memory_order_relaxed);
    }

    int64_t r = readPos_.load(std::memory_order_acquire);

    // Work relative to the anchor so arbitrary 64-bit samplePos values cannot overflow.
    int64_t delta = static_cast<int64_t>(samplePos - anchorSample_);
    if (delta > (w - anchorBase_) + cap) {  // absurdly far ahead (e.g. sender restarted with a gap)
        anchor();
        w = r = anchorBase_;
        delta = 0;
    }
    if (delta + frames <= r - anchorBase_) {  // entirely older than what the reader has consumed
        statLateDrops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    int64_t pos = anchorBase_ + delta;
    int64_t end = pos + frames;
    if (end > r + cap) {  // would overwrite unread data: the reader is stalled or far behind
        anchor();
        w = r = pos = anchorBase_;
        end = pos + frames;
    }

    if (pos > w) {  // missing packets: fill the hole with silence
        for (int64_t p = w; p < pos; ++p) ring_[slot(p)].store(0.0f, std::memory_order_relaxed);
        statGapFrames_.fetch_add(static_cast<uint64_t>(pos - w), std::memory_order_relaxed);
    }

    // Slots in [r, w) may already be visible to the reader (late packet patching a
    // zero-filled hole); atomics make that a benign race.
    for (int64_t p = std::max(pos, r); p < end; ++p) {
        ring_[slot(p)].store(toFloat(samples[p - pos]), std::memory_order_relaxed);
    }
    if (end > w) writePos_.store(end, std::memory_order_release);
    statPackets_.fetch_add(1, std::memory_order_relaxed);
}

void JitterBuffer::read(float* out, uint32_t frames) {
    if (out == nullptr || frames == 0) return;
    // The depth seen just before a read must cover the read itself plus the
    // jitter margin, whatever IO size the client uses.
    const int64_t targetFrames = static_cast<int64_t>(config_.targetFrames) + frames;
    const double target = static_cast<double>(targetFrames);

    const uint64_t gen = generation_.load(std::memory_order_acquire);
    if (gen != readerGeneration_) {
        readerGeneration_ = gen;
        playing_ = false;
        frac_ = 0;
    }

    const int64_t r = readPos_.load(std::memory_order_acquire);
    const int64_t w = writePos_.load(std::memory_order_acquire);
    int64_t avail = std::max<int64_t>(0, w - r);
    double depth = static_cast<double>(avail) - frac_;

    auto silence = [&] { std::fill(out, out + frames, 0.0f); };
    auto publish = [&](double ratio) {
        // Reported as the margin above this read, comparable to config_.targetFrames.
        statDepth_.store((playing_ ? smoothedDepth_ : depth) - frames, std::memory_order_relaxed);
        statRatioPpm_.store((ratio - 1.0) * 1e6, std::memory_order_relaxed);
        statPlaying_.store(playing_, std::memory_order_relaxed);
    };

    if (!playing_) {
        if (avail >= targetFrames) {
            playing_ = true;
            frac_ = 0;
            depth = static_cast<double>(avail);
            smoothedDepth_ = depth;
        } else {
            silence();
            publish(1.0);
            return;
        }
    }

    int64_t base = r;
    if (depth > target + config_.overrunMarginFrames) {
        base = w - targetFrames;
        avail = targetFrames;
        frac_ = 0;
        depth = smoothedDepth_ = target;
        statOverruns_.fetch_add(1, std::memory_order_relaxed);
    }

    double ratio = 1.0;
    if (config_.driftCompensation) {
        const double dt = static_cast<double>(frames) / config_.sampleRate;
        const double alpha = 1.0 - std::exp(-dt / config_.smoothingTauSec);
        smoothedDepth_ += alpha * (depth - smoothedDepth_);
        const double err = smoothedDepth_ - target;
        const double maxCorr = config_.maxCorrectionPpm * 1e-6;
        integral_ += err * dt;
        if (config_.ki > 0) integral_ = std::clamp(integral_, -maxCorr / config_.ki, maxCorr / config_.ki);
        ratio = 1.0 + std::clamp(config_.kp * err + config_.ki * integral_, -maxCorr, maxCorr);
    } else {
        smoothedDepth_ = depth;
    }

    // Linear-interpolating read: output i samples the input at base + frac + i*ratio.
    double p = frac_;
    uint32_t i = 0;
    bool underrun = false;
    for (; i < frames; ++i) {
        const int64_t ip = static_cast<int64_t>(p);
        const float f = static_cast<float>(p - static_cast<double>(ip));
        if (ip >= avail || (f != 0.0f && ip + 1 >= avail)) {
            underrun = true;
            break;
        }
        const float a = ring_[slot(base + ip)].load(std::memory_order_relaxed);
        if (f == 0.0f) {
            out[i] = a;
        } else {
            const float b = ring_[slot(base + ip + 1)].load(std::memory_order_relaxed);
            out[i] = a + (b - a) * f;
        }
        p += ratio;
    }

    int64_t consumed;
    if (underrun) {
        std::fill(out + i, out + frames, 0.0f);
        consumed = avail;
        frac_ = 0;
        playing_ = false;
        statUnderruns_.fetch_add(1, std::memory_order_relaxed);
    } else {
        consumed = static_cast<int64_t>(p);
        frac_ = p - static_cast<double>(consumed);
    }

    int64_t expected = r;
    if (!readPos_.compare_exchange_strong(expected, base + consumed, std::memory_order_acq_rel) ||
        generation_.load(std::memory_order_acquire) != gen) {
        // The writer reset the buffer while we were reading; what we produced is stale.
        silence();
        playing_ = false;
        frac_ = 0;
    }
    publish(ratio);
}

JitterBufferStats JitterBuffer::stats() const {
    JitterBufferStats s;
    s.packets = statPackets_.load(std::memory_order_relaxed);
    s.lateDrops = statLateDrops_.load(std::memory_order_relaxed);
    s.gapFrames = statGapFrames_.load(std::memory_order_relaxed);
    s.resets = statResets_.load(std::memory_order_relaxed);
    s.underruns = statUnderruns_.load(std::memory_order_relaxed);
    s.overruns = statOverruns_.load(std::memory_order_relaxed);
    s.depthFrames = statDepth_.load(std::memory_order_relaxed);
    s.ratioPpm = statRatioPpm_.load(std::memory_order_relaxed);
    s.playing = statPlaying_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace netmic
