/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DRIFT_OK = 0,
    DRIFT_OUT_OF_RANGE = 1,   /* ratio pinned at the clamp for too long: input rate is wrong */
} drift_status_t;

typedef struct {
    double kp;              /* ratio offset per frame of error */
    double ki;              /* ratio offset per frame*second of error */
    double lpf_tau_s;       /* measurement low-pass time constant */
    double max_ppm;         /* clamp of |ratio - 1| */
    double target_frames;   /* fill set point */
    double out_of_range_s;  /* continuous clamp time before DRIFT_OUT_OF_RANGE */
} drift_ctrl_cfg_t;

/*
 * omega_n = 0.126 rad/s, zeta = 1 for the plant de/dt = 48000 * (eps - (ratio - 1)).
 * The 3 s measurement low-pass keeps ISR timestamp jitter out of the ratio (< 1 ppm p-p).
 */
#define DRIFT_CTRL_CFG_DEFAULT(target) { 5.2e-6, 3.3e-7, 3.0, 500.0, (double)(target), 10.0 }

typedef struct {
    drift_ctrl_cfg_t cfg;
    double fill_lp;
    double integ;
    double ratio;
    double clamp_time_s;
    bool primed;
} drift_ctrl_t;

/* Resets the controller; ratio = 1.0 */
void drift_ctrl_init(drift_ctrl_t *c, const drift_ctrl_cfg_t *cfg);

/* One control step with the estimated FIFO fill (frames) and the time since the last step */
drift_status_t drift_ctrl_update(drift_ctrl_t *c, double fill_est_frames, double dt_s);

/* Input frames per output frame to apply to the resampler */
double drift_ctrl_ratio(const drift_ctrl_t *c);

/* (ratio - 1) * 1e6 */
double drift_ctrl_ppm(const drift_ctrl_t *c);

/* Clock offset learned by the integrator: the ratio without the proportional part */
double drift_ctrl_learned_ratio(const drift_ctrl_t *c);

/* Warm start: begin from a previously learned ratio instead of 1.0 (call after drift_ctrl_init) */
void drift_ctrl_preset(drift_ctrl_t *c, double ratio);

/*
 * FIFO fill interpolated between DMA interrupts: ring_count plus the frames that
 * arrived since the last DMA interrupt, clamped to [0, dma_frames].
 */
double fill_estimate(uint32_t ring_count, int64_t t_last_isr_us, int64_t t_now_us,
                     double fs_hz, uint32_t dma_frames);

#ifdef __cplusplus
}
#endif
