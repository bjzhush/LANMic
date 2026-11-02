// Driver configuration: the `NetMicConfig` dictionary in the bundle's Info.plist.
// The plugin runs sandboxed and can only read its own bundle, so changing the
// configuration means editing Info.plist and reinstalling.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <string>
#include <vector>

#include "netmic_protocol.h"

namespace netmic {

constexpr const char* kBundleIdentifier = "com.netmic.driver";
constexpr const char* kDeviceUID = "com.netmic.device.m4mic";
constexpr const char* kManufacturer = "NetMic";

struct Config {
    std::string deviceName = "M4 Mic";
    std::string senderAddress;  // empty: broadcast discovery; else IPv4 unicast
    uint16_t port = NETMIC_DEFAULT_PORT;
    uint32_t targetLatencyMs = 20;
    uint32_t channels = 1;                         // 1, or 2 (mono duplicated)
    uint8_t token[NETMIC_TOKEN_BYTES] = {};        // all zero: no token

    // Invalid entries fall back to the default and add a message here.
    std::vector<std::string> warnings;

    uint32_t targetFrames() const { return targetLatencyMs * NETMIC_SAMPLE_RATE / 1000; }
};

// Parses a NetMicConfig dictionary. nullptr gives the defaults.
Config parseConfig(CFDictionaryRef dict);

// Reads NetMicConfig from the bundle `kBundleIdentifier` (defaults if absent).
Config loadConfig();

}  // namespace netmic
