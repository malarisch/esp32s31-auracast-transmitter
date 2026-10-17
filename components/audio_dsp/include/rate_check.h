/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Measurement window before a verdict; +-20 us ISR jitter gives < 80 ppm error at 0.5 s */
#define RATE_CHECK_MIN_WINDOW_US 500000

typedef enum {
    RATE_UNKNOWN = 0,   /* window not long enough yet */
    RATE_OK,
    RATE_BAD,           /* |deviation| > max_ppm */
} rate_status_t;

typedef struct {
    uint64_t frames0;
    int64_t t0_us;
    bool armed;
} rate_check_t;

/* Restart the measurement (call when the input signal (re)appears) */
void rate_check_reset(rate_check_t *rc);

/*
 * Feed the DMA frame counter and the timestamp of the interrupt that delivered it.
 * The first call after a reset takes the reference. *ppm receives the measured
 * deviation from nominal_hz whenever time has passed since the reference.
 */
rate_status_t rate_check_update(rate_check_t *rc, uint64_t frames_total, int64_t t_isr_us,
                                double nominal_hz, double max_ppm, double *ppm);

#ifdef __cplusplus
}
#endif
