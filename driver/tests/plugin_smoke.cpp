// Loads NetMic.driver in-process and drives it the way the HAL does:
// factory → Initialize → property queries → clients → StartIO → IO cycles → StopIO.
// Catches crashes and wrong properties before the bundle goes near coreaudiod.
//
// Usage: netmic_plugin_smoke path/to/NetMic.driver [--expect-tone]
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.h"

namespace {

AudioServerPlugInDriverRef gDriver = nullptr;
std::string gBundlePath;
bool gExpectTone = false;

// --- host interface stubs -------------------------------------------------

OSStatus hostPropertiesChanged(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress*) {
    return 0;
}
OSStatus hostCopyFromStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* out) {
    *out = nullptr;
    return kAudioHardwareUnknownPropertyError;
}
OSStatus hostWriteToStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef) { return 0; }
OSStatus hostDeleteFromStorage(AudioServerPlugInHostRef, CFStringRef) { return 0; }
OSStatus hostRequestConfigChange(AudioServerPlugInHostRef, AudioObjectID, UInt64, void*) { return 0; }

AudioServerPlugInHostInterface gHost = {hostPropertiesChanged, hostCopyFromStorage, hostWriteToStorage,
                                        hostDeleteFromStorage, hostRequestConfigChange};

// --- property helpers -----------------------------------------------------

template <typename T>
bool getProp(AudioObjectID obj, AudioObjectPropertySelector sel, T& out,
             AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress addr = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if ((*gDriver)->GetPropertyData(gDriver, obj, 0, &addr, 0, nullptr, sizeof(T), &size, &out) != 0) return false;
    return size == sizeof(T);
}

std::vector<AudioObjectID> getIDs(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if ((*gDriver)->GetPropertyDataSize(gDriver, obj, 0, &addr, 0, nullptr, &size) != 0) return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if ((*gDriver)->GetPropertyData(gDriver, obj, 0, &addr, 0, nullptr, size, &size, ids.data()) != 0) return {};
    ids.resize(size / sizeof(AudioObjectID));
    return ids;
}

std::string getString(AudioObjectID obj, AudioObjectPropertySelector sel) {
    CFStringRef s = nullptr;
    if (!getProp(obj, sel, s) || !s) return "<error>";
    char buf[256] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

AudioObjectID gDevice = 0, gStream = 0;

}  // namespace

TEST(smoke_load_and_initialize) {
    CFStringRef path = CFStringCreateWithCString(nullptr, gBundlePath.c_str(), kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, path, kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    CFRelease(path);
    CHECK(bundle != nullptr);
    if (!bundle) return;
    CHECK(CFBundleLoadExecutable(bundle));

    // Same lookup the HAL does: factory UUID from CFPlugInTypes → function name in CFPlugInFactories.
    auto types = static_cast<CFDictionaryRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFPlugInTypes")));
    auto factories =
        static_cast<CFDictionaryRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFPlugInFactories")));
    CHECK(types && factories);
    if (!types || !factories) return;
    auto list = static_cast<CFArrayRef>(CFDictionaryGetValue(types, CFSTR("443ABAB8-E7B3-491A-B985-BEB9187030DB")));
    CHECK(list && CFArrayGetCount(list) == 1);
    if (!list) return;
    auto fname = static_cast<CFStringRef>(CFDictionaryGetValue(factories, CFArrayGetValueAtIndex(list, 0)));
    CHECK(fname != nullptr);
    if (!fname) return;
    using Factory = void* (*)(CFAllocatorRef, CFUUIDRef);
    auto factory = reinterpret_cast<Factory>(CFBundleGetFunctionPointerForName(bundle, fname));
    CHECK(factory != nullptr);
    if (!factory) return;

    CHECK(factory(nullptr, CFUUIDGetConstantUUIDWithBytes(nullptr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1)) ==
          nullptr);
    gDriver = static_cast<AudioServerPlugInDriverRef>(factory(nullptr, kAudioServerPlugInTypeUUID));
    CHECK(gDriver != nullptr);
    if (!gDriver) return;
    CHECK((*gDriver)->Initialize(gDriver, &gHost) == 0);
}

TEST(smoke_device_properties) {
    CHECK(gDriver != nullptr);
    if (!gDriver) return;
    auto devices = getIDs(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal);
    CHECK_MSG(devices.size() == 1, "%zu devices", devices.size());
    if (devices.size() != 1) return;
    gDevice = devices[0];

    CHECK(getString(gDevice, kAudioObjectPropertyName) == "M4 Mic");
    CHECK(getString(gDevice, kAudioObjectPropertyManufacturer) == "NetMic");
    CHECK(getString(gDevice, kAudioDevicePropertyDeviceUID) == "com.netmic.device.m4mic");

    UInt32 u = 0;
    CHECK(getProp(gDevice, kAudioDevicePropertyTransportType, u) && u == kAudioDeviceTransportTypeVirtual);
    CHECK(getProp(gDevice, kAudioDevicePropertyDeviceIsAlive, u) && u == 1);
    CHECK(getProp(gDevice, kAudioDevicePropertyLatency, u, kAudioObjectPropertyScopeInput) && u == 960);
    Float64 rate = 0;
    CHECK(getProp(gDevice, kAudioDevicePropertyNominalSampleRate, rate) && rate == 48000);

    auto inputs = getIDs(gDevice, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
    auto outputs = getIDs(gDevice, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    CHECK_MSG(inputs.size() == 1 && outputs.empty(), "%zu input / %zu output streams", inputs.size(), outputs.size());
    if (inputs.size() != 1) return;
    gStream = inputs[0];

    AudioStreamBasicDescription f = {};
    CHECK(getProp(gStream, kAudioStreamPropertyVirtualFormat, f));
    CHECK(f.mSampleRate == 48000 && f.mFormatID == kAudioFormatLinearPCM);
    CHECK((f.mFormatFlags & kAudioFormatFlagIsFloat) && f.mBitsPerChannel == 32);
    CHECK(f.mChannelsPerFrame == 1 && f.mBytesPerFrame == 4);
    CHECK(getProp(gStream, kAudioStreamPropertyDirection, u) && u == 1);  // 1 = input
}

// Two clients read every IO cycle, paced in real time like the HAL.
//   default:        1 s; output must be valid samples, identical for both clients.
//   --expect-tone:  4 s against a local `netmic-sender --tone 440` (the bundle's
//                   NetMicConfig points at it); the last 2 s must be a continuous
//                   440 Hz sine at -12 dBFS.
TEST(smoke_io_cycles) {
    CHECK(gDriver && gDevice && gStream);
    if (!gDriver || !gDevice || !gStream) return;
    AudioServerPlugInClientInfo c1 = {1, getpid(), true, CFSTR("com.netmic.smoke.1")};
    AudioServerPlugInClientInfo c2 = {2, getpid(), true, CFSTR("com.netmic.smoke.2")};
    CHECK((*gDriver)->AddDeviceClient(gDriver, gDevice, &c1) == 0);
    CHECK((*gDriver)->AddDeviceClient(gDriver, gDevice, &c2) == 0);
    CHECK((*gDriver)->StartIO(gDriver, gDevice, 1) == 0);
    CHECK((*gDriver)->StartIO(gDriver, gDevice, 2) == 0);

    Float64 sampleTime = 0;
    UInt64 hostTime = 0, seed = 0;
    CHECK((*gDriver)->GetZeroTimeStamp(gDriver, gDevice, 1, &sampleTime, &hostTime, &seed) == 0);
    CHECK(hostTime > 0);
    Boolean willDo = false, inPlace = false;
    CHECK((*gDriver)->WillDoIOOperation(gDriver, gDevice, 1, kAudioServerPlugInIOOperationReadInput, &willDo,
                                        &inPlace) == 0);
    CHECK(willDo);

    const UInt32 frames = 512;
    const double seconds = gExpectTone ? 4.0 : 1.0;
    const UInt32 cycles = static_cast<UInt32>(seconds * 48000 / frames);
    std::vector<float> all, a(frames), b(frames);
    bool clientsAgree = true, valid = true;
    auto next = std::chrono::steady_clock::now();
    for (UInt32 i = 0; i < cycles; ++i) {
        next += std::chrono::microseconds(frames * 1000000ull / 48000);
        std::this_thread::sleep_until(next);
        AudioServerPlugInIOCycleInfo info = {};
        info.mIOCycleCounter = i;
        info.mNominalIOBufferFrameSize = frames;
        info.mInputTime.mSampleTime = 1000.0 + double(i) * frames;
        info.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
        std::fill(a.begin(), a.end(), 99.0f);
        std::fill(b.begin(), b.end(), 99.0f);
        for (UInt32 client : {1u, 2u}) {
            float* buf = client == 1 ? a.data() : b.data();
            CHECK((*gDriver)->BeginIOOperation(gDriver, gDevice, client, kAudioServerPlugInIOOperationReadInput,
                                               frames, &info) == 0);
            CHECK((*gDriver)->DoIOOperation(gDriver, gDevice, gStream, client, kAudioServerPlugInIOOperationReadInput,
                                            frames, &info, buf, nullptr) == 0);
            CHECK((*gDriver)->EndIOOperation(gDriver, gDevice, client, kAudioServerPlugInIOOperationReadInput, frames,
                                             &info) == 0);
        }
        clientsAgree &= a == b;
        for (float s : a) valid &= std::isfinite(s) && s >= -1.0f && s <= 1.0f;
        all.insert(all.end(), a.begin(), a.end());
    }
    CHECK(clientsAgree);
    CHECK(valid);

    if (gExpectTone) {
        const size_t n = 2 * 48000;  // last 2 s
        float peak = 0, maxStep = 0;
        int crossings = 0;
        for (size_t i = all.size() - n; i < all.size(); ++i) {
            peak = std::max(peak, std::fabs(all[i]));
            maxStep = std::max(maxStep, std::fabs(all[i] - all[i - 1]));
            if (all[i - 1] < 0 && all[i] >= 0) ++crossings;
        }
        const float expectedStep = 0.25f * 2 * float(M_PI) * 440 / 48000;  // max slope per sample
        std::printf("  tone: peak %.2f dBFS, %d cycles in 2 s, max step %.5f (sine max %.5f)\n",
                    20 * std::log10(peak), crossings, maxStep, expectedStep);
        CHECK_MSG(std::fabs(20 * std::log10(peak) + 12.04f) < 0.1f, "peak %f", peak);
        CHECK_MSG(crossings >= 878 && crossings <= 882, "%d upward zero crossings", crossings);
        CHECK_MSG(maxStep <= expectedStep * 1.01f, "max step %f: discontinuity", maxStep);
    }

    CHECK((*gDriver)->StopIO(gDriver, gDevice, 2) == 0);
    CHECK((*gDriver)->StopIO(gDriver, gDevice, 1) == 0);
    CHECK((*gDriver)->RemoveDeviceClient(gDriver, gDevice, &c2) == 0);
    CHECK((*gDriver)->RemoveDeviceClient(gDriver, gDevice, &c1) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let UNSUBSCRIBE go out
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--expect-tone") != 0)) {
        std::fprintf(stderr, "usage: %s path/to/NetMic.driver [--expect-tone]\n", argv[0]);
        return 2;
    }
    gExpectTone = argc == 3;
    char resolved[PATH_MAX];
    if (!realpath(argv[1], resolved)) {
        std::perror(argv[1]);
        return 2;
    }
    gBundlePath = resolved;
    int failed = 0, run = 0;
    for (auto& c : nmtest::registry()) {
        int before = nmtest::failures();
        c.fn();
        bool ok = nmtest::failures() == before;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
        ++run;
        failed += !ok;
    }
    std::printf("\n%d smoke tests, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
