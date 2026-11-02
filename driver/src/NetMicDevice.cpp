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

static JitterBufferConfig bufferConfig(const Config& config) {
    JitterBufferConfig c;
    c.targetFrames = config.targetFrames();
    c.driftCompensation = false;  // stage 4
    return c;
}

NetMicHandler::NetMicHandler(const Config& config)
    : channels_(config.channels), buffer_(bufferConfig(config)), receiver_(config, buffer_), cache_(buffer_) {
    receiver_.start();  // runs for the plugin's lifetime
}

OSStatus NetMicHandler::OnStartIO() {
    // IO is stopped here (libASPL holds the IO lock), so the cache can be reset.
    cache_.reset();
    receiver_.setActive(true);
    return kAudioHardwareNoError;
}

void NetMicHandler::OnStopIO() { receiver_.setActive(false); }

void NetMicHandler::OnReadClientInput(const std::shared_ptr<aspl::Client>&,
                                      const std::shared_ptr<aspl::Stream>&,
                                      Float64,
                                      Float64 timestamp,
                                      void* bytes,
                                      UInt32 bytesCount) {
    // Real-time thread: no allocation, locks, syscalls or logging.
    if (!bytes) return;
    const uint32_t frames = bytesCount / (sizeof(float) * channels_);
    cache_.read(timestamp, static_cast<float*>(bytes), frames, channels_);
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
