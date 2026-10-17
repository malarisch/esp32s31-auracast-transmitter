/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <string.h>

#include "spsc_ring.h"
#include "test_util.h"

#define CH 2

static int32_t storage[256 * CH];
static int32_t in[512 * CH];
static int32_t out[512 * CH];

/* Fill frames [0, n) with values derived from a running frame index start+i */
static void fill(int32_t *dst, uint32_t start, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i * CH + 0] = (int32_t)(start + i);
        dst[i * CH + 1] = -(int32_t)(start + i) - 1;
    }
}

static int matches(const int32_t *src, uint32_t start, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (src[i * CH + 0] != (int32_t)(start + i) || src[i * CH + 1] != -(int32_t)(start + i) - 1) {
            return 0;
        }
    }
    return 1;
}

static void test_empty_after_init(void)
{
    spsc_ring_t r;
    spsc_ring_init(&r, storage, 64, CH);
    CHECK(spsc_ring_count(&r) == 0);
    CHECK(spsc_ring_read(&r, out, 10) == 0);
}

static void test_write_read_roundtrip(void)
{
    spsc_ring_t r;
    spsc_ring_init(&r, storage, 256, CH);
    fill(in, 0, 100);
    CHECK(spsc_ring_write(&r, in, 100) == 100);
    CHECK(spsc_ring_count(&r) == 100);
    memset(out, 0, sizeof(out));
    CHECK(spsc_ring_read(&r, out, 100) == 100);
    CHECK(matches(out, 0, 100));
    CHECK(spsc_ring_count(&r) == 0);
}

static void test_wraparound(void)
{
    spsc_ring_t r;
    spsc_ring_init(&r, storage, 64, CH);
    int ok = 1;
    for (uint32_t round = 0; round < 50; round++) {
        fill(in, round * 40, 40);
        ok &= spsc_ring_write(&r, in, 40) == 40;
        memset(out, 0, sizeof(out));
        ok &= spsc_ring_read(&r, out, 40) == 40;
        ok &= matches(out, round * 40, 40);
    }
    CHECK(ok);
    CHECK(spsc_ring_count(&r) == 0);
}

static void test_full_rejects_excess(void)
{
    spsc_ring_t r;
    spsc_ring_init(&r, storage, 64, CH);
    fill(in, 0, 100);
    CHECK(spsc_ring_write(&r, in, 100) == 64);
    CHECK(spsc_ring_count(&r) == 64);
    CHECK(spsc_ring_write(&r, in, 1) == 0);
    CHECK(spsc_ring_read(&r, out, 100) == 64);
    CHECK(matches(out, 0, 64));
}

static void test_drop_all(void)
{
    spsc_ring_t r;
    spsc_ring_init(&r, storage, 64, CH);
    fill(in, 0, 30);
    spsc_ring_write(&r, in, 30);
    spsc_ring_drop_all(&r);
    CHECK(spsc_ring_count(&r) == 0);
    fill(in, 1000, 20);
    CHECK(spsc_ring_write(&r, in, 20) == 20);
    CHECK(spsc_ring_read(&r, out, 20) == 20);
    CHECK(matches(out, 1000, 20));
}

/* The free-running u32 indices must survive wrapping past UINT32_MAX */
static void test_u32_index_wrap(void)
{
    spsc_ring_t r;
    spsc_ring_init(&r, storage, 64, CH);
    atomic_store(&r.w, UINT32_MAX - 10);
    atomic_store(&r.r, UINT32_MAX - 10);
    CHECK(spsc_ring_count(&r) == 0);
    fill(in, 7, 40);
    CHECK(spsc_ring_write(&r, in, 40) == 40);
    CHECK(spsc_ring_count(&r) == 40);
    CHECK(spsc_ring_read(&r, out, 40) == 40);
    CHECK(matches(out, 7, 40));
}

int main(void)
{
    RUN_TEST(test_empty_after_init);
    RUN_TEST(test_write_read_roundtrip);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_full_rejects_excess);
    RUN_TEST(test_drop_all);
    RUN_TEST(test_u32_index_wrap);
    TEST_MAIN_RETURN();
}
