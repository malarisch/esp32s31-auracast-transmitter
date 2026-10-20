/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "big_airtime.h"

#define PDU_OVERHEAD_BYTES 11   /* preamble 2, access address 4, header 2, CRC 3 */
#define US_PER_BYTE_2M     4
#define SUBEVENT_SPACE_US  150

uint32_t big_airtime_us(uint8_t bis, uint16_t sdu, uint8_t rtn)
{
    const uint32_t subevent = (PDU_OVERHEAD_BYTES + (uint32_t)sdu) * US_PER_BYTE_2M + SUBEVENT_SPACE_US;
    return (uint32_t)bis * ((uint32_t)rtn + 1) * subevent;
}

uint8_t big_rtn_fit(uint8_t bis, uint16_t sdu, uint8_t rtn_wanted)
{
    uint8_t rtn = rtn_wanted;
    while (rtn > 0 && big_airtime_us(bis, sdu, rtn) > BIG_AIRTIME_BUDGET_US) {
        rtn--;
    }
    return rtn;
}
