// Finds dropouts in a recording of "M4 Mic". The driver outputs exact digital
// silence when it has no data (underrun, buffering, sender gone), while a real
// microphone always has some noise. So every run of 5 ms windows whose samples
// are all exactly zero is a moment the driver had nothing to play. (Very quiet
// passages after AAC encoding can reach ±1 LSB, so a level threshold is not used.)
// Record with QuickTime quality "Maximum" (Apple Lossless) to keep short gaps exact.
//
//   afconvert -f WAVE -d LEI16@48000 -c 1 long.m4a long.wav
//   netmic_find_gaps long.wav [start HH:MM:SS]   (wall-clock time of the first sample, for the report)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool readWav(const char* path, std::vector<int16_t>& out, uint32_t& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char riff[12];
    if (std::fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4)) {
        std::fclose(f);
        return false;
    }
    uint16_t channels = 0, bits = 0;
    for (;;) {
        char id[4];
        uint32_t size;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&size, 4, 1, f) != 1) break;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> fmt(size);
            if (std::fread(fmt.data(), 1, size, f) != size) break;
            std::memcpy(&channels, &fmt[2], 2);
            std::memcpy(&rate, &fmt[4], 4);
            std::memcpy(&bits, &fmt[14], 2);
        } else if (!std::memcmp(id, "data", 4)) {
            if (channels != 1 || bits != 16) {
                std::fprintf(stderr, "need mono 16-bit PCM (got %u ch, %u bits)\n", channels, bits);
                break;
            }
            out.resize(size / 2);
            const size_t n = std::fread(out.data(), 2, out.size(), f);
            out.resize(n);
            std::fclose(f);
            return true;
        } else {
            std::fseek(f, size + (size & 1), SEEK_CUR);
        }
    }
    std::fclose(f);
    return false;
}

double db(double rms) { return rms > 0 ? 20 * std::log10(rms) : -150; }

void clock(double t, int startSec, char* buf) {
    if (startSec < 0) {
        std::snprintf(buf, 32, "%02d:%05.2f", int(t) / 60, std::fmod(t, 60.0));
        return;
    }
    const double w = startSec + t;
    std::snprintf(buf, 32, "%02d:%02d:%05.2f", int(w / 3600) % 24, int(w / 60) % 60, std::fmod(w, 60.0));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s file.wav [start HH:MM:SS]\n", argv[0]);
        return 2;
    }
    int startSec = -1;
    if (argc > 2) {
        int h, m, s;
        if (std::sscanf(argv[2], "%d:%d:%d", &h, &m, &s) == 3) startSec = h * 3600 + m * 60 + s;
    }
    std::vector<int16_t> x;
    uint32_t rate = 0;
    if (!readWav(argv[1], x, rate) || rate == 0) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const size_t win = rate / 200;  // 5 ms
    const size_t nw = x.size() / win;
    std::vector<double> level(nw);
    std::vector<bool> zero(nw);
    for (size_t w = 0; w < nw; ++w) {
        double e = 0;
        bool z = true;
        for (size_t i = 0; i < win; ++i) {
            const double v = x[w * win + i] / 32768.0;
            e += v * v;
            z = z && x[w * win + i] == 0;
        }
        level[w] = db(std::sqrt(e / win));
        zero[w] = z;
    }
    std::printf("%s: %.1f min, %u Hz\n", argv[1], x.size() / double(rate) / 60, rate);

    // Per-minute summary: median level (noise floor when quiet) and loudest window.
    std::printf("\nper minute: median / max level (dBFS), digital-silence windows\n");
    for (size_t m = 0; m * 12000 < nw; ++m) {
        std::vector<double> v(level.begin() + m * 12000, level.begin() + std::min(nw, (m + 1) * 12000));
        size_t silent = 0;
        for (size_t w = m * 12000; w < std::min(nw, (m + 1) * 12000); ++w) silent += zero[w];
        std::vector<double> s = v;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        double mx = -150;
        for (double l : v) mx = std::max(mx, l);
        char t[32];
        clock(m * 60.0, startSec, t);
        std::printf("  %s  %6.1f / %6.1f  %5zu%s\n", t, s[s.size() / 2], mx, silent, silent ? "  <--" : "");
    }

    // Runs of digital silence.
    std::printf("\ndigital-silence runs (driver had no data):\n");
    size_t runs = 0;
    double total = 0;
    for (size_t w = 0; w < nw;) {
        if (!zero[w]) {
            ++w;
            continue;
        }
        size_t e = w;
        while (e < nw && zero[e]) ++e;
        const double t0 = w * 0.005, dur = (e - w) * 0.005;
        char t[32];
        clock(t0, startSec, t);
        std::printf("  %s  %8.3f s\n", t, dur);
        ++runs;
        total += dur;
        w = e;
    }
    std::printf("%zu run(s), %.2f s in total\n", runs, total);
    return 0;
}
