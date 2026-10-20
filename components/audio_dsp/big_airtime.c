/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "big_airtime.h"

#define PDU_OVERHEAD_BYTES 11   /* preamble 2, access address 4, header 2, CRC 3 */
#define MIC_BYTES          4
#define US_PER_BYTE_2M     4
#define SUBEVENT_SPACE_US  150

uint32_t big_airtime_us_enc(uint8_t bis, uint16_t sdu, uint8_t rtn, bool encrypted)
{
    const uint32_t bytes = PDU_OVERHEAD_BYTES + (uint32_t)sdu + (encrypted ? MIC_BYTES : 0);
    const uint32_t subevent = bytes * US_PER_BYTE_2M + SUBEVENT_SPACE_US;
    return (uint32_t)bis * ((uint32_t)rtn + 1) * subevent;
}

uint32_t big_airtime_us(uint8_t bis, uint16_t sdu, uint8_t rtn)
{
    return big_airtime_us_enc(bis, sdu, rtn, false);
}

uint8_t big_rtn_fit_enc(uint8_t bis, uint16_t sdu, uint8_t rtn_wanted, bool encrypted)
{
    uint8_t rtn = rtn_wanted;
    while (rtn > 0 && big_airtime_us_enc(bis, sdu, rtn, encrypted) > BIG_AIRTIME_BUDGET_US) {
        rtn--;
    }
    return rtn;
}

uint8_t big_rtn_fit(uint8_t bis, uint16_t sdu, uint8_t rtn_wanted)
{
    return big_rtn_fit_enc(bis, sdu, rtn_wanted, false);
}
