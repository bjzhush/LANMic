// Config parsing: defaults, valid values, and per-key fallback on bad values.
#include <CoreFoundation/CoreFoundation.h>

#include <cstring>

#include "Config.h"
#include "test_harness.h"

using netmic::Config;
using netmic::parseConfig;

namespace {

// Small owning builder for a CFMutableDictionary.
struct Dict {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    ~Dict() { CFRelease(d); }
    void set(const char* key, CFTypeRef value) {
        CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        CFDictionarySetValue(d, k, value);
        CFRelease(k);
        CFRelease(value);
    }
    void str(const char* key, const char* v) {
        set(key, CFStringCreateWithCString(kCFAllocatorDefault, v, kCFStringEncodingUTF8));
    }
    void num(const char* key, long long v) { set(key, CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &v)); }
    void real(const char* key, double v) { set(key, CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &v)); }
};

bool zeroToken(const Config& c) {
    for (uint8_t b : c.token)
        if (b) return false;
    return true;
}

}  // namespace

TEST(config_defaults) {
    Config c = parseConfig(nullptr);
    CHECK(c.deviceName == "M4 Mic");
    CHECK(c.senderAddress.empty());
    CHECK(c.port == 47810);
    CHECK(c.targetLatencyMs == 20);
    CHECK(c.targetFrames() == 960);
    CHECK(c.channels == 1);
    CHECK(zeroToken(c));
    CHECK(c.warnings.empty());

    Dict empty;
    CHECK(parseConfig(empty.d).warnings.empty());
}

TEST(config_valid_values) {
    Dict d;
    d.str("DeviceName", "Studio Mic ✓");
    d.str("SenderAddress", "127.0.0.1");
    d.num("Port", 50000);
    d.num("TargetLatencyMs", 40);
    d.num("Channels", 2);
    d.str("Token", "00112233445566778899aabbccddEEFF");
    Config c = parseConfig(d.d);
    CHECK(c.warnings.empty());
    CHECK(c.deviceName == "Studio Mic ✓");
    CHECK(c.senderAddress == "127.0.0.1");
    CHECK(c.port == 50000);
    CHECK(c.targetFrames() == 1920);
    CHECK(c.channels == 2);
    const uint8_t want[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                              0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    CHECK(std::memcmp(c.token, want, 16) == 0);
}

TEST(config_invalid_values_fall_back) {
    Dict d;
    d.str("DeviceName", "");
    d.str("SenderAddress", "m4.local");
    d.num("Port", 70000);
    d.num("TargetLatencyMs", 1);
    d.num("Channels", 3);
    d.str("Token", "xyz");
    Config c = parseConfig(d.d);
    CHECK_MSG(c.warnings.size() == 6, "%zu warnings", c.warnings.size());
    CHECK(c.deviceName == "M4 Mic");
    CHECK(c.senderAddress.empty());
    CHECK(c.port == 47810);
    CHECK(c.targetLatencyMs == 20);
    CHECK(c.channels == 1);
    CHECK(zeroToken(c));
}

TEST(config_wrong_types_fall_back) {
    Dict d;
    d.num("DeviceName", 5);
    d.str("Port", "47810");
    d.real("TargetLatencyMs", 20.5);
    d.str("Token", "0011223344556677889900aabbccddeeZZ");  // 34 chars
    Config c = parseConfig(d.d);
    CHECK_MSG(c.warnings.size() == 4, "%zu warnings", c.warnings.size());
    CHECK(c.deviceName == "M4 Mic");
    CHECK(c.port == 47810);
    CHECK(c.targetLatencyMs == 20);
    CHECK(zeroToken(c));
}

TEST(config_bad_hex_token_keeps_zero) {
    Dict d;
    d.str("Token", "00112233445566778899aabbccddeefg");  // 32 chars, last not hex
    Config c = parseConfig(d.d);
    CHECK(c.warnings.size() == 1);
    CHECK(zeroToken(c));
}
