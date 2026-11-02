// NetReceiver against an in-process fake sender over UDP loopback.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "JitterBuffer.h"
#include "NetReceiver.h"
#include "test_harness.h"

using namespace netmic;
using Clock = std::chrono::steady_clock;

namespace {

struct Received {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    sockaddr_in from = {};
};

// Plays the M4 side: answers on its own UDP port.
class FakeSender {
public:
    FakeSender() {
        fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a = {};
        a.sin_len = sizeof a;
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
    }
    ~FakeSender() { close(fd_); }
    uint16_t port() const { return port_; }

    // Next packet of `type` within `ms`, skipping others.
    bool expect(uint8_t type, int ms, Received* out = nullptr) {
        auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < deadline) {
            int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            pollfd p = {fd_, POLLIN, 0};
            if (poll(&p, 1, std::max(left, 1)) <= 0) continue;
            uint8_t buf[2048];
            Received r;
            socklen_t len = sizeof r.from;
            ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&r.from), &len);
            netmic_header h;
            const uint8_t* payload;
            if (n <= 0 || netmic_packet_decode(buf, static_cast<size_t>(n), &h, &payload) != NETMIC_OK) continue;
            if (h.type != type) continue;
            r.type = h.type;
            r.payload.assign(payload, payload + h.payloadBytes);
            if (out) *out = r;
            return true;
        }
        return false;
    }

    // Counts packets of `type` arriving during `ms`.
    int count(uint8_t type, int ms) {
        int n = 0;
        auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < deadline) {
            int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (left > 0 && expect(type, left)) ++n;
        }
        return n;
    }

    void announce(const sockaddr_in& to, uint32_t rate = 48000, uint8_t channels = 1) {
        netmic_announce a = {};
        a.sampleRate = rate;
        a.channels = channels;
        a.format = NETMIC_FORMAT_INT16;
        a.framesPerPacket = NETMIC_FRAMES_PER_PACKET;
        a.nameLen = 4;
        std::memcpy(a.name, "Fake", 4);
        uint8_t payload[64];
        size_t pn = netmic_announce_encode(&a, payload, sizeof payload);
        send(NETMIC_TYPE_ANNOUNCE, 0, 0, 0, payload, pn, to);
    }

    // AUDIO packet k of a ramp stream (sample value = position % 30000).
    void audio(const sockaddr_in& to, uint32_t streamId, uint32_t k, int fd = -1) {
        uint8_t payload[NETMIC_FRAMES_PER_PACKET * 2];
        for (uint32_t i = 0; i < NETMIC_FRAMES_PER_PACKET; ++i)
            netmic_put_u16(payload + 2 * i, static_cast<uint16_t>((k * NETMIC_FRAMES_PER_PACKET + i) % 30000));
        send(NETMIC_TYPE_AUDIO, k, streamId, uint64_t(k) * NETMIC_FRAMES_PER_PACKET, payload, sizeof payload, to, fd);
    }

private:
    void send(uint8_t type, uint32_t seq, uint32_t streamId, uint64_t pos, const uint8_t* payload, size_t len,
              const sockaddr_in& to, int fd = -1) {
        uint8_t pkt[1500];
        size_t n = netmic_packet_encode(type, seq, streamId, pos, payload, len, pkt, sizeof pkt);
        sendto(fd >= 0 ? fd : fd_, pkt, n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    int fd_ = -1;
    uint16_t port_ = 0;
};

Config loopbackConfig(uint16_t port) {
    Config c;
    c.senderAddress = "127.0.0.1";
    c.port = port;
    return c;
}

JitterBufferConfig bufferConfig() {
    JitterBufferConfig c;
    c.driftCompensation = false;
    return c;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST(net_inactive_sends_nothing) {
    FakeSender fake;
    JitterBuffer jb(bufferConfig());
    NetReceiver rx(loopbackConfig(fake.port()), jb);
    CHECK(rx.start());
    CHECK(fake.count(NETMIC_TYPE_SUBSCRIBE, 700) == 0);
}

TEST(net_configured_sender_subscribe_audio_unsubscribe) {
    FakeSender fake;
    JitterBuffer jb(bufferConfig());
    Config cfg = loopbackConfig(fake.port());
    for (int i = 0; i < 16; ++i) cfg.token[i] = static_cast<uint8_t>(0xA0 + i);
    NetReceiver rx(cfg, jb);
    CHECK(rx.start());
    rx.setActive(true);

    Received sub;
    CHECK(fake.expect(NETMIC_TYPE_SUBSCRIBE, 300, &sub));
    CHECK(sub.payload.size() == 16 && std::memcmp(sub.payload.data(), cfg.token, 16) == 0);
    // Heartbeat every 500 ms.
    int n = fake.count(NETMIC_TYPE_SUBSCRIBE, 1600);
    CHECK_MSG(n >= 3 && n <= 4, "%d SUBSCRIBEs in 1.6 s", n);

    // 8 packets = 40 ms: above the 20 ms target, below the overrun threshold.
    for (uint32_t k = 0; k < 8; ++k) fake.audio(sub.from, 0x1234, k);
    sleepMs(150);
    CHECK_MSG(rx.stats().audioPackets == 8, "%llu packets", (unsigned long long)rx.stats().audioPackets);
    CHECK(jb.stats().packets == 8);
    float out[480];
    jb.read(out, 480);
    bool ramp = true;
    for (int i = 0; i < 480; ++i) ramp &= out[i] == static_cast<float>(i) / 32768.0f;
    CHECK(ramp);

    // A gap of 3 packets is counted as lost.
    fake.audio(sub.from, 0x1234, 11);
    sleepMs(100);
    CHECK(rx.stats().lostPackets == 3);

    rx.setActive(false);
    CHECK(fake.expect(NETMIC_TYPE_UNSUBSCRIBE, 300));
    sleepMs(100);
    jb.read(out, 480);  // buffer was cleared: silence, back to buffering
    bool silent = true;
    for (float v : out) silent &= v == 0.0f;
    CHECK(silent);
    CHECK(!jb.stats().playing);
    CHECK(fake.count(NETMIC_TYPE_SUBSCRIBE, 700) == 0);
}

TEST(net_ignores_audio_from_other_sources_and_when_inactive) {
    FakeSender fake, other;
    JitterBuffer jb(bufferConfig());
    NetReceiver rx(loopbackConfig(fake.port()), jb);
    CHECK(rx.start());
    rx.setActive(true);
    Received sub;
    CHECK(fake.expect(NETMIC_TYPE_SUBSCRIBE, 300, &sub));
    for (uint32_t k = 0; k < 5; ++k) other.audio(sub.from, 1, k);  // wrong source port
    sleepMs(100);
    CHECK(rx.stats().audioPackets == 0);
    CHECK(rx.stats().ignored == 5);

    rx.setActive(false);
    sleepMs(100);
    for (uint32_t k = 0; k < 5; ++k) fake.audio(sub.from, 1, k);
    sleepMs(100);
    CHECK(rx.stats().audioPackets == 0);
    CHECK(jb.stats().packets == 0);
}

TEST(net_discovery_announce_subscribe_and_timeout) {
    FakeSender fake;
    JitterBuffer jb(bufferConfig());
    Config cfg;  // no SenderAddress: broadcast discovery to fake's port
    cfg.port = fake.port();
    NetReceiver rx(cfg, jb);
    CHECK(rx.start());
    rx.setActive(true);

    Received disc;
    CHECK(fake.expect(NETMIC_TYPE_DISCOVER, 500, &disc));
    // Unsupported format is ignored: discovery continues.
    fake.announce(disc.from, 44100);
    CHECK(fake.expect(NETMIC_TYPE_DISCOVER, 1500, &disc));
    CHECK(!fake.expect(NETMIC_TYPE_SUBSCRIBE, 100));

    fake.announce(disc.from);
    Received sub;
    CHECK(fake.expect(NETMIC_TYPE_SUBSCRIBE, 300, &sub));
    for (uint32_t k = 0; k < 10; ++k) fake.audio(sub.from, 7, k);
    sleepMs(100);
    CHECK(rx.stats().audioPackets == 10);

    // Silent for > 5 s: dropped, discovery starts again.
    CHECK(fake.expect(NETMIC_TYPE_DISCOVER, 6500));
}
