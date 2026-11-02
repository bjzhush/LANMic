// The "M4 Mic" input device: libASPL object tree and the IO handler.
#pragma once

#include <aspl/Driver.hpp>

#include <atomic>
#include <memory>

#include "Config.h"
#include "ToneGenerator.h"

namespace netmic {

// Control requests run on HAL control threads; OnReadClientInput runs on the
// real-time IO thread and only touches the tone generator and atomics.
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

    bool ioRunning() const { return ioRunning_.load(std::memory_order_acquire); }

private:
    const uint32_t channels_;
    ToneGenerator tone_;
    std::atomic<bool> ioRunning_{false};
};

// Builds the whole driver (plugin → device → input stream) from `config`.
std::shared_ptr<aspl::Driver> createDriver(const Config& config);

}  // namespace netmic
