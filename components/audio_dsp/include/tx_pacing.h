/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SDUs still queued in the controller, from the next sequence number to be sent and the
 * newest sequence number the controller reported as completed (16-bit, wrapping).
 */
uint32_t tx_in_flight(uint16_t next_seq, uint16_t last_done_seq);

/* SDUs to send now to keep `depth` queued */
uint32_t tx_pairs_wanted(uint16_t next_seq, uint16_t last_done_seq, uint32_t depth);

#ifdef __cplusplus
}
#endif
