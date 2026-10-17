/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PI controller that steers the resampler ratio from the input FIFO fill level.
 */
#include <math.h>

#include "drift_ctrl.h"

void drift_ctrl_init(drift_ctrl_t *c, const drift_ctrl_cfg_t *cfg)
{
    c->cfg = *cfg;
    c->fill_lp = 0.0;
    c->integ = 0.0;
    c->ratio = 1.0;
    c->clamp_time_s = 0.0;
    c->primed = false;
}

drift_status_t drift_ctrl_update(drift_ctrl_t *c, double fill_est_frames, double dt_s)
{
    if (!c->primed) {
        c->fill_lp = fill_est_frames;
        c->primed = true;
    } else {
        c->fill_lp += (fill_est_frames - c->fill_lp) * dt_s / (c->cfg.lpf_tau_s + dt_s);
    }

    const double e = c->fill_lp - c->cfg.target_frames;
    const double limit = c->cfg.max_ppm * 1e-6;
    double offset = c->cfg.kp * e + c->cfg.ki * c->integ;
    bool clamped = false;
    if (offset > limit) {
        offset = limit;
        clamped = true;
    } else if (offset < -limit) {
        offset = -limit;
        clamped = true;
    }

    /* Anti-windup: integrate only when not clamped, or when the error drives out of the clamp */
    if (!clamped || (offset > 0 && e < 0) || (offset < 0 && e > 0)) {
        c->integ += e * dt_s;
    }

    c->ratio = 1.0 + offset;
    c->clamp_time_s = clamped ? c->clamp_time_s + dt_s : 0.0;
    return (c->clamp_time_s >= c->cfg.out_of_range_s) ? DRIFT_OUT_OF_RANGE : DRIFT_OK;
}

double drift_ctrl_ratio(const drift_ctrl_t *c)
{
    return c->ratio;
}

double drift_ctrl_ppm(const drift_ctrl_t *c)
{
    return (c->ratio - 1.0) * 1e6;
}

double drift_ctrl_learned_ratio(const drift_ctrl_t *c)
{
    return 1.0 + c->cfg.ki * c->integ;
}

void drift_ctrl_preset(drift_ctrl_t *c, double ratio)
{
    c->integ = (ratio - 1.0) / c->cfg.ki;
    c->ratio = ratio;
}

double fill_estimate(uint32_t ring_count, int64_t t_last_isr_us, int64_t t_now_us,
                     double fs_hz, uint32_t dma_frames)
{
    double since = (double)(t_now_us - t_last_isr_us) * fs_hz * 1e-6;
    if (since < 0.0) {
        since = 0.0;
    } else if (since > (double)dma_frames) {
        since = (double)dma_frames;
    }
    return (double)ring_count + since;
}
