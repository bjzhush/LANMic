// The "M4 Mic" input device: libASPL object tree and the IO handler.
#pragma once

#include <aspl/Driver.hpp>

#include <memory>

#include "Config.h"
#include "JitterBuffer.h"
#include "NetReceiver.h"
#include "SampleTimeCache.h"

namespace netmic {

// Control requests (StartIO / StopIO) run on HAL control threads and only flip
// an atomic in the receiver. OnReadClientInput runs on the real-time IO thread
// and only reads the JitterBuffer through the sample-time cache.
class NetMicHandler : public aspl::ControlRequestHandler, public aspl::IORequestHandler {
public:
    explicit NetMicHandler(const Config& config);

    OSStatus OnStartIO() override;
    void OnStopIO() override;

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp,
                           Float64 timestamp,
                           void* bytes,
                           UInt32 bytesCount) override;

private:
    const uint32_t channels_;
    JitterBuffer buffer_;
    NetReceiver receiver_;
    SampleTimeCache<JitterBuffer> cache_;
};

// Builds the whole driver (plugin → device → input stream) from `config`.
std::shared_ptr<aspl::Driver> createDriver(const Config& config);

}  // namespace netmic
