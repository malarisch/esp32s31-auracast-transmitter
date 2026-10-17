/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "resampler.h"
#include "test_util.h"

#define FS        48000.0
#define BLOCK     480
#define MAX_OUT   (2 * 48000)
#define MAX_IN    (BLOCK * 2 + RESAMPLER_TAPS + 16)

static double out_l[MAX_OUT + BLOCK];
static int32_t in_buf[MAX_IN * 2];
static int32_t blk_l[BLOCK];
static int32_t blk_r[BLOCK];

typedef double (*signal_fn)(uint64_t k);

static double g_f;
static double g_amp;

static double sig_sine(uint64_t k)
{
    return g_amp * sin(2.0 * M_PI * g_f * (double)k / FS);
}

/* Square wave: period 96 frames, 48 high / 48 low */
static double sig_square(uint64_t k)
{
    return ((k / 48) % 2 == 0) ? g_amp : -g_amp;
}

static int32_t to_i32(double v)
{
    return (int32_t)lrint(v * 2147483647.0);
}

/*
 * Run `blocks` blocks of BLOCK output frames. ratio_of(block) supplies the ratio per block.
 * Left output (normalized to +-1.0) goes to out_l. Returns frames pushed in total.
 */
static uint64_t run(signal_fn sig, double (*ratio_of)(uint32_t), uint32_t blocks, int *pull_ok)
{
    resampler_t *rs = resampler_create(2, BLOCK);
    CHECK(rs != NULL);
    uint64_t k = 0;
    int32_t *const out[2] = {blk_l, blk_r};
    *pull_ok = 1;
    for (uint32_t b = 0; b < blocks; b++) {
        resampler_set_ratio(rs, ratio_of(b));
        uint32_t need = resampler_input_needed(rs, BLOCK);
        if (need > MAX_IN) {
            *pull_ok = 0;
            break;
        }
        for (uint32_t i = 0; i < need; i++, k++) {
            int32_t v = to_i32(sig(k));
            in_buf[2 * i] = v;
            in_buf[2 * i + 1] = v;
        }
        resampler_push(rs, in_buf, need);
        uint32_t got = resampler_pull(rs, out, BLOCK);
        if (got != BLOCK) {
            *pull_ok = 0;
        }
        for (uint32_t i = 0; i < BLOCK; i++) {
            if ((size_t)b * BLOCK + i < MAX_OUT) {
                out_l[(size_t)b * BLOCK + i] = blk_l[i] / 2147483648.0;
            }
            if (blk_l[i] != blk_r[i]) {
                *pull_ok = 0; /* identical L/R input must give identical L/R output */
            }
        }
    }
    resampler_destroy(rs);
    return k;
}

static double g_ratio;
static double ratio_const(uint32_t b)
{
    (void)b;
    return g_ratio;
}

static void check_sine(double f, double ratio, double min_db)
{
    int ok;
    g_f = f;
    g_amp = 0.5;
    g_ratio = ratio;
    run(sig_sine, ratio_const, MAX_OUT / BLOCK, &ok);
    CHECK(ok);
    const size_t skip = 4800;
    double d = sinad_db(out_l + skip, MAX_OUT - skip, 2.0 * M_PI * f * ratio / FS);
    printf("  f=%.0f ratio=1%+.1fppm sinad=%.1f dB (min %.0f)\n", f, (ratio - 1.0) * 1e6, d, min_db);
    CHECK(d >= min_db);
}

static void test_sine_1k_unity(void)
{
    check_sine(1000, 1.0, 100.0);
}

static void test_sine_1k_plus100ppm(void)
{
    check_sine(1000, 1.0 + 100e-6, 100.0);
}

static void test_sine_1k_minus100ppm(void)
{
    check_sine(1000, 1.0 - 100e-6, 100.0);
}

static void test_sine_10k(void)
{
    check_sine(10000, 1.0 + 100e-6, 90.0);
}

static void test_sine_18k(void)
{
    check_sine(18000, 1.0 + 100e-6, 80.0);
}

static void test_input_needed_sufficient(void)
{
    resampler_t *rs = resampler_create(2, BLOCK);
    int32_t *const out[2] = {blk_l, blk_r};
    const double r = 1.0 + 41.7e-6;
    int ok = 1;
    resampler_set_ratio(rs, r);
    for (uint32_t b = 0; b < 1000; b++) {
        uint32_t need = resampler_input_needed(rs, BLOCK);
        if (b >= 10 && need > (uint32_t)ceil(BLOCK * r) + 2) {
            ok = 0;
        }
        for (uint32_t i = 0; i < 2 * need; i++) {
            in_buf[i] = 0;
        }
        resampler_push(rs, in_buf, need);
        if (resampler_pull(rs, out, BLOCK) != BLOCK) {
            ok = 0;
        }
    }
    CHECK(ok);
    resampler_destroy(rs);
}

/* Long-run accounting: input consumed must match output * ratio (no drift in the bookkeeping) */
static void test_frame_accounting(void)
{
    resampler_t *rs = resampler_create(2, BLOCK);
    int32_t *const out[2] = {blk_l, blk_r};
    const double r = 1.0 + 41.7e-6;
    uint64_t pushed = 0;
    resampler_set_ratio(rs, r);
    for (uint32_t b = 0; b < 6000; b++) {
        uint32_t need = resampler_input_needed(rs, BLOCK);
        for (uint32_t i = 0; i < 2 * need; i++) {
            in_buf[i] = 0;
        }
        resampler_push(rs, in_buf, need);
        resampler_pull(rs, out, BLOCK);
        pushed += need;
    }
    double expect = (double)BLOCK * 6000.0 * r;
    printf("  pushed=%llu expected=%.1f\n", (unsigned long long)pushed, expect);
    CHECK(fabs((double)pushed - expect) <= RESAMPLER_TAPS + 2);
    resampler_destroy(rs);
}

/*
 * With a ratio that changes every block (as under the PI controller) the input actually
 * consumed must still equal sum(480 * ratio_b): no frame may be lost or added.
 */
static void test_varying_ratio_accounting(void)
{
    resampler_t *rs = resampler_create(2, BLOCK);
    int32_t *const out[2] = {blk_l, blk_r};
    uint64_t pushed = 0;
    double expect = 0.0;
    uint32_t short_pulls = 0;
    for (uint32_t b = 0; b < 60000; b++) {
        double r = 1.0 + 40e-6 + 3e-6 * sin(2.0 * M_PI * b / 517.0);
        resampler_set_ratio(rs, r);
        uint32_t need = resampler_input_needed(rs, BLOCK);
        for (uint32_t i = 0; i < 2 * need; i++) {
            in_buf[i] = 0;
        }
        resampler_push(rs, in_buf, need);
        short_pulls += resampler_pull(rs, out, BLOCK) != BLOCK;
        pushed += need;
        expect += BLOCK * r;
    }
    printf("  pushed=%llu expected=%.1f diff=%.1f short_pulls=%u\n", (unsigned long long)pushed, expect,
           (double)pushed - expect, short_pulls);
    CHECK(short_pulls == 0);
    CHECK(fabs((double)pushed - expect) <= RESAMPLER_TAPS + 2);
    resampler_destroy(rs);
}

static double ratio_toggle(uint32_t b)
{
    return (b % 2) ? 1.0 + 500e-6 : 1.0 - 500e-6;
}

/* Changing the ratio between blocks must not create a discontinuity */
static void test_ratio_toggle_no_jump(void)
{
    int ok;
    g_f = 1000;
    g_amp = 0.5;
    run(sig_sine, ratio_toggle, MAX_OUT / BLOCK, &ok);
    CHECK(ok);
    double max_step = 0;
    for (size_t m = 4800; m + 1 < MAX_OUT; m++) {
        double s = fabs(out_l[m + 1] - out_l[m]);
        if (s > max_step) {
            max_step = s;
        }
    }
    double limit = 1.02 * 0.5 * 2.0 * M_PI * 1000.0 / FS;
    printf("  max_step=%.5f limit=%.5f\n", max_step, limit);
    CHECK(max_step <= limit);
}

/* Gibbs overshoot on a near-full-scale square must saturate, never wrap around */
static void test_fullscale_square_no_wrap(void)
{
    int ok;
    g_amp = 0.95;
    g_ratio = 1.0 + 100e-6;
    run(sig_square, ratio_const, 100, &ok);
    CHECK(ok);
    int bad = 0, overshoot = 0;
    for (size_t m = 4800 + 3; m + 3 < 100 * BLOCK; m++) {
        int pos_plateau = 1, neg_plateau = 1;
        for (int d = 1; d <= 3; d++) {
            pos_plateau &= out_l[m - d] > 0.5 && out_l[m + d] > 0.5;
            neg_plateau &= out_l[m - d] < -0.5 && out_l[m + d] < -0.5;
        }
        if ((pos_plateau && out_l[m] <= 0) || (neg_plateau && out_l[m] >= 0)) {
            bad++;
        }
        if (fabs(out_l[m]) > 0.999) {
            overshoot++;
        }
    }
    printf("  wrapped=%d saturated=%d\n", bad, overshoot);
    CHECK(overshoot > 0); /* the test only means something if the overshoot really hits the rails */
    CHECK(bad == 0);
}

int main(void)
{
    RUN_TEST(test_sine_1k_unity);
    RUN_TEST(test_sine_1k_plus100ppm);
    RUN_TEST(test_sine_1k_minus100ppm);
    RUN_TEST(test_sine_10k);
    RUN_TEST(test_sine_18k);
    RUN_TEST(test_input_needed_sufficient);
    RUN_TEST(test_frame_accounting);
    RUN_TEST(test_varying_ratio_accounting);
    RUN_TEST(test_ratio_toggle_no_jump);
    RUN_TEST(test_fullscale_square_no_wrap);
    TEST_MAIN_RETURN();
}
