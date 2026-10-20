/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Airtime model of a BIG on the 2M PHY, unframed, ISO interval 10 ms, BN 1: every BIS gets (RTN + 1)
 * subevents per interval, each sized for Max_PDU (preamble, access address, header, payload, CRC, plus
 * the 150 us sub-event spacing). The ESP32-S31 controller accepted 4 BIS x 120 B at RTN 2 (8.1 ms by
 * this model) and rejected 4 BIS x 155 B at RTN 2 (9.8 ms) and 3 BIS x 155 B at RTN 4 (12.2 ms), hence
 * the budget.
 */
#define BIG_AIRTIME_BUDGET_US 8500

/* Microseconds per ISO interval for `bis` BIS with Max_SDU `sdu` octets and `rtn` retransmissions */
uint32_t big_airtime_us(uint8_t bis, uint16_t sdu, uint8_t rtn);

/* The largest RTN <= rtn_wanted whose airtime fits the budget (0 if even that does not fit) */
uint8_t big_rtn_fit(uint8_t bis, uint16_t sdu, uint8_t rtn_wanted);

#ifdef __cplusplus
}
#endif
