// Network thread of the driver: discovery, subscription heartbeats, and
// receiving AUDIO packets into the JitterBuffer (it is the buffer's writer).
//
// It runs for the lifetime of the plugin. The IO side only flips setActive()
// (from StartIO / StopIO); all socket work happens here:
//   inactive: no traffic sent; incoming packets are drained and ignored.
//   active:   no sender yet → broadcast DISCOVER every 1 s (unless SenderAddress
//             is configured); sender known → SUBSCRIBE every 500 ms, AUDIO from
//             that sender goes into the buffer. A discovered sender that is
//             silent for 5 s is dropped and discovery restarts.
//   active → inactive: UNSUBSCRIBE (twice) and clear the buffer.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "Config.h"
#include "JitterBuffer.h"

namespace netmic {

struct NetReceiverStats {
    uint64_t audioPackets = 0;  // accepted AUDIO packets
    uint64_t lostPackets = 0;   // seq gaps (within a stream)
    uint64_t reordered = 0;     // seq older than the newest seen
    uint64_t ignored = 0;       // malformed, wrong source, or wrong format
    uint64_t sendErrors = 0;
};

class NetReceiver {
public:
    NetReceiver(const Config& config, JitterBuffer& buffer);
    ~NetReceiver();

    NetReceiver(const NetReceiver&) = delete;
    NetReceiver& operator=(const NetReceiver&) = delete;

    // Creates the socket and starts the thread. False if the socket could not be created.
    bool start();
    // Stops and joins the thread (tests; the driver never calls it).
    void stop();

    // Any thread, real-time safe.
    void setActive(bool active) { active_.store(active, std::memory_order_release); }

    NetReceiverStats stats() const;

private:
    struct Peer {
        uint32_t ip = 0;  // host order
        uint16_t port = 0;
        bool operator==(const Peer& o) const { return ip == o.ip && port == o.port; }
    };

    void run();
    void handle(const uint8_t* buf, size_t len, const Peer& from, double now);
    void sendControl(uint8_t type, const Peer& to);
    void sendDiscover();
    void dropSender(const char* why);
    void logStats(const char* prefix);

    const Config config_;
    JitterBuffer& buffer_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> active_{false};

    // Network thread only.
    bool wasActive_ = false;
    bool haveSender_ = false;
    bool configuredSender_ = false;
    Peer sender_;
    std::string senderName_;
    double lastHeard_ = 0;
    bool silenceWarned_ = false;
    bool haveSeq_ = false;
    uint32_t streamId_ = 0;
    uint32_t nextSeq_ = 0;
    uint32_t controlSeq_ = 0;

    std::atomic<uint64_t> statAudio_{0}, statLost_{0}, statReordered_{0}, statIgnored_{0}, statSendErrors_{0};
};

}  // namespace netmic
