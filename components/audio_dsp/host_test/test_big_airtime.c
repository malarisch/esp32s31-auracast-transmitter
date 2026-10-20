/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "big_airtime.h"
#include "test_util.h"

/* Observed on the ESP32-S31 controller (release/v6.1) */
static void test_observed_accept_reject(void)
{
    CHECK(big_airtime_us(4, 120, 2) <= BIG_AIRTIME_BUDGET_US);   /* accepted */
    CHECK(big_airtime_us(3, 155, 2) <= BIG_AIRTIME_BUDGET_US);   /* accepted */
    CHECK(big_airtime_us(4, 155, 2) > BIG_AIRTIME_BUDGET_US);    /* rejected: Unsupported Value */
    CHECK(big_airtime_us(3, 155, 4) > BIG_AIRTIME_BUDGET_US);    /* rejected */
}

static void test_subevent_model(void)
{
    /* (2 + 4 + 2 + 155 + 3) bytes at 2 Mbit/s = 664 us, plus 150 us spacing; 3 subevents, 3 BIS */
    CHECK(big_airtime_us(3, 155, 2) == 3 * 3 * (664 + 150));
    CHECK(big_airtime_us(1, 60, 0) == 1 * 1 * ((11 + 60) * 4 + 150));
}

/* Encrypted BIS PDUs carry a 4-byte MIC: 16 us more per subevent on the 2M PHY */
static void test_encryption_mic(void)
{
    CHECK(big_airtime_us_enc(3, 155, 2, true) == big_airtime_us(3, 155, 2) + 3 * 3 * 16);
    CHECK(big_airtime_us_enc(3, 155, 2, false) == big_airtime_us(3, 155, 2));
    /* 2 BIS x 155 B x RTN 4 = 8140 us unencrypted, 8300 us encrypted: both still fit */
    CHECK(big_rtn_fit_enc(2, 155, 4, true) == 4);
    /* 4 BIS x 120 B x RTN 2: 8088 us unencrypted (fits), 8280 us encrypted (fits) */
    CHECK(big_rtn_fit_enc(4, 120, 2, true) == 2);
    CHECK(big_rtn_fit_enc(4, 155, 2, true) == 1);
}

static void test_rtn_fit(void)
{
    CHECK(big_rtn_fit(3, 155, 4) == 2);
    CHECK(big_rtn_fit(4, 155, 2) == 1);
    CHECK(big_rtn_fit(4, 120, 2) == 2);
    CHECK(big_rtn_fit(1, 60, 2) == 2);
    CHECK(big_rtn_fit(2, 155, 4) == 4);    /* 48_6_1 stereo alone: 8140 us */
    CHECK(big_rtn_fit(4, 155, 0) == 0);
}

int main(void)
{
    RUN_TEST(test_observed_accept_reject);
    RUN_TEST(test_subevent_model);
    RUN_TEST(test_rtn_fit);
    RUN_TEST(test_encryption_mic);
    TEST_MAIN_RETURN();
}
