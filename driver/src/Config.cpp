#include "Config.h"

#include <arpa/inet.h>

#include <algorithm>

namespace netmic {
namespace {

bool toString(CFTypeRef v, std::string& out) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return false;
    auto s = static_cast<CFStringRef>(v);
    CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string buf(static_cast<size_t>(max), '\0');
    if (!CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8)) return false;
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    out = buf;
    return true;
}

bool toInt(CFTypeRef v, long long& out) {
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
    auto n = static_cast<CFNumberRef>(v);
    if (CFNumberIsFloatType(n)) return false;
    return CFNumberGetValue(n, kCFNumberLongLongType, &out);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

Config parseConfig(CFDictionaryRef dict) {
    Config c;
    if (!dict) return c;
    auto get = [&](const char* key) -> CFTypeRef {
        CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        CFTypeRef v = CFDictionaryGetValue(dict, k);
        CFRelease(k);
        return v;
    };
    auto warn = [&](const std::string& key, const std::string& why) {
        c.warnings.push_back(key + ": " + why + ", using default");
    };

    if (CFTypeRef v = get("DeviceName")) {
        std::string s;
        if (toString(v, s) && !s.empty()) c.deviceName = s;
        else warn("DeviceName", "not a non-empty string");
    }
    if (CFTypeRef v = get("SenderAddress")) {
        std::string s;
        in_addr addr;
        if (toString(v, s) && (s.empty() || inet_pton(AF_INET, s.c_str(), &addr) == 1)) c.senderAddress = s;
        else warn("SenderAddress", "not an IPv4 address");
    }
    if (CFTypeRef v = get("Port")) {
        long long n;
        if (toInt(v, n) && n >= 1 && n <= 65535) c.port = static_cast<uint16_t>(n);
        else warn("Port", "not an integer in 1..65535");
    }
    if (CFTypeRef v = get("TargetLatencyMs")) {
        long long n;
        if (toInt(v, n) && n >= 5 && n <= 200) c.targetLatencyMs = static_cast<uint32_t>(n);
        else warn("TargetLatencyMs", "not an integer in 5..200");
    }
    if (CFTypeRef v = get("Channels")) {
        long long n;
        if (toInt(v, n) && (n == 1 || n == 2)) c.channels = static_cast<uint32_t>(n);
        else warn("Channels", "not 1 or 2");
    }
    if (CFTypeRef v = get("Token")) {
        std::string s;
        bool ok = toString(v, s) && (s.empty() || s.size() == 2 * NETMIC_TOKEN_BYTES);
        uint8_t token[NETMIC_TOKEN_BYTES] = {};
        for (size_t i = 0; ok && i < s.size() / 2; ++i) {
            int hi = hexValue(s[2 * i]), lo = hexValue(s[2 * i + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else token[i] = static_cast<uint8_t>(hi << 4 | lo);
        }
        if (ok) std::copy(token, token + NETMIC_TOKEN_BYTES, c.token);
        else warn("Token", "not 32 hex digits");
    }
    return c;
}

Config loadConfig() {
    CFStringRef id = CFStringCreateWithCString(kCFAllocatorDefault, kBundleIdentifier, kCFStringEncodingUTF8);
    CFBundleRef bundle = CFBundleGetBundleWithIdentifier(id);
    CFRelease(id);
    if (!bundle) {
        Config c;
        c.warnings.push_back(std::string("bundle ") + kBundleIdentifier + " not found, using defaults");
        return c;
    }
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("NetMicConfig"));
    if (v && CFGetTypeID(v) != CFDictionaryGetTypeID()) {
        Config c;
        c.warnings.push_back("NetMicConfig is not a dictionary, using defaults");
        return c;
    }
    return parseConfig(static_cast<CFDictionaryRef>(v));
}

}  // namespace netmic
