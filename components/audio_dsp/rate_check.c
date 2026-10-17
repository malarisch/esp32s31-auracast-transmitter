/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>

#include "rate_check.h"

void rate_check_reset(rate_check_t *rc)
{
    rc->frames0 = 0;
    rc->t0_us = 0;
    rc->armed = false;
}

rate_status_t rate_check_update(rate_check_t *rc, uint64_t frames_total, int64_t t_isr_us,
                                double nominal_hz, double max_ppm, double *ppm)
{
    if (!rc->armed) {
        rc->frames0 = frames_total;
        rc->t0_us = t_isr_us;
        rc->armed = true;
        return RATE_UNKNOWN;
    }
    int64_t dt_us = t_isr_us - rc->t0_us;
    if (dt_us <= 0) {
        return RATE_UNKNOWN;
    }
    double rate = (double)(frames_total - rc->frames0) / ((double)dt_us * 1e-6);
    *ppm = (rate / nominal_hz - 1.0) * 1e6;
    if (dt_us < RATE_CHECK_MIN_WINDOW_US) {
        return RATE_UNKNOWN;
    }
    return fabs(*ppm) > max_ppm ? RATE_BAD : RATE_OK;
}
