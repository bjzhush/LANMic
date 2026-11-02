#include "NetReceiver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "Log.h"

namespace netmic {
namespace {

double nowSeconds() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

std::string ipString(uint32_t ip) {
    char buf[INET_ADDRSTRLEN];
    in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntop(AF_INET, &a, buf, sizeof buf) ? buf : "?";
}

}  // namespace

NetReceiver::NetReceiver(const Config& config, JitterBuffer& buffer) : config_(config), buffer_(buffer) {
    if (!config_.senderAddress.empty()) {
        in_addr a;
        if (inet_pton(AF_INET, config_.senderAddress.c_str(), &a) == 1) {
            sender_ = {ntohl(a.s_addr), config_.port};
            haveSender_ = configuredSender_ = true;
            senderName_ = config_.senderAddress;
        }
    }
}

NetReceiver::~NetReceiver() { stop(); }

bool NetReceiver::start() {
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        NM_LOG_ERROR("network: socket() failed: %{public}s", strerror(errno));
        return false;
    }
    int on = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) != 0)
        NM_LOG_ERROR("network: SO_BROADCAST failed: %{public}s", strerror(errno));
    int rcvbuf = 1 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    sockaddr_in local = {};
    local.sin_len = sizeof local;
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        NM_LOG_ERROR("network: bind() failed: %{public}s", strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    socklen_t len = sizeof local;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &len);
    NM_LOG("network: socket ready on UDP %u; sender %{public}s", ntohs(local.sin_port),
           configuredSender_ ? (config_.senderAddress + ":" + std::to_string(config_.port)).c_str()
                             : "by broadcast discovery");
    thread_ = std::thread([this] { run(); });
    return true;
}

void NetReceiver::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

NetReceiverStats NetReceiver::stats() const {
    NetReceiverStats s;
    s.audioPackets = statAudio_.load(std::memory_order_relaxed);
    s.lostPackets = statLost_.load(std::memory_order_relaxed);
    s.reordered = statReordered_.load(std::memory_order_relaxed);
    s.ignored = statIgnored_.load(std::memory_order_relaxed);
    s.sendErrors = statSendErrors_.load(std::memory_order_relaxed);
    return s;
}

void NetReceiver::sendControl(uint8_t type, const Peer& to) {
    uint8_t pkt[NETMIC_HEADER_BYTES + NETMIC_TOKEN_BYTES];
    size_t n = netmic_packet_encode(type, controlSeq_++, 0, 0, config_.token, NETMIC_TOKEN_BYTES, pkt, sizeof pkt);
    sockaddr_in dst = {};
    dst.sin_len = sizeof dst;
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(to.ip);
    dst.sin_port = htons(to.port);
    if (sendto(fd_, pkt, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst) < 0) {
        // Logged once per error streak so a down network doesn't flood the log.
        if (statSendErrors_.fetch_add(1, std::memory_order_relaxed) % 100 == 0)
            NM_LOG_ERROR("network: sendto %{public}s:%u failed: %{public}s", ipString(to.ip).c_str(), to.port,
                         strerror(errno));
    }
}

void NetReceiver::sendDiscover() {
    // Limited broadcast only leaves through the primary interface, so also send
    // to every interface's subnet broadcast address.
    std::vector<uint32_t> targets = {INADDR_BROADCAST};
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            if (!i->ifa_broadaddr) continue;
            uint32_t b = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
            bool dup = false;
            for (uint32_t t : targets) dup |= t == b;
            if (!dup) targets.push_back(b);
        }
        freeifaddrs(list);
    }
    for (uint32_t t : targets) sendControl(NETMIC_TYPE_DISCOVER, {t, config_.port});
}

void NetReceiver::dropSender(const char* why) {
    NM_LOG("network: sender %{public}s (%{public}s:%u) %{public}s, discovering again", senderName_.c_str(),
           ipString(sender_.ip).c_str(), sender_.port, why);
    haveSender_ = false;
    haveSeq_ = false;
    buffer_.clear();
}

void NetReceiver::logStats(const char* prefix) {
    const JitterBufferStats b = buffer_.stats();
    const NetReceiverStats n = stats();
    NM_LOG("%{public}s: sender %{public}s | packets %llu lost %llu (+%llu) reordered %llu ignored %llu | "
           "buffer %{public}s depth %.1f ms (target %u ms) ratio %+.1f ppm | underruns %llu (+%llu) "
           "overruns %llu (+%llu) catch-ups %llu (+%llu) gaps %llu frames late %llu resets %llu",
           prefix, haveSender_ ? senderName_.c_str() : "<none>", n.audioPackets, n.lostPackets,
           n.lostPackets - loggedNet_.lostPackets, n.reordered, n.ignored, b.playing ? "playing" : "buffering",
           b.depthFrames * 1000.0 / NETMIC_SAMPLE_RATE, config_.targetLatencyMs, b.ratioPpm, b.underruns,
           b.underruns - loggedBuffer_.underruns, b.overruns, b.overruns - loggedBuffer_.overruns, b.catchUps,
           b.catchUps - loggedBuffer_.catchUps, b.gapFrames,
           b.lateDrops, b.resets);
    loggedNet_ = n;
    loggedBuffer_ = b;
}

// Underruns / overruns as they happen (at most one line per second), so they
// can be matched to network events in the log.
void NetReceiver::logBufferEvents(double now) {
    if (now - lastEventLog_ < 1) return;
    const JitterBufferStats b = buffer_.stats();
    if (b.underruns != seenUnderruns_ || b.overruns != seenOverruns_ || b.catchUps != seenCatchUps_) {
        NM_LOG("buffer: %llu underrun(s), %llu overrun(s), %llu catch-up(s) in the last %.0f s; depth now %.1f ms",
               b.underruns - seenUnderruns_, b.overruns - seenOverruns_, b.catchUps - seenCatchUps_,
               std::min(now - lastEventLog_, 99.0), b.depthFrames * 1000.0 / NETMIC_SAMPLE_RATE);
        seenUnderruns_ = b.underruns;
        seenOverruns_ = b.overruns;
        seenCatchUps_ = b.catchUps;
    }
    lastEventLog_ = now;
}

void NetReceiver::handle(const uint8_t* buf, size_t len, const Peer& from, double now) {
    netmic_header h;
    const uint8_t* payload = nullptr;
    if (netmic_packet_decode(buf, len, &h, &payload) != NETMIC_OK) {
        statIgnored_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool active = wasActive_;

    if (h.type == NETMIC_TYPE_ANNOUNCE) {
        if (!active || haveSender_) return;
        netmic_announce a;
        if (netmic_announce_decode(payload, h.payloadBytes, &a) != NETMIC_OK) return;
        std::string name(a.name, a.nameLen);
        if (a.sampleRate != NETMIC_SAMPLE_RATE || a.channels != 1 || a.format != NETMIC_FORMAT_INT16 ||
            a.framesPerPacket != NETMIC_FRAMES_PER_PACKET) {
            NM_LOG_ERROR("network: ignoring sender \"%{public}s\" at %{public}s: unsupported format "
                         "(%u Hz, %u ch, format %u, %u frames/packet)",
                         name.c_str(), ipString(from.ip).c_str(), a.sampleRate, a.channels, a.format,
                         a.framesPerPacket);
            return;
        }
        sender_ = from;
        senderName_ = "\"" + name + "\" " + ipString(from.ip);
        haveSender_ = true;
        haveSeq_ = false;
        lastHeard_ = now;
        NM_LOG("network: found sender %{public}s:%u, subscribing", senderName_.c_str(), from.port);
        sendControl(NETMIC_TYPE_SUBSCRIBE, sender_);
        return;
    }

    if (h.type != NETMIC_TYPE_AUDIO) return;
    if (!active || !haveSender_ || !(from == sender_)) {
        if (active) statIgnored_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t frames = h.payloadBytes / 2;
    if (frames > NETMIC_SAMPLE_RATE / 10) {  // > 100 ms in one packet: not ours
        statIgnored_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lastHeard_ = now;
    if (silenceWarned_) {
        NM_LOG("network: audio from %{public}s resumed", senderName_.c_str());
        silenceWarned_ = false;
    }

    if (!haveSeq_ || h.streamId != streamId_) {
        if (haveSeq_) NM_LOG("network: sender restarted its stream (%08x → %08x)", streamId_, h.streamId);
        haveSeq_ = true;
        streamId_ = h.streamId;
        nextSeq_ = h.seq + 1;
    } else {
        const int32_t diff = static_cast<int32_t>(h.seq - nextSeq_);
        if (diff >= 0) {
            if (diff > 0 && diff < 10000) statLost_.fetch_add(static_cast<uint64_t>(diff), std::memory_order_relaxed);
            nextSeq_ = h.seq + 1;
        } else {
            statReordered_.fetch_add(1, std::memory_order_relaxed);  // late, duplicate, or reordered
        }
    }

    int16_t pcm[NETMIC_SAMPLE_RATE / 10];
    for (uint32_t i = 0; i < frames; ++i) pcm[i] = static_cast<int16_t>(netmic_get_u16(payload + 2 * i));
    buffer_.write(h.streamId, h.samplePos, pcm, frames);
    statAudio_.fetch_add(1, std::memory_order_relaxed);
}

void NetReceiver::run() {
    pthread_setname_np("netmic.network");
    double lastDiscover = -1e9, lastSubscribe = -1e9, lastStats = nowSeconds();
    uint8_t buf[2048];

    while (!stop_.load(std::memory_order_relaxed)) {
        const double now = nowSeconds();
        const bool active = active_.load(std::memory_order_acquire);

        if (active != wasActive_) {
            wasActive_ = active;
            if (active) {
                NM_LOG("network: IO started, %{public}s", haveSender_ ? "subscribing" : "discovering sender");
                buffer_.clear();
                haveSeq_ = false;
                lastHeard_ = now;
                silenceWarned_ = false;
                lastSubscribe = -1e9;
                lastDiscover = -1e9;
                lastStats = now;
                const JitterBufferStats b = buffer_.stats();
                seenUnderruns_ = b.underruns;
                seenOverruns_ = b.overruns;
                seenCatchUps_ = b.catchUps;
                lastEventLog_ = now;
            } else {
                if (haveSender_) {
                    sendControl(NETMIC_TYPE_UNSUBSCRIBE, sender_);
                    sendControl(NETMIC_TYPE_UNSUBSCRIBE, sender_);
                }
                logStats("network: IO stopped, unsubscribed");
                buffer_.clear();
            }
        }

        if (active) {
            if (!haveSender_) {
                if (now - lastDiscover >= NETMIC_DISCOVER_INTERVAL_MS / 1000.0) {
                    sendDiscover();
                    lastDiscover = now;
                }
            } else {
                if (now - lastSubscribe >= NETMIC_SUBSCRIBE_INTERVAL_MS / 1000.0) {
                    sendControl(NETMIC_TYPE_SUBSCRIBE, sender_);
                    lastSubscribe = now;
                }
                if (now - lastHeard_ > NETMIC_SENDER_TIMEOUT_MS / 1000.0) {
                    if (!configuredSender_) {
                        dropSender("silent for 5 s");
                    } else if (!silenceWarned_) {
                        NM_LOG_ERROR("network: no audio from %{public}s:%u for 5 s, still subscribing",
                                     senderName_.c_str(), sender_.port);
                        silenceWarned_ = true;
                    }
                }
            }
            logBufferEvents(now);
            if (now - lastStats >= 10) {
                logStats("stats");
                lastStats = now;
            }
        }

        pollfd p = {fd_, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0) continue;
        // Drain everything that is queued.
        for (;;) {
            sockaddr_in from = {};
            socklen_t flen = sizeof from;
            ssize_t n = recvfrom(fd_, buf, sizeof buf, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 0) break;
            handle(buf, static_cast<size_t>(n), {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)}, nowSeconds());
        }
    }
}

}  // namespace netmic
