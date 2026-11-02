// SampleTimeCache: one source read per device-time range, shared by all clients.
#include <vector>

#include "SampleTimeCache.h"
#include "test_harness.h"

namespace {

// Produces 0, 1, 2, ... and counts frames consumed.
struct Counter {
    float next = 0;
    uint64_t consumed = 0;
    void read(float* out, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i) out[i] = next++;
        consumed += frames;
    }
};

bool isSeq(const std::vector<float>& v, float first, uint32_t channels = 1) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != first + static_cast<float>(i / channels)) return false;
    return true;
}

}  // namespace

TEST(cache_consecutive_cycles_are_continuous) {
    Counter src;
    netmic::SampleTimeCache<Counter> cache(src);
    std::vector<float> out(512);
    for (int k = 0; k < 100; ++k) {  // wraps the 16384-frame ring several times
        cache.read(1000.0 + 512 * k, out.data(), 512, 1);
        CHECK(isSeq(out, 512.0f * k));
    }
    CHECK(src.consumed == 51200);
}

TEST(cache_multiple_clients_share_a_cycle) {
    Counter src;
    netmic::SampleTimeCache<Counter> cache(src);
    std::vector<float> a(512), b(1024);
    cache.read(0, a.data(), 512, 1);
    cache.read(0, b.data(), 512, 2);  // second client, stereo, same time
    CHECK(isSeq(a, 0));
    CHECK(isSeq(b, 0, 2));
    CHECK(src.consumed == 512);
    cache.read(512, a.data(), 512, 1);
    CHECK(isSeq(a, 512));
    CHECK(src.consumed == 1024);
}

TEST(cache_overlapping_and_partial_ranges) {
    Counter src;
    netmic::SampleTimeCache<Counter> cache(src);
    std::vector<float> out(256);
    cache.read(100, out.data(), 256, 1);  // [100, 356)
    out.resize(300);
    cache.read(200, out.data(), 300, 1);  // [200, 500): 156 cached + 144 new
    CHECK(isSeq(out, 100));
    CHECK(src.consumed == 400);
}

TEST(cache_time_jump_restarts) {
    Counter src;
    netmic::SampleTimeCache<Counter> cache(src);
    std::vector<float> out(512);
    cache.read(0, out.data(), 512, 1);
    cache.read(1e7, out.data(), 512, 1);  // far ahead: no attempt to fill the gap
    CHECK(isSeq(out, 512));
    CHECK(src.consumed == 1024);
    cache.read(0, out.data(), 512, 1);  // far behind (device restarted its clock)
    CHECK(isSeq(out, 1024));
    cache.reset();
    cache.read(5e6, out.data(), 512, 1);
    CHECK(isSeq(out, 1536));
    CHECK(src.consumed == 2048);
}

TEST(cache_oversized_request_is_silent) {
    Counter src;
    netmic::SampleTimeCache<Counter> cache(src);
    std::vector<float> out(10000, 1.0f);
    cache.read(0, out.data(), 10000, 1);
    bool silent = true;
    for (float s : out) silent &= s == 0.0f;
    CHECK(silent);
    CHECK(src.consumed == 0);
}
