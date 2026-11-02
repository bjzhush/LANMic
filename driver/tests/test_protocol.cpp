// Checks shared/netmic_protocol.h against shared/test_vectors.txt (the Swift
// suite checks NetMicProtocol against the same file).
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "netmic_protocol.h"
#include "test_harness.h"

#ifndef NETMIC_TEST_VECTORS
#error "NETMIC_TEST_VECTORS must point at shared/test_vectors.txt"
#endif

namespace {

using Bytes = std::vector<uint8_t>;

Bytes hex(const std::string& s) {
    Bytes out;
    if (s == "-") return out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::vector<std::vector<std::string>> vectors(const std::string& kind) {
    std::ifstream in(NETMIC_TEST_VECTORS);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::vector<std::string> f;
        for (std::string t; ss >> t;) f.push_back(t);
        if (!f.empty() && f[0] == kind) rows.push_back(f);
    }
    return rows;
}

}  // namespace

TEST(protocol_vector_file_loaded) {
    CHECK(vectors("valid").size() >= 8);
    CHECK(vectors("invalid").size() >= 10);
    CHECK(vectors("announce").size() >= 2);
    CHECK(vectors("pcm").size() >= 1);
}

TEST(protocol_encode_matches_vectors) {
    for (auto& v : vectors("valid")) {
        Bytes payload = hex(v[6]);
        uint8_t buf[NETMIC_MAX_PACKET_BYTES];
        size_t n = netmic_packet_encode(static_cast<uint8_t>(std::stoul(v[2])), static_cast<uint32_t>(std::stoul(v[3])),
                                        static_cast<uint32_t>(std::stoul(v[4])), std::stoull(v[5]), payload.data(),
                                        payload.size(), buf, sizeof buf);
        CHECK_MSG(Bytes(buf, buf + n) == hex(v[7]), "%s", v[1].c_str());
    }
}

TEST(protocol_decode_matches_vectors) {
    for (auto& v : vectors("valid")) {
        Bytes pkt = hex(v[7]);
        netmic_header h;
        const uint8_t* payload = nullptr;
        netmic_status st = netmic_packet_decode(pkt.data(), pkt.size(), &h, &payload);
        CHECK_MSG(st == NETMIC_OK, "%s status %d", v[1].c_str(), st);
        if (st != NETMIC_OK) continue;
        CHECK(h.magic == NETMIC_MAGIC);
        CHECK(h.version == NETMIC_VERSION);
        CHECK(h.type == std::stoul(v[2]));
        CHECK(h.seq == std::stoul(v[3]));
        CHECK(h.streamId == std::stoul(v[4]));
        CHECK(h.samplePos == std::stoull(v[5]));
        CHECK(Bytes(payload, payload + h.payloadBytes) == hex(v[6]));
    }
}

TEST(protocol_invalid_packets_rejected) {
    std::map<std::string, netmic_status> errors = {{"short", NETMIC_ERR_SHORT},
                                                   {"magic", NETMIC_ERR_MAGIC},
                                                   {"version", NETMIC_ERR_VERSION},
                                                   {"type", NETMIC_ERR_TYPE},
                                                   {"length", NETMIC_ERR_LENGTH}};
    for (auto& v : vectors("invalid")) {
        Bytes pkt = hex(v[3]);
        netmic_header h;
        const uint8_t* payload = nullptr;
        netmic_status st = netmic_packet_decode(pkt.data(), pkt.size(), &h, &payload);
        CHECK_MSG(st == errors.at(v[2]), "%s: got %d", v[1].c_str(), st);
    }
}

TEST(protocol_announce_vectors) {
    for (auto& v : vectors("announce")) {
        Bytes name = hex(v[6]);
        netmic_announce a{};
        a.sampleRate = static_cast<uint32_t>(std::stoul(v[2]));
        a.channels = static_cast<uint8_t>(std::stoul(v[3]));
        a.format = static_cast<uint8_t>(std::stoul(v[4]));
        a.framesPerPacket = static_cast<uint16_t>(std::stoul(v[5]));
        a.nameLen = static_cast<uint8_t>(name.size());
        std::copy(name.begin(), name.end(), a.name);

        uint8_t buf[512];
        size_t n = netmic_announce_encode(&a, buf, sizeof buf);
        Bytes expected = hex(v[7]);
        CHECK_MSG(Bytes(buf, buf + n) == expected, "%s", v[1].c_str());

        netmic_announce d{};
        CHECK(netmic_announce_decode(expected.data(), expected.size(), &d) == NETMIC_OK);
        CHECK(d.sampleRate == a.sampleRate && d.channels == a.channels && d.format == a.format);
        CHECK(d.framesPerPacket == a.framesPerPacket && d.nameLen == a.nameLen);
        CHECK(Bytes(d.name, d.name + d.nameLen) == name);
    }
}

TEST(protocol_pcm_vectors) {
    for (auto& v : vectors("pcm")) {
        std::vector<int16_t> samples;
        std::istringstream ss(v[2]);
        for (std::string t; std::getline(ss, t, ',');) samples.push_back(static_cast<int16_t>(std::stoi(t)));
        Bytes payload = hex(v[3]);
        CHECK(payload.size() == samples.size() * 2);
        for (size_t i = 0; i < samples.size(); ++i) CHECK(netmic_pcm_sample(payload.data(), i) == samples[i]);
    }
}

TEST(protocol_encode_rejects_oversize) {
    uint8_t buf[NETMIC_MAX_PACKET_BYTES];
    std::vector<uint8_t> big(NETMIC_MAX_PAYLOAD_BYTES + 1);
    CHECK(netmic_packet_encode(NETMIC_TYPE_AUDIO, 0, 0, 0, big.data(), big.size(), buf, sizeof buf) == 0);
    CHECK(netmic_packet_encode(NETMIC_TYPE_AUDIO, 0, 0, 0, big.data(), 480, buf, 100) == 0);
    CHECK(netmic_packet_encode(NETMIC_TYPE_AUDIO, 0, 0, 0, big.data(), 480, buf, sizeof buf) == 504);
}
