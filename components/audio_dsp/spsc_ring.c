/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "spsc_ring.h"

void spsc_ring_init(spsc_ring_t *ring, int32_t *storage, uint32_t capacity_frames, uint8_t channels)
{
    ring->buf = storage;
    ring->capacity_frames = capacity_frames;
    ring->channels = channels;
    atomic_store_explicit(&ring->w, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->r, 0, memory_order_relaxed);
}

uint32_t spsc_ring_count(const spsc_ring_t *ring)
{
    uint32_t w = atomic_load_explicit(&((spsc_ring_t *)ring)->w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&((spsc_ring_t *)ring)->r, memory_order_acquire);
    return w - r;
}

/* Copy n frames between the ring (starting at frame index idx) and a linear buffer, handling the wrap */
static void ring_copy(const spsc_ring_t *ring, uint32_t idx, int32_t *linear, uint32_t n, int to_ring)
{
    uint32_t mask = ring->capacity_frames - 1;
    uint32_t pos = idx & mask;
    uint32_t first = ring->capacity_frames - pos;
    if (first > n) {
        first = n;
    }
    size_t frame_bytes = (size_t)ring->channels * sizeof(int32_t);
    int32_t *slot = ring->buf + (size_t)pos * ring->channels;
    if (to_ring) {
        memcpy(slot, linear, first * frame_bytes);
        memcpy(ring->buf, linear + (size_t)first * ring->channels, (n - first) * frame_bytes);
    } else {
        memcpy(linear, slot, first * frame_bytes);
        memcpy(linear + (size_t)first * ring->channels, ring->buf, (n - first) * frame_bytes);
    }
}

uint32_t spsc_ring_write(spsc_ring_t *ring, const int32_t *frames, uint32_t n)
{
    uint32_t w = atomic_load_explicit(&ring->w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&ring->r, memory_order_acquire);
    uint32_t space = ring->capacity_frames - (w - r);
    if (n > space) {
        n = space;
    }
    if (n == 0) {
        return 0;
    }
    ring_copy(ring, w, (int32_t *)frames, n, 1);
    atomic_store_explicit(&ring->w, w + n, memory_order_release);
    return n;
}

uint32_t spsc_ring_read(spsc_ring_t *ring, int32_t *frames, uint32_t n)
{
    uint32_t r = atomic_load_explicit(&ring->r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&ring->w, memory_order_acquire);
    uint32_t avail = w - r;
    if (n > avail) {
        n = avail;
    }
    if (n == 0) {
        return 0;
    }
    ring_copy(ring, r, frames, n, 0);
    atomic_store_explicit(&ring->r, r + n, memory_order_release);
    return n;
}

void spsc_ring_drop_all(spsc_ring_t *ring)
{
    uint32_t w = atomic_load_explicit(&ring->w, memory_order_acquire);
    atomic_store_explicit(&ring->r, w, memory_order_release);
}
