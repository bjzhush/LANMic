// CFPlugIn factory: the entry point named in Info.plist (CFPlugInFactories).
#include <CoreAudio/AudioServerPlugIn.h>

#include "Config.h"
#include "Log.h"
#include "NetMicDevice.h"

namespace {

std::shared_ptr<aspl::Driver> makeDriver() {
    netmic::Config config = netmic::loadConfig();
    for (const auto& w : config.warnings) NM_LOG_ERROR("config: %{public}s", w.c_str());
    NM_LOG("loading: device \"%{public}s\", %u ch, target %u ms, sender %{public}s:%u (stage 2: 440 Hz test tone)",
           config.deviceName.c_str(), config.channels, config.targetLatencyMs,
           config.senderAddress.empty() ? "<discover>" : config.senderAddress.c_str(), config.port);
    return netmic::createDriver(config);
}

}  // namespace

extern "C" void* NetMicEntryPoint(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    static std::shared_ptr<aspl::Driver> driver = makeDriver();
    return driver->GetReference();
}
