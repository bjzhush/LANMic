// Loads NetMic.driver in-process and drives it the way the HAL does:
// factory → Initialize → property queries → client → StartIO → IO cycles → StopIO.
// Catches crashes and wrong properties before the bundle goes near coreaudiod.
//
// Usage: netmic_plugin_smoke path/to/NetMic.driver
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "test_harness.h"

namespace {

AudioServerPlugInDriverRef gDriver = nullptr;
std::string gBundlePath;

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

TEST(smoke_io_cycle_outputs_tone) {
    CHECK(gDriver && gDevice && gStream);
    if (!gDriver || !gDevice || !gStream) return;
    AudioServerPlugInClientInfo client = {1, getpid(), true, CFSTR("com.netmic.smoke")};
    CHECK((*gDriver)->AddDeviceClient(gDriver, gDevice, &client) == 0);
    CHECK((*gDriver)->StartIO(gDriver, gDevice, 1) == 0);

    Float64 sampleTime = 0;
    UInt64 hostTime = 0, seed = 0;
    CHECK((*gDriver)->GetZeroTimeStamp(gDriver, gDevice, 1, &sampleTime, &hostTime, &seed) == 0);
    CHECK(hostTime > 0);

    Boolean willDo = false, inPlace = false;
    CHECK((*gDriver)->WillDoIOOperation(gDriver, gDevice, 1, kAudioServerPlugInIOOperationReadInput, &willDo,
                                        &inPlace) == 0);
    CHECK(willDo);

    // 100 cycles of 512 frames, contiguous sample times: the result must be one
    // continuous 440 Hz sine at -12 dBFS.
    const UInt32 frames = 512, cycles = 100;
    std::vector<float> all;
    std::vector<float> buf(frames);
    for (UInt32 i = 0; i < cycles; ++i) {
        AudioServerPlugInIOCycleInfo info = {};
        info.mIOCycleCounter = i;
        info.mNominalIOBufferFrameSize = frames;
        info.mInputTime.mSampleTime = 1000.0 + i * frames;
        info.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
        CHECK((*gDriver)->BeginIOOperation(gDriver, gDevice, 1, kAudioServerPlugInIOOperationReadInput, frames,
                                           &info) == 0);
        std::fill(buf.begin(), buf.end(), 99.0f);
        CHECK((*gDriver)->DoIOOperation(gDriver, gDevice, gStream, 1, kAudioServerPlugInIOOperationReadInput, frames,
                                        &info, buf.data(), nullptr) == 0);
        CHECK((*gDriver)->EndIOOperation(gDriver, gDevice, 1, kAudioServerPlugInIOOperationReadInput, frames,
                                         &info) == 0);
        all.insert(all.end(), buf.begin(), buf.end());
    }

    float peak = 0, maxStep = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        peak = std::max(peak, std::fabs(all[i]));
        if (i) maxStep = std::max(maxStep, std::fabs(all[i] - all[i - 1]));
    }
    const float expectedStep = 0.25f * 2 * float(M_PI) * 440 / 48000;  // max slope of the sine per sample
    CHECK_MSG(std::fabs(20 * std::log10(peak) + 12.04f) < 0.05f, "peak %f", peak);
    CHECK_MSG(maxStep <= expectedStep * 1.001f, "max step %f (expected <= %f)", maxStep, expectedStep);

    CHECK((*gDriver)->StopIO(gDriver, gDevice, 1) == 0);
    CHECK((*gDriver)->RemoveDeviceClient(gDriver, gDevice, &client) == 0);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s path/to/NetMic.driver\n", argv[0]);
        return 2;
    }
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
