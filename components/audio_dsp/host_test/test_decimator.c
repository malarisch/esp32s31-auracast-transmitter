/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "decimator.h"
#include "test_util.h"

#define FS      48000.0
#define BLOCK   480
#define BLOCKS  200

static double out_l[BLOCKS * BLOCK];
static int32_t in_buf[BLOCK * 2];
static int32_t blk[2][BLOCK];

/* Runs a stereo sine (identical L/R) through the decimator block by block; returns output frames */
static size_t run_sine(double f, double amp, uint8_t decim, int *ok)
{
    decimator_t *d = decimator_create(2, decim, BLOCK);
    CHECK(d != NULL);
    *ok = d != NULL;
    if (!d) {
        return 0;
    }
    size_t n = 0;
    uint64_t k = 0;
    int32_t *const out[2] = {blk[0], blk[1]};
    for (uint32_t b = 0; b < BLOCKS; b++) {
        for (uint32_t i = 0; i < BLOCK; i++, k++) {
            int32_t v = (int32_t)lrint(amp * 2147483647.0 * sin(2.0 * M_PI * f * (double)k / FS));
            in_buf[2 * i] = v;
            in_buf[2 * i + 1] = v;
        }
        uint32_t got = decimator_process(d, in_buf, BLOCK, out);
        if (got != BLOCK / decim) {
            *ok = 0;
        }
        for (uint32_t i = 0; i < got; i++) {
            if (blk[0][i] != blk[1][i]) {
                *ok = 0;
            }
            out_l[n++] = blk[0][i] / 2147483648.0;
        }
    }
    decimator_destroy(d);
    return n;
}

static void check_sine(double f, uint8_t decim, double min_db)
{
    int ok;
    size_t n = run_sine(f, 0.5, decim, &ok);
    CHECK(ok);
    const size_t skip = 2400;
    double d = sinad_db(out_l + skip, n - skip, 2.0 * M_PI * f * decim / FS);
    printf("  f=%.0f decim=%u sinad=%.1f dB (min %.0f)\n", f, decim, d, min_db);
    CHECK(d >= min_db);
}

/* A tone above the output's Nyquist frequency must be attenuated, not aliased into the band */
static void check_alias(double f, uint8_t decim, double max_rms)
{
    int ok;
    size_t n = run_sine(f, 0.5, decim, &ok);
    CHECK(ok);
    double e = 0.0;
    const size_t skip = 2400;
    for (size_t i = skip; i < n; i++) {
        e += out_l[i] * out_l[i];
    }
    double rms = sqrt(e / (double)(n - skip));
    printf("  alias f=%.0f decim=%u rms=%.2e (max %.0e)\n", f, decim, rms, max_rms);
    CHECK(rms <= max_rms);
}

static void test_decim2_sine_1k(void) { check_sine(1000.0, 2, 90.0); }
static void test_decim2_sine_9k(void) { check_sine(9000.0, 2, 70.0); }
static void test_decim2_alias_14k(void) { check_alias(14000.0, 2, 1e-3); }
static void test_decim3_sine_1k(void) { check_sine(1000.0, 3, 90.0); }
static void test_decim3_sine_6k(void) { check_sine(6000.0, 3, 70.0); }
static void test_decim3_alias_10k(void) { check_alias(10000.0, 3, 1e-3); }

/* 480 input frames give exactly 240 (or 160) output frames, every block, from the first one on */
static void test_frame_accounting(void)
{
    int ok;
    size_t n = run_sine(1000.0, 0.5, 2, &ok);
    CHECK(ok && n == (size_t)BLOCKS * BLOCK / 2);
    n = run_sine(1000.0, 0.5, 3, &ok);
    CHECK(ok && n == (size_t)BLOCKS * BLOCK / 3);
}

/* Group delay: (taps - 1) / 2 = 63.5 input samples for 2:1; the impulse peaks at output frame 32 */
static void test_group_delay(void)
{
    decimator_t *d = decimator_create(1, 2, BLOCK);
    CHECK(d != NULL);
    int32_t *const out[1] = {blk[0]};
    for (uint32_t i = 0; i < BLOCK; i++) {
        in_buf[i] = i == 0 ? INT32_MAX / 2 : 0;
    }
    decimator_process(d, in_buf, BLOCK, out);
    uint32_t peak = 0;
    for (uint32_t i = 1; i < BLOCK / 2; i++) {
        if (llabs(blk[0][i]) > llabs(blk[0][peak])) {
            peak = i;
        }
    }
    const uint32_t expect = decimator_delay_frames(d) / 2;   /* output frames */
    printf("  impulse peak at output frame %u (expected %u)\n", (unsigned)peak, (unsigned)expect);
    CHECK(peak == expect);
    CHECK(decimator_delay_frames(d) == DECIMATOR_TAPS_PER_DECIM);
    decimator_destroy(d);
}

static void test_reset_clears_history(void)
{
    decimator_t *d = decimator_create(1, 2, BLOCK);
    CHECK(d != NULL);
    int32_t *const out[1] = {blk[0]};
    for (uint32_t i = 0; i < BLOCK; i++) {
        in_buf[i] = INT32_MAX / 2;
    }
    decimator_process(d, in_buf, BLOCK, out);
    decimator_reset(d);
    for (uint32_t i = 0; i < BLOCK; i++) {
        in_buf[i] = 0;
    }
    decimator_process(d, in_buf, BLOCK, out);
    int all_zero = 1;
    for (uint32_t i = 0; i < BLOCK / 2; i++) {
        all_zero = all_zero && blk[0][i] == 0;
    }
    CHECK(all_zero);
    decimator_destroy(d);
}

int main(void)
{
    RUN_TEST(test_decim2_sine_1k);
    RUN_TEST(test_decim2_sine_9k);
    RUN_TEST(test_decim2_alias_14k);
    RUN_TEST(test_decim3_sine_1k);
    RUN_TEST(test_decim3_sine_6k);
    RUN_TEST(test_decim3_alias_10k);
    RUN_TEST(test_frame_accounting);
    RUN_TEST(test_group_delay);
    RUN_TEST(test_reset_clears_history);
    return g_test_failures ? 1 : 0;
}
