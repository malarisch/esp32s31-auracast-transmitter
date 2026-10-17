/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lock-free single-producer/single-consumer ring of interleaved int32 audio frames.
 * The producer may run in an ISR, the consumer in a task (possibly on the other core).
 */
typedef struct {
    int32_t *buf;               /* capacity_frames * channels samples */
    uint32_t capacity_frames;   /* power of two */
    uint8_t channels;
    _Atomic uint32_t w;         /* total frames written (wraps) */
    _Atomic uint32_t r;         /* total frames read (wraps) */
} spsc_ring_t;

void spsc_ring_init(spsc_ring_t *ring, int32_t *storage, uint32_t capacity_frames, uint8_t channels);

/* Producer side. Writes at most the free space; returns frames written. */
uint32_t spsc_ring_write(spsc_ring_t *ring, const int32_t *frames, uint32_t n);

/* Consumer side. Returns frames read (<= n). */
uint32_t spsc_ring_read(spsc_ring_t *ring, int32_t *frames, uint32_t n);

/* Frames currently available to the consumer. */
uint32_t spsc_ring_count(const spsc_ring_t *ring);

/* Consumer side: discard everything currently in the ring. */
void spsc_ring_drop_all(spsc_ring_t *ring);

#ifdef __cplusplus
}
#endif
