#include "nm_ring.h"

#include <stdatomic.h>
#include <stdlib.h>

struct nm_ring {
    uint32_t capacity;
    _Atomic uint64_t head;  // total written (producer)
    _Atomic uint64_t tail;  // total read (consumer)
    _Atomic uint64_t overflows;
    float data[];
};

nm_ring* nm_ring_create(uint32_t capacity) {
    if (capacity == 0) return NULL;
    nm_ring* r = calloc(1, sizeof(nm_ring) + sizeof(float) * capacity);
    if (!r) return NULL;
    r->capacity = capacity;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->overflows, 0);
    return r;
}

void nm_ring_destroy(nm_ring* ring) { free(ring); }

uint32_t nm_ring_read(nm_ring* r, float* out, uint32_t max) {
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t avail = head - tail;
    uint32_t n = avail < max ? (uint32_t)avail : max;
    for (uint32_t i = 0; i < n; i++) out[i] = r->data[(tail + i) % r->capacity];
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    return n;
}

// Sample `frame` of global channel `ch` from a Float32 buffer list, where each
// buffer may itself hold several interleaved channels.
static inline float sample_at(const AudioBufferList* abl, uint32_t frame, uint32_t ch) {
    for (uint32_t b = 0; b < abl->mNumberBuffers; b++) {
        const AudioBuffer* buf = &abl->mBuffers[b];
        uint32_t nch = buf->mNumberChannels ? buf->mNumberChannels : 1;
        if (ch < nch) {
            uint32_t idx = frame * nch + ch;
            if (buf->mData == NULL || (idx + 1) * sizeof(float) > buf->mDataByteSize) return 0.0f;
            return ((const float*)buf->mData)[idx];
        }
        ch -= nch;
    }
    return 0.0f;
}

uint32_t nm_ring_push_abl(nm_ring* r, const AudioBufferList* abl, uint32_t frames, int32_t channel) {
    if (!r || !abl || abl->mNumberBuffers == 0) return 0;
    uint32_t total_ch = 0;
    for (uint32_t b = 0; b < abl->mNumberBuffers; b++) {
        uint32_t nch = abl->mBuffers[b].mNumberChannels;
        total_ch += nch ? nch : 1;
    }

    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint64_t space = r->capacity - (head - tail);
    uint32_t n = frames <= space ? frames : (uint32_t)space;
    if (n < frames) atomic_fetch_add_explicit(&r->overflows, frames - n, memory_order_relaxed);

    for (uint32_t f = 0; f < n; f++) {
        float v;
        if (channel >= 0) {
            v = (uint32_t)channel < total_ch ? sample_at(abl, f, (uint32_t)channel) : 0.0f;
        } else {
            float sum = 0.0f;
            for (uint32_t c = 0; c < total_ch; c++) sum += sample_at(abl, f, c);
            v = sum / (float)total_ch;
        }
        r->data[(head + f) % r->capacity] = v;
    }
    atomic_store_explicit(&r->head, head + n, memory_order_release);
    return n;
}

uint64_t nm_ring_overflows(const nm_ring* r) {
    return atomic_load_explicit(&((nm_ring*)r)->overflows, memory_order_relaxed);
}
