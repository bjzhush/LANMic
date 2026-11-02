// Lock-free single-producer / single-consumer jitter buffer with clock drift
// compensation. No Core Audio dependency.
//
// Threads:
//   - writer (network thread): write(), requestReset() may also be called from any thread.
//   - reader (IO thread, real-time): read(). Never allocates, locks or makes syscalls.
//   - stats(): any thread.
//
// Positions live in an internal monotonically increasing frame coordinate.
// The writer maps the sender's samplePos into it with a per-stream offset.
// Ring slots are std::atomic<float> (plain loads/stores on arm64), so the
// writer may patch holes the reader can already see without a data race.
//
// Resets (new streamId, packet too far ahead, requestReset) are performed by
// the writer: it bumps `generation_` and moves both positions forward. The
// reader advances its position with a CAS and re-checks the generation, so a
// read that races with a reset is discarded (outputs silence) rather than
// corrupting state.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace netmic {

struct JitterBufferConfig {
    uint32_t sampleRate = 48000;
    uint32_t capacityFrames = 24000;      // 500 ms
    // Jitter margin: the reader aims to have this much buffered beyond what the
    // current read() takes, so the effective depth adapts to the client's IO size.
    uint32_t targetFrames = 960;          // 20 ms
    uint32_t overrunMarginFrames = 2880;  // 60 ms above target
    bool driftCompensation = true;

    // PI controller on the smoothed depth error (in frames). Output is the
    // resampling ratio offset, clamped to ±maxCorrectionPpm.
    // Defaults: ωn ≈ 0.25 rad/s, ζ ≈ 1 (critically damped); settles in ~15–20 s.
    // Depth is smoothed over 2 s to keep packet-granularity noise out of the ratio.
    double smoothingTauSec = 2.0;
    double kp = 1.04e-5;  // per frame
    double ki = 1.30e-6;  // per frame·second
    double maxCorrectionPpm = 1000.0;

    // Catch-up (drift compensation only): when the depth stays more than
    // catchUpMarginFrames above target for catchUpSec (e.g. a network stall
    // followed by a burst), the excess is dropped in one step instead of being
    // drained at maxCorrectionPpm, which would take ~1 s per ms of excess.
    uint32_t catchUpMarginFrames = 720;  // 15 ms; 0 disables
    double catchUpSec = 0.5;
};

struct JitterBufferStats {
    uint64_t packets = 0;      // accepted write() calls
    uint64_t lateDrops = 0;    // packets entirely behind the read position
    uint64_t gapFrames = 0;    // frames zero-filled because packets were missing
    uint64_t resets = 0;       // writer-side resets (stream change, out of window, requested)
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    uint64_t catchUps = 0;     // excess dropped after a sustained surplus
    double depthFrames = 0;    // smoothed depth seen by the reader, minus the read size (≈ targetFrames)
    double ratioPpm = 0;       // current resampling correction
    bool playing = false;
};

class JitterBuffer {
public:
    explicit JitterBuffer(const JitterBufferConfig& config = {});

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    // ---- writer side ----
    void write(uint32_t streamId, uint64_t samplePos, const int16_t* pcm, uint32_t frames);
    void write(uint32_t streamId, uint64_t samplePos, const float* samples, uint32_t frames);

    // Thread-safe request; applied by the writer on its next write().
    void requestReset() { resetRequested_.store(true, std::memory_order_release); }

    // Writer thread only: empties the buffer now (the reader goes back to
    // Buffering and outputs silence); the next write() starts a new mapping.
    void clear();

    // ---- reader side (real-time safe) ----
    // Always fills `frames` samples of `out` (silence when no data).
    void read(float* out, uint32_t frames);

    // ---- any thread ----
    JitterBufferStats stats() const;
    const JitterBufferConfig& config() const { return config_; }

private:
    template <typename Sample>
    void writeImpl(uint32_t streamId, uint64_t samplePos, const Sample* samples, uint32_t frames);
    void resetTo(int64_t base);
    uint32_t slot(int64_t pos) const { return static_cast<uint32_t>(pos % config_.capacityFrames); }

    const JitterBufferConfig config_;
    std::unique_ptr<std::atomic<float>[]> ring_;

    // Shared state.
    std::atomic<uint64_t> generation_{0};
    std::atomic<int64_t> readPos_{0};
    std::atomic<int64_t> writePos_{0};
    std::atomic<bool> resetRequested_{false};

    // Writer-owned.
    bool anchored_ = false;
    uint32_t streamId_ = 0;
    int64_t anchorBase_ = 0;     // internal position of anchorSample_
    uint64_t anchorSample_ = 0;  // samplePos that started the current mapping

    // Reader-owned.
    uint64_t readerGeneration_ = ~0ull;
    bool playing_ = false;
    double frac_ = 0;  // fractional read position in [0, 1)
    double smoothedDepth_ = 0;
    double integral_ = 0;
    double excessSec_ = 0;            // how long depth has been above target + catch-up margin
    double minExcess_ = 0;            // smallest excess seen in that time (safe amount to drop)
    double integralBeforeExcess_ = 0;

    // Stats (relaxed).
    std::atomic<uint64_t> statPackets_{0}, statLateDrops_{0}, statGapFrames_{0}, statResets_{0};
    std::atomic<uint64_t> statUnderruns_{0}, statOverruns_{0}, statCatchUps_{0};
    std::atomic<double> statDepth_{0}, statRatioPpm_{0};
    std::atomic<bool> statPlaying_{false};

    static_assert(std::atomic<float>::is_always_lock_free, "ring slots must be lock-free");
    static_assert(std::atomic<int64_t>::is_always_lock_free, "positions must be lock-free");
    static_assert(std::atomic<double>::is_always_lock_free, "stats must be lock-free");
};

}  // namespace netmic
