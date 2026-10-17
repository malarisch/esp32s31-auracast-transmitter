/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Guards the measurement helper the resampler tests rely on: a broken
 * sinad_db() would make every quality threshold meaningless.
 */
#include <stdlib.h>

#include "test_util.h"

#define N 48000

static double buf[N];

/* Clean sine with arbitrary phase and DC must measure as (numerically) perfect */
static void test_sinad_clean_sine(void)
{
    double w = 2 * M_PI * 1000.0 / 48000.0;
    for (int i = 0; i < N; i++) {
        buf[i] = 0.5 * sin(w * i + 0.3) + 0.01;
    }
    CHECK(sinad_db(buf, N, w) > 150.0);
}

/* Sine 0.5 plus an interferer of 0.0005 at another frequency: SINAD = 20*log10(1000) = 60 dB */
static void test_sinad_known_interferer(void)
{
    double w = 2 * M_PI * 1000.0 / 48000.0;
    double w2 = 2 * M_PI * 3100.0 / 48000.0;
    for (int i = 0; i < N; i++) {
        buf[i] = 0.5 * sin(w * i) + 0.0005 * sin(w2 * i);
    }
    double d = sinad_db(buf, N, w);
    CHECK(fabs(d - 60.0) < 0.1);
}

/* Fitting at the wrong frequency must not look clean */
static void test_sinad_wrong_frequency_is_bad(void)
{
    double w = 2 * M_PI * 1000.0 / 48000.0;
    for (int i = 0; i < N; i++) {
        buf[i] = 0.5 * sin(w * i);
    }
    CHECK(sinad_db(buf, N, w * 1.001) < 20.0);
}

int main(void)
{
    RUN_TEST(test_sinad_clean_sine);
    RUN_TEST(test_sinad_known_interferer);
    RUN_TEST(test_sinad_wrong_frequency_is_bad);
    TEST_MAIN_RETURN();
}
