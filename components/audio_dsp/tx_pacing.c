/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tx_pacing.h"

uint32_t tx_in_flight(uint16_t next_seq, uint16_t last_done_seq)
{
    uint16_t n = (uint16_t)(next_seq - 1u - last_done_seq);
    /* A completion newer than anything sent is inconsistent (e.g. after a restart): assume empty */
    return n >= 0x8000u ? 0u : n;
}

uint32_t tx_pairs_wanted(uint16_t next_seq, uint16_t last_done_seq, uint32_t depth)
{
    uint32_t n = tx_in_flight(next_seq, last_done_seq);
    return n >= depth ? 0u : depth - n;
}
