/*
 * NetMic wire protocol v1 — shared definitions.
 *
 * Used by the driver (C++) directly. The Swift sender/probe re-implement the
 * same format in the NetMicProtocol module; both are checked against
 * shared/test_vectors.txt.
 *
 * All multi-byte fields are little-endian on the wire. The encode/decode
 * helpers below do explicit byte shuffling, so they are correct regardless of
 * host endianness and never perform unaligned loads.
 *
 * All helpers are allocation-free and do no I/O.
 */
#ifndef NETMIC_PROTOCOL_H
#define NETMIC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETMIC_MAGIC 0x43494D4Eu /* bytes on the wire: 'N' 'M' 'I' 'C' */
#define NETMIC_VERSION 1u
#define NETMIC_DEFAULT_PORT 47810u

#define NETMIC_HEADER_BYTES 24u
#define NETMIC_TOKEN_BYTES 16u
#define NETMIC_MAX_PACKET_BYTES 1500u
#define NETMIC_MAX_PAYLOAD_BYTES (NETMIC_MAX_PACKET_BYTES - NETMIC_HEADER_BYTES)

#define NETMIC_SAMPLE_RATE 48000u
#define NETMIC_FRAMES_PER_PACKET 240u /* 5 ms @ 48 kHz */
#define NETMIC_FORMAT_INT16 1u

/* ANNOUNCE payload: fixed part is sampleRate(4) channels(1) format(1) framesPerPacket(2) nameLen(1). */
#define NETMIC_ANNOUNCE_FIXED_BYTES 9u
#define NETMIC_MAX_NAME_BYTES 255u

#define NETMIC_SUBSCRIBE_INTERVAL_MS 500u
#define NETMIC_SUBSCRIBER_TIMEOUT_MS 3000u
#define NETMIC_DISCOVER_INTERVAL_MS 1000u
#define NETMIC_SENDER_TIMEOUT_MS 5000u

typedef enum {
    NETMIC_TYPE_DISCOVER = 1,
    NETMIC_TYPE_ANNOUNCE = 2,
    NETMIC_TYPE_SUBSCRIBE = 3,
    NETMIC_TYPE_UNSUBSCRIBE = 4,
    NETMIC_TYPE_AUDIO = 5,
} netmic_packet_type;

typedef enum {
    NETMIC_OK = 0,
    NETMIC_ERR_SHORT = 1,   /* buffer smaller than a header / declared payload */
    NETMIC_ERR_MAGIC = 2,
    NETMIC_ERR_VERSION = 3,
    NETMIC_ERR_TYPE = 4,    /* unknown packet type */
    NETMIC_ERR_LENGTH = 5,  /* payload length inconsistent with type or datagram size */
} netmic_status;

/* Logical header. The in-memory layout mirrors the wire layout (packed, 24 bytes)
 * but code should go through netmic_header_encode/decode rather than memcpy. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t payloadBytes;
    uint32_t seq;
    uint32_t streamId;
    uint64_t samplePos;
} netmic_header;

#ifdef __cplusplus
static_assert(sizeof(netmic_header) == NETMIC_HEADER_BYTES, "header must be 24 bytes");
#else
_Static_assert(sizeof(netmic_header) == NETMIC_HEADER_BYTES, "header must be 24 bytes");
#endif

typedef struct {
    uint32_t sampleRate;
    uint8_t channels;
    uint8_t format;
    uint16_t framesPerPacket;
    uint8_t nameLen;
    char name[NETMIC_MAX_NAME_BYTES]; /* UTF-8, not NUL-terminated */
} netmic_announce;

/* ---- little-endian primitives ---- */

static inline void netmic_put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void netmic_put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void netmic_put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint16_t netmic_get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t netmic_get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static inline uint64_t netmic_get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ---- header ---- */

static inline void netmic_header_encode(const netmic_header* h, uint8_t out[NETMIC_HEADER_BYTES]) {
    netmic_put_u32(out + 0, h->magic);
    out[4] = h->version;
    out[5] = h->type;
    netmic_put_u16(out + 6, h->payloadBytes);
    netmic_put_u32(out + 8, h->seq);
    netmic_put_u32(out + 12, h->streamId);
    netmic_put_u64(out + 16, h->samplePos);
}

/* Validates the payload length for a given type (header already parsed). */
static inline netmic_status netmic_check_payload(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
    case NETMIC_TYPE_DISCOVER:
    case NETMIC_TYPE_SUBSCRIBE:
    case NETMIC_TYPE_UNSUBSCRIBE:
        return len == NETMIC_TOKEN_BYTES ? NETMIC_OK : NETMIC_ERR_LENGTH;
    case NETMIC_TYPE_ANNOUNCE:
        if (len < NETMIC_ANNOUNCE_FIXED_BYTES) return NETMIC_ERR_LENGTH;
        return len == NETMIC_ANNOUNCE_FIXED_BYTES + (size_t)payload[8] ? NETMIC_OK : NETMIC_ERR_LENGTH;
    case NETMIC_TYPE_AUDIO:
        return (len > 0 && len % 2 == 0) ? NETMIC_OK : NETMIC_ERR_LENGTH;
    default:
        return NETMIC_ERR_TYPE;
    }
}

/* Parses and validates a whole datagram. On success *payload points into buf. */
static inline netmic_status netmic_packet_decode(const uint8_t* buf, size_t len, netmic_header* h,
                                                 const uint8_t** payload) {
    if (len < NETMIC_HEADER_BYTES) return NETMIC_ERR_SHORT;
    h->magic = netmic_get_u32(buf + 0);
    h->version = buf[4];
    h->type = buf[5];
    h->payloadBytes = netmic_get_u16(buf + 6);
    h->seq = netmic_get_u32(buf + 8);
    h->streamId = netmic_get_u32(buf + 12);
    h->samplePos = netmic_get_u64(buf + 16);
    if (h->magic != NETMIC_MAGIC) return NETMIC_ERR_MAGIC;
    if (h->version != NETMIC_VERSION) return NETMIC_ERR_VERSION;
    if (len - NETMIC_HEADER_BYTES < h->payloadBytes) return NETMIC_ERR_SHORT;
    if (len - NETMIC_HEADER_BYTES != h->payloadBytes) return NETMIC_ERR_LENGTH;
    *payload = buf + NETMIC_HEADER_BYTES;
    return netmic_check_payload(h->type, *payload, h->payloadBytes);
}

/* Writes header + payload into out (capacity outCap). Fills magic, version and
 * payloadBytes itself. Returns total bytes written, or 0 if it does not fit. */
static inline size_t netmic_packet_encode(uint8_t type, uint32_t seq, uint32_t streamId, uint64_t samplePos,
                                          const uint8_t* payload, size_t payloadLen, uint8_t* out,
                                          size_t outCap) {
    if (payloadLen > NETMIC_MAX_PAYLOAD_BYTES || NETMIC_HEADER_BYTES + payloadLen > outCap) return 0;
    netmic_header h;
    h.magic = NETMIC_MAGIC;
    h.version = NETMIC_VERSION;
    h.type = type;
    h.payloadBytes = (uint16_t)payloadLen;
    h.seq = seq;
    h.streamId = streamId;
    h.samplePos = samplePos;
    netmic_header_encode(&h, out);
    if (payloadLen) memcpy(out + NETMIC_HEADER_BYTES, payload, payloadLen);
    return NETMIC_HEADER_BYTES + payloadLen;
}

/* ---- ANNOUNCE payload ---- */

/* Returns bytes written (NETMIC_ANNOUNCE_FIXED_BYTES + nameLen), or 0 if out is too small. */
static inline size_t netmic_announce_encode(const netmic_announce* a, uint8_t* out, size_t outCap) {
    size_t total = NETMIC_ANNOUNCE_FIXED_BYTES + a->nameLen;
    if (total > outCap) return 0;
    netmic_put_u32(out + 0, a->sampleRate);
    out[4] = a->channels;
    out[5] = a->format;
    netmic_put_u16(out + 6, a->framesPerPacket);
    out[8] = a->nameLen;
    if (a->nameLen) memcpy(out + 9, a->name, a->nameLen);
    return total;
}

static inline netmic_status netmic_announce_decode(const uint8_t* p, size_t len, netmic_announce* a) {
    if (len < NETMIC_ANNOUNCE_FIXED_BYTES || len != NETMIC_ANNOUNCE_FIXED_BYTES + (size_t)p[8])
        return NETMIC_ERR_LENGTH;
    a->sampleRate = netmic_get_u32(p + 0);
    a->channels = p[4];
    a->format = p[5];
    a->framesPerPacket = netmic_get_u16(p + 6);
    a->nameLen = p[8];
    if (a->nameLen) memcpy(a->name, p + 9, a->nameLen);
    return NETMIC_OK;
}

/* ---- AUDIO payload (Int16 LE mono PCM) ---- */

static inline int16_t netmic_pcm_sample(const uint8_t* payload, size_t index) {
    return (int16_t)netmic_get_u16(payload + 2 * index);
}

#ifdef __cplusplus
}
#endif

#endif /* NETMIC_PROTOCOL_H */
