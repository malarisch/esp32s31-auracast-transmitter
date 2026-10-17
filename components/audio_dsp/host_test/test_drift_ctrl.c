/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Closed-loop simulation: I2S DMA blocks arrive at 48 kHz * (1 + eps) with
 * timestamp jitter, the Bluetooth side consumes 480 * ratio frames every 10 ms.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "drift_ctrl.h"
#include "test_util.h"

#define FS        48000.0
#define DMA       240
#define CAPACITY  4096
#define TARGET    960
#define TICK_S    0.010

static void test_fill_estimate(void)
{
    CHECK(fabs(fill_estimate(500, 1000, 3000, 48000, 240) - 596.0) < 1e-9);  /* +2 ms -> +96 */
    CHECK(fabs(fill_estimate(500, 1000, 11000, 48000, 240) - 740.0) < 1e-9); /* clamp to count + dma */
    CHECK(fabs(fill_estimate(500, 5000, 3000, 48000, 240) - 500.0) < 1e-9);  /* now < isr -> count */
}

typedef struct {
    double eps_ppm;
    double eps_ppm_after;
    double step_at_s;     /* < 0: no step */
    double duration_s;
    uint32_t seed;
} sim_cfg_t;

typedef struct {
    double final_ppm;          /* mean over the last 10 s */
    double final_fill_err;     /* mean (est - TARGET) over the last 10 s */
    double ppm_pp_last60s;
    int64_t min_count;         /* minimum ring count right before a consume (after priming) */
    int64_t max_count;
    bool out_of_range;
    double out_of_range_at_s;
} sim_result_t;

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static sim_result_t simulate(const sim_cfg_t *cfg)
{
    drift_ctrl_cfg_t dc = DRIFT_CTRL_CFG_DEFAULT(TARGET);
    drift_ctrl_t c;
    drift_ctrl_init(&c, &dc);

    sim_result_t res = {0};
    res.min_count = INT64_MAX;
    uint32_t seed = cfg->seed;
    int64_t count = 0;
    double t_dma = 0.0;
    double t_isr = 0.0;
    double acc = 0.0;
    bool running = false;
    double ppm_min60 = 1e9, ppm_max60 = -1e9;
    double sum_ppm = 0, sum_err = 0;
    int n_last10 = 0;
    int ticks = (int)lround(cfg->duration_s / TICK_S);

    for (int n = 1; n <= ticks; n++) {
        double t_now = n * TICK_S;
        /* Deliver all DMA blocks that completed before this tick */
        for (;;) {
            double eps = (cfg->step_at_s >= 0 && t_dma >= cfg->step_at_s) ? cfg->eps_ppm_after : cfg->eps_ppm;
            double t_next = t_dma + DMA / (FS * (1.0 + eps * 1e-6));
            if (t_next > t_now) {
                break;
            }
            t_dma = t_next;
            count += DMA;
            if (count > CAPACITY) {
                count = CAPACITY;
            }
            if (count > res.max_count) {
                res.max_count = count;
            }
            double jitter = ((double)(lcg(&seed) >> 8) / (double)(1u << 24) - 0.5) * 40e-6;
            t_isr = t_dma + jitter;
        }
        double est = fill_estimate((uint32_t)count, (int64_t)llround(t_isr * 1e6),
                                   (int64_t)llround(t_now * 1e6), FS, DMA);
        if (!running) {
            running = count >= TARGET;
            if (!running) {
                continue;
            }
            /* PRIME -> RUN as in the pipeline: drop the excess so the loop starts at the set point */
            int64_t excess = (int64_t)floor(est - TARGET);
            if (excess > 0) {
                count -= excess;
                est -= (double)excess;
            }
        }
        drift_status_t st = drift_ctrl_update(&c, est, TICK_S);
        if (st == DRIFT_OUT_OF_RANGE && !res.out_of_range) {
            res.out_of_range = true;
            res.out_of_range_at_s = t_now;
        }
        if (count < res.min_count) {
            res.min_count = count;
        }
        acc += 480.0 * drift_ctrl_ratio(&c);
        int64_t take = (int64_t)floor(acc);
        acc -= (double)take;
        count -= take;
        if (count < 0) {
            count = 0;
        }

        double ppm = drift_ctrl_ppm(&c);
        if (t_now > cfg->duration_s - 60.0) {
            if (ppm < ppm_min60) ppm_min60 = ppm;
            if (ppm > ppm_max60) ppm_max60 = ppm;
        }
        if (t_now > cfg->duration_s - 10.0) {
            sum_ppm += ppm;
            sum_err += est - TARGET;
            n_last10++;
        }
    }
    res.final_ppm = n_last10 ? sum_ppm / n_last10 : 0;
    res.final_fill_err = n_last10 ? sum_err / n_last10 : 0;
    res.ppm_pp_last60s = ppm_max60 - ppm_min60;
    printf("  eps=%+.1f->%+.1f ppm: final_ppm=%+.3f fill_err=%+.2f pp60=%.3f min=%lld max=%lld oor=%d@%.1fs\n",
           cfg->eps_ppm, cfg->step_at_s >= 0 ? cfg->eps_ppm_after : cfg->eps_ppm,
           res.final_ppm, res.final_fill_err, res.ppm_pp_last60s,
           (long long)res.min_count, (long long)res.max_count, res.out_of_range, res.out_of_range_at_s);
    return res;
}

static void test_converges_plus(void)
{
    sim_cfg_t cfg = {41.7, 41.7, -1, 300, 1};
    sim_result_t r = simulate(&cfg);
    CHECK(fabs(r.final_ppm - 41.7) < 1.0);
    CHECK(fabs(r.final_fill_err) < 24.0);
    CHECK(r.min_count >= 544);
    CHECK(r.max_count <= CAPACITY - DMA);
    CHECK(r.ppm_pp_last60s < 1.0);
    CHECK(!r.out_of_range);
}

static void test_converges_minus(void)
{
    sim_cfg_t cfg = {-100.0, -100.0, -1, 300, 2};
    sim_result_t r = simulate(&cfg);
    CHECK(fabs(r.final_ppm + 100.0) < 1.0);
    CHECK(r.min_count >= 544);
    CHECK(r.max_count <= CAPACITY - DMA);
    CHECK(!r.out_of_range);
}

static void test_step(void)
{
    sim_cfg_t cfg = {50.0, -50.0, 150.0, 400, 3};
    sim_result_t r = simulate(&cfg);
    CHECK(fabs(r.final_ppm + 50.0) < 1.0);
    CHECK(r.min_count >= 544);
    CHECK(r.max_count <= CAPACITY - DMA);
    CHECK(!r.out_of_range);
}

static void test_out_of_range(void)
{
    sim_cfg_t cfg = {2000.0, 2000.0, -1, 60, 4};
    sim_result_t r = simulate(&cfg);
    CHECK(r.out_of_range);
    CHECK(r.out_of_range_at_s <= 30.0);
}

int main(void)
{
    RUN_TEST(test_fill_estimate);
    RUN_TEST(test_converges_plus);
    RUN_TEST(test_converges_minus);
    RUN_TEST(test_step);
    RUN_TEST(test_out_of_range);
    TEST_MAIN_RETURN();
}
