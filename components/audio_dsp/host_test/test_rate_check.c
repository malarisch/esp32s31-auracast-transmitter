/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Direct input-rate check from DMA block counts and ISR timestamps. It must flag
 * grossly wrong rates (44.1 kHz, 96 kHz) that the drift controller cannot see,
 * because those drain or flood the FIFO long before the controller's clamp timer.
 */
#include <math.h>
#include <stdint.h>

#include "rate_check.h"
#include "test_util.h"

#define DMA 240
#define MAX_PPM 600.0

static uint32_t s_seed = 1;

static double jitter_us(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return ((double)(s_seed >> 8) / (double)(1u << 24) - 0.5) * 40.0; /* +-20 us */
}

/* Feed DMA blocks of an input running at fs_hz until t_end_s; returns the last status */
static rate_status_t feed(rate_check_t *rc, double fs_hz, double t_end_s, double *ppm)
{
    rate_status_t st = RATE_UNKNOWN;
    uint64_t frames = 0;
    for (uint64_t k = 1;; k++) {
        double t = (double)k * DMA / fs_hz;
        if (t > t_end_s) {
            break;
        }
        frames += DMA;
        st = rate_check_update(rc, frames, (int64_t)llround(t * 1e6 + jitter_us()), 48000.0, MAX_PPM, ppm);
    }
    return st;
}

static void test_unknown_before_window(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    CHECK(feed(&rc, 48000.0, 0.3, &ppm) == RATE_UNKNOWN);
}

static void test_nominal_ok(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    CHECK(feed(&rc, 48000.0, 0.6, &ppm) == RATE_OK);
    CHECK(fabs(ppm) < 100.0);
}

static void test_small_offset_measured(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    CHECK(feed(&rc, 48000.0 * (1.0 + 41.74e-6), 10.0, &ppm) == RATE_OK);
    CHECK(fabs(ppm - 41.74) < 4.5); /* +-20 us at both ends of a 10 s window = +-4 ppm */
}

static void test_44k1_bad(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    CHECK(feed(&rc, 44100.0, 0.6, &ppm) == RATE_BAD);
    CHECK(fabs(ppm - (-81250.0)) < 200.0);
}

static void test_96k_bad(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    CHECK(feed(&rc, 96000.0, 0.6, &ppm) == RATE_BAD);
}

static void test_threshold(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    CHECK(feed(&rc, 48000.0 * (1.0 + 700e-6), 1.0, &ppm) == RATE_BAD);
    rate_check_reset(&rc);
    CHECK(feed(&rc, 48000.0 * (1.0 - 700e-6), 1.0, &ppm) == RATE_BAD);
    rate_check_reset(&rc);
    CHECK(feed(&rc, 48000.0 * (1.0 + 450e-6), 1.0, &ppm) == RATE_OK);
}

/* After a reset the reference restarts: a gap must not end up in the measurement window */
static void test_reset_restarts_window(void)
{
    rate_check_t rc;
    double ppm = 0;
    rate_check_reset(&rc);
    feed(&rc, 48000.0, 1.0, &ppm);
    rate_check_reset(&rc);
    CHECK(rate_check_update(&rc, 1000000, 50000000, 48000.0, MAX_PPM, &ppm) == RATE_UNKNOWN);
    CHECK(rate_check_update(&rc, 1000000 + 48000, 51000000, 48000.0, MAX_PPM, &ppm) == RATE_OK);
    CHECK(fabs(ppm) < 1e-6);
}

int main(void)
{
    RUN_TEST(test_unknown_before_window);
    RUN_TEST(test_nominal_ok);
    RUN_TEST(test_small_offset_measured);
    RUN_TEST(test_44k1_bad);
    RUN_TEST(test_96k_bad);
    RUN_TEST(test_threshold);
    RUN_TEST(test_reset_restarts_window);
    TEST_MAIN_RETURN();
}
