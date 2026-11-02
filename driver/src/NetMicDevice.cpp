#include "NetMicDevice.h"

#include "Log.h"

namespace netmic {
namespace {

// Forwards libASPL's (verbose) operation trace to os_log at debug level.
class OsLogTracer : public aspl::Tracer {
public:
    OsLogTracer() : aspl::Tracer(Mode::Custom) {}

protected:
    void Print(const char* message) override { os_log_debug(asplLog(), "%{public}s", message); }
};

AudioStreamBasicDescription floatFormat(uint32_t channels) {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = NETMIC_SAMPLE_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = channels;
    f.mBytesPerFrame = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;
    return f;
}

}  // namespace

os_log_t driverLog() {
    static os_log_t log = os_log_create("com.netmic.driver", "driver");
    return log;
}

os_log_t asplLog() {
    static os_log_t log = os_log_create("com.netmic.driver", "aspl");
    return log;
}

NetMicHandler::NetMicHandler(const Config& config)
    : channels_(config.channels), tone_(440.0, 0.25f, NETMIC_SAMPLE_RATE) {}

OSStatus NetMicHandler::OnStartIO() {
    ioRunning_.store(true, std::memory_order_release);
    NM_LOG("IO started");
    return kAudioHardwareNoError;
}

void NetMicHandler::OnStopIO() {
    ioRunning_.store(false, std::memory_order_release);
    NM_LOG("IO stopped");
}

void NetMicHandler::OnReadClientInput(const std::shared_ptr<aspl::Client>&,
                                      const std::shared_ptr<aspl::Stream>&,
                                      Float64,
                                      Float64 timestamp,
                                      void* bytes,
                                      UInt32 bytesCount) {
    // Real-time thread: no allocation, locks, syscalls or logging.
    if (!bytes) return;
    const uint32_t frames = bytesCount / (sizeof(float) * channels_);
    tone_.fill(static_cast<float*>(bytes), frames, channels_, timestamp);
}

std::shared_ptr<aspl::Driver> createDriver(const Config& config) {
    auto context = std::make_shared<aspl::Context>(std::make_shared<OsLogTracer>());

    aspl::DeviceParameters dp;
    dp.Name = config.deviceName;
    dp.Manufacturer = kManufacturer;
    dp.DeviceUID = kDeviceUID;
    dp.ModelUID = "com.netmic.model";
    dp.SampleRate = NETMIC_SAMPLE_RATE;
    dp.ChannelCount = config.channels;
    dp.Latency = config.targetFrames();
    dp.CanBeDefault = true;
    dp.CanBeDefaultForSystemSounds = false;
    auto device = std::make_shared<aspl::Device>(context, dp);

    aspl::StreamParameters sp;
    sp.Direction = aspl::Direction::Input;
    sp.StartingChannel = 1;
    sp.Format = floatFormat(config.channels);
    device->AddStreamWithControlsAsync(sp);

    auto handler = std::make_shared<NetMicHandler>(config);
    device->SetControlHandler(handler);
    device->SetIOHandler(handler);

    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);
    return std::make_shared<aspl::Driver>(context, plugin);
}

}  // namespace netmic
