// Replays a packet arrival trace (netmic-probe --trace) through the real
// JitterBuffer with a HAL-like reader, to choose the buffer target from data.
//
//   netmic_replay trace.csv [--io-frames 512] [--targets 15,20,25,30,35,40] [--drift on|off|both]
//
// The reader takes `io-frames` every io-frames/48000 s of M2 host time (the
// clock libASPL's device runs on — the same clock the trace's arrival times
// use). Each target is run at 16 reader phases; underruns/overruns are summed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "JitterBuffer.h"

using netmic::JitterBuffer;
using netmic::JitterBufferConfig;

namespace {

struct Packet {
    uint32_t seq, streamId;
    uint64_t samplePos;
    double arrival;
};

std::vector<Packet> load(const char* path) {
    std::ifstream in(path);
    std::vector<Packet> v;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        Packet p;
        unsigned long long pos;
        if (std::sscanf(line.c_str(), "%u,%u,%llu,%lf", &p.seq, &p.streamId, &pos, &p.arrival) == 4) {
            p.samplePos = pos;
            v.push_back(p);
        }
    }
    std::sort(v.begin(), v.end(), [](const Packet& a, const Packet& b) { return a.arrival < b.arrival; });
    return v;
}

// Lateness of each packet relative to the earliest-possible arrival, with the
// sender/receiver clock drift removed by a least-squares fit (per stream).
void lateness(const std::vector<Packet>& pk) {
    std::vector<double> late;
    size_t i = 0;
    double driftPpm = 0;
    while (i < pk.size()) {
        size_t j = i;
        while (j < pk.size() && pk[j].streamId == pk[i].streamId) ++j;
        const size_t n = j - i;
        if (n > 100) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t k = i; k < j; ++k) {
                double x = pk[k].samplePos / 48000.0, y = pk[k].arrival;
                sx += x, sy += y, sxx += x * x, sxy += x * y;
            }
            double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
            driftPpm = (1 / slope - 1) * 1e6;
            std::vector<double> t;
            for (size_t k = i; k < j; ++k) t.push_back(pk[k].arrival - slope * pk[k].samplePos / 48000.0);
            double mn = *std::min_element(t.begin(), t.end());
            for (double x : t) late.push_back((x - mn) * 1000);
        }
        i = j;
    }
    if (late.empty()) return;
    std::sort(late.begin(), late.end());
    auto pct = [&](double p) { return late[std::min(late.size() - 1, size_t(p * late.size()))]; };
    std::printf("packets %zu, span %.1f s, drift %+.2f ppm\n", pk.size(), pk.back().arrival - pk.front().arrival,
                driftPpm);
    std::printf("lateness vs earliest (ms): p50 %.2f  p90 %.2f  p99 %.2f  p99.9 %.2f  p99.99 %.2f  max %.2f\n",
                pct(0.5), pct(0.9), pct(0.99), pct(0.999), pct(0.9999), late.back());
    int hist[12] = {};
    for (double x : late) hist[std::min(11, int(x / 2.5))]++;
    std::printf("histogram (2.5 ms bins, last = 27.5+):");
    for (int b = 0; b < 12; ++b) std::printf(" %d", hist[b]);
    std::printf("\n");
}

struct Result {
    uint64_t underruns = 0, overruns = 0;
    double minDepth = 1e9, maxDepth = 0, sumDepth = 0, sumAbsPpm = 0;
    uint64_t reads = 0;
};

Result run(const std::vector<Packet>& pk, uint32_t targetFrames, uint32_t ioFrames, bool drift, double phase) {
    JitterBufferConfig cfg;
    cfg.targetFrames = targetFrames;
    cfg.driftCompensation = drift;
    JitterBuffer jb(cfg);
    Result r;
    const double period = ioFrames / 48000.0;
    std::vector<float> out(ioFrames);
    int16_t pcm[240] = {};
    size_t next = 0;
    const double t0 = pk.front().arrival + phase * period;
    for (uint64_t k = 0;; ++k) {
        const double t = t0 + k * period;
        if (t > pk.back().arrival) break;
        while (next < pk.size() && pk[next].arrival <= t) {
            jb.write(pk[next].streamId, pk[next].samplePos, pcm, 240);
            ++next;
        }
        jb.read(out.data(), ioFrames);
        auto s = jb.stats();
        if (s.playing && k > 200) {  // skip the first ~2 s (start-up)
            r.minDepth = std::min(r.minDepth, s.depthFrames);
            r.maxDepth = std::max(r.maxDepth, s.depthFrames);
            r.sumDepth += s.depthFrames;
            r.sumAbsPpm += std::fabs(s.ratioPpm);
            ++r.reads;
        }
    }
    auto s = jb.stats();
    r.underruns = s.underruns;
    r.overruns = s.overruns;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s trace.csv [--io-frames 512] [--targets 15,20,...] [--drift on|off|both]\n",
                     argv[0]);
        return 2;
    }
    uint32_t ioFrames = 512;
    std::vector<int> targets = {15, 20, 25, 30, 35, 40};
    std::vector<bool> drifts = {false, true};
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--io-frames")) ioFrames = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--targets")) {
            targets.clear();
            std::stringstream ss(argv[i + 1]);
            std::string t;
            while (std::getline(ss, t, ',')) targets.push_back(std::atoi(t.c_str()));
        } else if (!std::strcmp(argv[i], "--drift")) {
            std::string d = argv[i + 1];
            drifts = d == "on" ? std::vector<bool>{true} : d == "off" ? std::vector<bool>{false} : drifts;
        }
    }
    auto pk = load(argv[1]);
    if (pk.size() < 1000) {
        std::fprintf(stderr, "trace too short (%zu packets)\n", pk.size());
        return 1;
    }
    lateness(pk);
    std::printf("\nreader: %u frames every %.2f ms; 16 phases per row\n", ioFrames, ioFrames / 48.0);
    std::printf("target  drift  underruns  overruns  depth min/mean/max (ms)  mean |ratio| ppm\n");
    for (bool d : drifts) {
        for (int t : targets) {
            Result total;
            total.minDepth = 1e9;
            for (int ph = 0; ph < 16; ++ph) {
                Result r = run(pk, t * 48, ioFrames, d, ph / 16.0);
                total.underruns += r.underruns;
                total.overruns += r.overruns;
                total.minDepth = std::min(total.minDepth, r.minDepth);
                total.maxDepth = std::max(total.maxDepth, r.maxDepth);
                total.sumDepth += r.sumDepth;
                total.sumAbsPpm += r.sumAbsPpm;
                total.reads += r.reads;
            }
            std::printf("%4d ms  %-5s  %9llu  %8llu  %6.1f / %5.1f / %5.1f     %8.1f\n", t, d ? "on" : "off",
                        (unsigned long long)total.underruns, (unsigned long long)total.overruns,
                        total.minDepth / 48, total.sumDepth / total.reads / 48, total.maxDepth / 48,
                        total.sumAbsPpm / total.reads);
        }
    }
    return 0;
}
