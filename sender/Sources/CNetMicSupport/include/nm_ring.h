// Lock-free SPSC ring of mono float samples, used between the capture
// real-time callback (producer) and the sender's packetizer thread (consumer).
// Written in C so the real-time path has no Swift runtime / ARC involvement.
#ifndef NM_RING_H
#define NM_RING_H

#include <CoreAudioTypes/CoreAudioTypes.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nm_ring nm_ring;

nm_ring* nm_ring_create(uint32_t capacity);
void nm_ring_destroy(nm_ring* ring);

/// Consumer: copies up to `max` samples into `out`, returns how many.
uint32_t nm_ring_read(nm_ring* ring, float* out, uint32_t max);

/// Producer (real-time safe): reduces a Float32 AudioBufferList to mono and
/// appends it. `channel` >= 0 selects one channel (counted across all buffers),
/// `channel` < 0 averages all channels. Frames that do not fit are dropped and
/// counted. Returns frames written.
uint32_t nm_ring_push_abl(nm_ring* ring, const AudioBufferList* abl, uint32_t frames, int32_t channel);

/// Frames dropped because the ring was full (consumer stalled).
uint64_t nm_ring_overflows(const nm_ring* ring);

#ifdef __cplusplus
}
#endif

#endif
