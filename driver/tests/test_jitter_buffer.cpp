#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "JitterBuffer.h"
#include "test_harness.h"

using netmic::JitterBuffer;
using netmic::JitterBufferConfig;

namespace {

constexpr uint32_t kFpp = 240;
constexpr uint32_t kIo = 512;

// Deterministic content: sample value encodes its stream position.
int16_t rampAt(uint64_t pos) { return static_cast<int16_t>(pos % 20000 + 1); }
float rampFloat(uint64_t pos) { return rampAt(pos) / 32768.0f; }

void writePacket(JitterBuffer& jb, uint32_t stream, uint64_t index) {
    int16_t pcm[kFpp];
    for (uint32_t i = 0; i < kFpp; ++i) pcm[i] = rampAt(index * kFpp + i);
    jb.write(stream, index * kFpp, pcm, kFpp);
}

JitterBufferConfig fixedRatio() {
    JitterBufferConfig c;
    c.driftCompensation = false;  // exact sample-for-sample output
    return c;
}

bool isRamp(const float* out, uint32_t n, uint64_t startPos) {
    for (uint32_t i = 0; i < n; ++i)
        if (out[i] != rampFloat(startPos + i)) return false;
    return true;
}

bool isSilent(const float* out, uint32_t n) {
    return std::all_of(out, out + n, [](float s) { return s == 0.0f; });
}

}  // namespace

TEST(jb_buffers_until_target) {
    JitterBuffer jb(fixedRatio());
    float out[kIo];
    for (uint64_t k = 0; k < 3; ++k) writePacket(jb, 1, k);  // 720 < 960
    jb.read(out, kIo);
    CHECK(isSilent(out, kIo));
    CHECK(!jb.stats().playing);
    writePacket(jb, 1, 3);  // 960 == target
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, 0));
    CHECK(jb.stats().playing);
}

TEST(jb_normal_sequence) {
    JitterBuffer jb(fixedRatio());
    float out[kIo];
    uint64_t written = 0, readPos = 0;
    for (; written < 5; ++written) writePacket(jb, 7, written);
    bool ok = true;
    for (int cycle = 0; cycle < 2000; ++cycle) {
        // Keep the writer ahead by ~target, like a steady stream.
        while (written * kFpp < readPos + kIo + 960) writePacket(jb, 7, written++);
        jb.read(out, kIo);
        ok = ok && isRamp(out, kIo, readPos);
        readPos += kIo;
    }
    CHECK(ok);
    auto s = jb.stats();
    CHECK(s.underruns == 0 && s.overruns == 0 && s.lateDrops == 0 && s.gapFrames == 0);
    CHECK(s.resets == 1);  // the initial anchor
}

TEST(jb_out_of_order_packets_reassembled) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k : {0, 1, 3, 2, 5, 4, 6, 7, 8}) writePacket(jb, 1, k);
    float out[2048];
    jb.read(out, 2048);
    CHECK(isRamp(out, 2048, 0));
    CHECK(jb.stats().lateDrops == 0);
}

TEST(jb_lost_packet_zero_filled) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k : {0, 1, 3, 4, 5, 6, 7, 8, 9}) writePacket(jb, 1, k);  // packet 2 lost
    float out[2048];
    jb.read(out, 2048);
    CHECK(isRamp(out, 480, 0));
    CHECK(isSilent(out + 480, 240));
    CHECK(isRamp(out + 720, 2048 - 720, 720));
    CHECK(jb.stats().gapFrames == 240);
}

TEST(jb_late_packet_after_read_dropped) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k = 0; k < 10; ++k) writePacket(jb, 1, k);
    float out[kIo];
    jb.read(out, kIo);
    jb.read(out, kIo);  // read position now 1024
    writePacket(jb, 1, 0);
    writePacket(jb, 1, 2);
    CHECK(jb.stats().lateDrops == 2);
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, 1024));
}

TEST(jb_late_packet_straddling_read_position_patched) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k : {0, 1, 2, 4, 5, 6, 7, 8}) writePacket(jb, 1, k);  // hole at 720..959
    float out[800];
    jb.read(out, 800);  // read position 800, inside the hole
    CHECK(isSilent(out + 720, 80));
    writePacket(jb, 1, 3);  // late but still partly unread
    CHECK(jb.stats().lateDrops == 0);
    float rest[400];
    jb.read(rest, 400);
    CHECK(isRamp(rest, 400, 800));
}

TEST(jb_underrun_outputs_partial_then_rebuffers) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k = 0; k < 4; ++k) writePacket(jb, 1, k);  // 960 frames
    float out[kIo];
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, 0));
    jb.read(out, kIo);  // only 448 left
    CHECK(isRamp(out, 448, 512));
    CHECK(isSilent(out + 448, kIo - 448));
    CHECK(jb.stats().underruns == 1);
    CHECK(!jb.stats().playing);

    for (uint64_t k = 4; k < 7; ++k) writePacket(jb, 1, k);  // 720 buffered < target
    jb.read(out, kIo);
    CHECK(isSilent(out, kIo));
    writePacket(jb, 1, 7);  // 960
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, 960));
    CHECK(jb.stats().underruns == 1);
}

TEST(jb_overrun_skips_to_target_depth) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k = 0; k < 20; ++k) writePacket(jb, 1, k);  // 4800 > 960 + 2880
    float out[kIo];
    jb.read(out, kIo);
    CHECK(jb.stats().overruns == 1);
    CHECK(isRamp(out, kIo, 4800 - 960));
    jb.read(out, kIo);
    CHECK(isSilent(out, kIo) == false);
    CHECK(jb.stats().overruns == 1);
}

TEST(jb_stream_id_change_resets) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k = 0; k < 10; ++k) writePacket(jb, 1, k);
    float out[kIo];
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, 0));

    // Sender restarted: new stream id, positions start over (here at an arbitrary offset).
    int16_t pcm[kFpp];
    for (uint32_t i = 0; i < kFpp; ++i) pcm[i] = 1000;
    jb.write(2, 123456, pcm, kFpp);
    jb.read(out, kIo);
    CHECK(isSilent(out, kIo));  // old data discarded, buffering again
    CHECK(jb.stats().resets == 2);

    for (uint32_t k = 1; k < 5; ++k) jb.write(2, 123456 + k * kFpp, pcm, kFpp);
    jb.read(out, kIo);
    CHECK(std::all_of(out, out + kIo, [](float s) { return s == 1000 / 32768.0f; }));
}

TEST(jb_far_ahead_packet_resets) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k = 0; k < 10; ++k) writePacket(jb, 1, k);
    float out[kIo];
    jb.read(out, kIo);
    const uint64_t jump = 48000 * 10 / kFpp;  // 10 s later, same stream
    for (uint64_t k = jump; k < jump + 5; ++k) writePacket(jb, 1, k);
    CHECK(jb.stats().resets == 2);
    CHECK(jb.stats().gapFrames == 0);  // no giant zero fill
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, jump * kFpp));
}

TEST(jb_stalled_reader_does_not_corrupt) {
    // IO stopped while packets keep arriving: writer must re-anchor instead of
    // overwriting, and a resumed reader plays recent audio via the overrun rule.
    JitterBuffer jb(fixedRatio());
    const uint64_t n = 48000 * 3 / kFpp;  // 3 s of packets, capacity is 0.5 s
    for (uint64_t k = 0; k < n; ++k) writePacket(jb, 1, k);
    CHECK(jb.stats().resets >= 2);
    float out[kIo];
    jb.read(out, kIo);
    CHECK(jb.stats().overruns == 1);
    CHECK(isRamp(out, kIo, n * kFpp - 960));  // newest data, target depth behind the head
}

TEST(jb_request_reset) {
    JitterBuffer jb(fixedRatio());
    for (uint64_t k = 0; k < 10; ++k) writePacket(jb, 1, k);
    float out[kIo];
    jb.read(out, kIo);
    jb.requestReset();
    writePacket(jb, 1, 10);
    CHECK(jb.stats().resets == 2);
    jb.read(out, kIo);
    CHECK(isSilent(out, kIo));  // re-buffering from packet 10
    for (uint64_t k = 11; k < 15; ++k) writePacket(jb, 1, k);
    jb.read(out, kIo);
    CHECK(isRamp(out, kIo, 2400));
}

TEST(jb_ignores_bad_arguments) {
    JitterBuffer jb(fixedRatio());
    jb.write(1, 0, static_cast<const int16_t*>(nullptr), 240);
    int16_t pcm[1] = {1};
    jb.write(1, 0, pcm, 0);
    std::vector<int16_t> huge(30000, 1);
    jb.write(1, 0, huge.data(), static_cast<uint32_t>(huge.size()));  // larger than capacity
    CHECK(jb.stats().packets == 0);
    jb.read(nullptr, 512);
    float out[4];
    jb.read(out, 0);
}

// ---------------------------------------------------------------------------
// Clock drift simulation: sender clock runs at (1 + drift) relative to the
// reader, packets arrive with network jitter (and occasional reordering), the
// reader pulls 512-frame IO buffers at exactly 48 kHz.

namespace {

struct DriftResult {
    double meanRatioPpm = 0;       // over the last 100 s
    double maxRatioDevPpm = 0;     // max |ratio - drift| after settleSec
    double maxErrAfterSettle = 0;  // |smoothed depth - target| after settleSec
    double settleTime = -1;        // time after which |err| stays < 48 frames (1 ms)
    uint64_t underruns = 0, overruns = 0;
    float maxStep = 0;             // largest sample-to-sample jump in the output
};

DriftResult simulateDrift(double driftPpm, double seconds, double settleSec, uint32_t seed) {
    JitterBufferConfig cfg;  // defaults: drift compensation on
    JitterBuffer jb(cfg);
    const double sr = cfg.sampleRate;
    const double senderRate = sr * (1.0 + driftPpm * 1e-6);

    // 440 Hz sine at -6 dBFS, generated in the sender's sample domain.
    auto sample = [](uint64_t pos) {
        return static_cast<int16_t>(std::lround(16384.0 * std::sin(2 * M_PI * 440.0 * pos / 48000.0)));
    };

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(0.0, 0.004);  // 0–4 ms
    const uint64_t packets = static_cast<uint64_t>(seconds * senderRate / kFpp) + 400;
    std::vector<std::pair<double, uint64_t>> arrivals(packets);
    for (uint64_t k = 0; k < packets; ++k) arrivals[k] = {(k + 1) * kFpp / senderRate + 0.001 + jitter(rng), k};
    std::sort(arrivals.begin(), arrivals.end());

    DriftResult res;
    size_t next = 0;
    float out[kIo];
    float prev = 0;
    bool havePrev = false;
    const double ioPeriod = kIo / sr;
    double lastBad = 0;
    uint64_t underrunsAtSettle = 0, overrunsAtSettle = 0;
    double ratioSum = 0;
    uint64_t ratioCount = 0;
    for (double t = 0; t < seconds; t += ioPeriod) {
        while (next < arrivals.size() && arrivals[next].first <= t) {
            uint64_t k = arrivals[next++].second;
            int16_t pcm[kFpp];
            for (uint32_t i = 0; i < kFpp; ++i) pcm[i] = sample(k * kFpp + i);
            jb.write(1, k * kFpp, pcm, kFpp);
        }
        jb.read(out, kIo);
        auto s = jb.stats();
        if (s.playing && t > 1.0) {
            for (uint32_t i = 0; i < kIo; ++i) {
                if (havePrev) res.maxStep = std::max(res.maxStep, std::fabs(out[i] - prev));
                prev = out[i];
                havePrev = true;
            }
        } else {
            havePrev = false;
        }
        double err = std::fabs(s.depthFrames - cfg.targetFrames);
        if (err >= 48 || !s.playing) lastBad = t;
        if (t >= settleSec) {
            res.maxErrAfterSettle = std::max(res.maxErrAfterSettle, err);
        } else {
            underrunsAtSettle = s.underruns;
            overrunsAtSettle = s.overruns;
        }
        if (t >= settleSec && s.playing)
            res.maxRatioDevPpm = std::max(res.maxRatioDevPpm, std::fabs(s.ratioPpm - driftPpm));
        if (t >= seconds - 100.0) {
            ratioSum += s.ratioPpm;
            ++ratioCount;
        }
    }
    res.meanRatioPpm = ratioSum / std::max<uint64_t>(1, ratioCount);
    auto s = jb.stats();
    res.underruns = s.underruns - underrunsAtSettle;
    res.overruns = s.overruns - overrunsAtSettle;
    res.settleTime = lastBad;
    return res;
}

void checkDrift(double ppm) {
    DriftResult r = simulateDrift(ppm, 300.0, 30.0, 12345);
    std::printf("  drift %+6.1f ppm: mean ratio %+7.2f ppm (max dev %.0f), settled at %.1f s, "
                "max err after 30 s %.1f frames, max step %.4f, underruns %llu, overruns %llu\n",
                ppm, r.meanRatioPpm, r.maxRatioDevPpm, r.settleTime, r.maxErrAfterSettle, r.maxStep,
                (unsigned long long)r.underruns, (unsigned long long)r.overruns);
    CHECK_MSG(std::fabs(r.meanRatioPpm - ppm) < 5.0, "ratio %.2f ppm vs drift %.1f", r.meanRatioPpm, ppm);
    CHECK_MSG(r.settleTime < 30.0, "settled only at %.1f s", r.settleTime);
    CHECK_MSG(r.maxErrAfterSettle < 48.0, "err %.1f frames", r.maxErrAfterSettle);
    // Residual wobble from depth measurement noise; 300 ppm ≈ 0.005 semitone, inaudible.
    CHECK_MSG(r.maxRatioDevPpm < 300.0, "ratio wobble %.0f ppm", r.maxRatioDevPpm);
    CHECK(r.underruns == 0);
    CHECK(r.overruns == 0);
    // 440 Hz at amplitude 0.5 moves at most ~0.029 per sample; anything much
    // bigger would be an audible click.
    CHECK_MSG(r.maxStep < 0.035f, "max step %.4f", r.maxStep);
}

}  // namespace

TEST(jb_drift_zero) { checkDrift(0.0); }
TEST(jb_drift_plus_100ppm) { checkDrift(100.0); }
TEST(jb_drift_minus_100ppm) { checkDrift(-100.0); }
TEST(jb_drift_plus_300ppm) { checkDrift(300.0); }
TEST(jb_drift_minus_300ppm) { checkDrift(-300.0); }

TEST(jb_drift_uncompensated_would_fail) {
    // Sanity check that compensation is actually needed: uncompensated, 300 ppm
    // accumulates ~86 ms in 300 s, which exceeds the 60 ms overrun margin.
    JitterBufferConfig cfg;
    cfg.driftCompensation = false;
    JitterBuffer jb(cfg);
    const double senderRate = 48000 * (1 + 300e-6);
    float out[kIo];
    uint64_t k = 0;
    for (double t = 0; t < 300.0; t += kIo / 48000.0) {
        while ((k + 1) * kFpp / senderRate + 0.002 <= t) {
            int16_t pcm[kFpp] = {};
            pcm[0] = 1;
            jb.write(1, k * kFpp, pcm, kFpp);
            ++k;
        }
        jb.read(out, kIo);
    }
    CHECK(jb.stats().overruns >= 1);
}

// ---------------------------------------------------------------------------

TEST(jb_concurrent_stress) {
    // Real threads hammering write/read with stream switches and resets. Checks
    // for crashes and garbage output; run under -fsanitize=thread for races.
    JitterBuffer jb;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0};
    std::thread writer([&] {
        std::mt19937 rng(1);
        uint32_t stream = 1;
        uint64_t k = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            int16_t pcm[kFpp];
            for (uint32_t i = 0; i < kFpp; ++i) pcm[i] = static_cast<int16_t>((k * kFpp + i) % 30000);
            uint32_t r = rng() % 1000;
            if (r == 0) ++stream;
            if (r == 1) jb.requestReset();
            uint64_t idx = (r < 20) ? k + (rng() % 8) - 4 : k;  // some reordering / duplicates
            jb.write(stream, idx * kFpp, pcm, kFpp);
            ++k;
            if (k % 4 == 0) std::this_thread::yield();
        }
    });
    std::thread reader([&] {
        float out[kIo];
        while (!stop.load(std::memory_order_relaxed)) {
            jb.read(out, kIo);
            for (float s : out)
                if (!std::isfinite(s) || s < -1.0f || s > 1.0f) bad.fetch_add(1);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop = true;
    writer.join();
    reader.join();
    CHECK(bad.load() == 0);
    auto s = jb.stats();
    std::printf("  packets %llu, resets %llu, underruns %llu, overruns %llu, late %llu\n",
                (unsigned long long)s.packets, (unsigned long long)s.resets, (unsigned long long)s.underruns,
                (unsigned long long)s.overruns, (unsigned long long)s.lateDrops);
    CHECK(s.packets > 1000);
}
