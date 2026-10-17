/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal test helpers for the audio_dsp host tests (no external framework).
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdio.h>

static int g_test_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            g_test_failures++;                                               \
        }                                                                    \
    } while (0)

#define RUN_TEST(fn)                         \
    do {                                     \
        int before_ = g_test_failures;       \
        fn();                                \
        printf("%s %s\n",                    \
               g_test_failures == before_ ? "PASS" : "FAIL", #fn); \
    } while (0)

#define TEST_MAIN_RETURN() return g_test_failures ? 1 : 0

/*
 * Least-squares fit of y[n] ~ a*sin(omega*n) + b*cos(omega*n) + c and return
 * 10*log10(P_signal / P_residual) in dB (SINAD against the fitted sine).
 * omega is in radians per sample.
 */
static inline double sinad_db(const double *y, size_t n, double omega)
{
    /* Normal equations for basis [s, c, 1] */
    double ss = 0, sc = 0, s1 = 0, cc = 0, c1 = 0, n1 = 0;
    double ys = 0, yc = 0, y1 = 0;
    for (size_t i = 0; i < n; i++) {
        double s = sin(omega * (double)i);
        double c = cos(omega * (double)i);
        ss += s * s; sc += s * c; s1 += s;
        cc += c * c; c1 += c; n1 += 1.0;
        ys += y[i] * s; yc += y[i] * c; y1 += y[i];
    }
    /* Solve the symmetric 3x3 system with Cramer's rule */
    double m[3][3] = {{ss, sc, s1}, {sc, cc, c1}, {s1, c1, n1}};
    double r[3] = {ys, yc, y1};
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
               - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    double x[3];
    for (int k = 0; k < 3; k++) {
        double t[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                t[i][j] = (j == k) ? r[i] : m[i][j];
            }
        }
        x[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1])
              - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0])
              + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    double p_res = 0;
    for (size_t i = 0; i < n; i++) {
        double fit = x[0] * sin(omega * (double)i) + x[1] * cos(omega * (double)i) + x[2];
        double e = y[i] - fit;
        p_res += e * e;
    }
    p_res /= (double)n;
    double p_sig = (x[0] * x[0] + x[1] * x[1]) / 2.0;
    if (p_res <= 0) {
        return 300.0;
    }
    return 10.0 * log10(p_sig / p_res);
}
