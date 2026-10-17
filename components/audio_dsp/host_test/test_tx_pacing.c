/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDU pacing from sequence numbers: in flight = newest sent - newest completed.
 * A lost or late completion callback must correct itself at the next callback.
 */
#include <stdint.h>

#include "test_util.h"
#include "tx_pacing.h"

#define DEPTH 2

/* Steady state: one SDU queued after a completion -> top up with one */
static void test_steady_state(void)
{
    CHECK(tx_in_flight(301, 299) == 1);
    CHECK(tx_pairs_wanted(301, 299, DEPTH) == 1);
}

/* Nothing sent yet (last_done initialised to next - 1) -> prime the full depth */
static void test_prime(void)
{
    CHECK(tx_in_flight(0, 65535) == 0);
    CHECK(tx_pairs_wanted(0, 65535, DEPTH) == DEPTH);
}

/* A lost callback: the next one reports a newer sequence number, the count is right again */
static void test_lost_callback_heals(void)
{
    /* sent up to 310 (next 311); callback for 308 was lost, callback for 309 arrives */
    CHECK(tx_in_flight(311, 309) == 1);
    CHECK(tx_pairs_wanted(311, 309, DEPTH) == 1);
}

/* Over-queued (e.g. after a spurious refill): wait until the controller drains */
static void test_over_queued_waits(void)
{
    CHECK(tx_in_flight(305, 299) == 5);
    CHECK(tx_pairs_wanted(305, 299, DEPTH) == 0);
}

static void test_wraparound(void)
{
    CHECK(tx_in_flight(1, 65534) == 2);
    CHECK(tx_pairs_wanted(1, 65534, DEPTH) == 0);
    CHECK(tx_in_flight(0, 65534) == 1);
}

/* A completion "newer" than anything sent is inconsistent: treat the queue as empty */
static void test_inconsistent_treated_empty(void)
{
    CHECK(tx_in_flight(100, 200) == 0);
    CHECK(tx_pairs_wanted(100, 200, DEPTH) == DEPTH);
}

int main(void)
{
    RUN_TEST(test_steady_state);
    RUN_TEST(test_prime);
    RUN_TEST(test_lost_callback_heals);
    RUN_TEST(test_over_queued_waits);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_inconsistent_treated_empty);
    TEST_MAIN_RETURN();
}
