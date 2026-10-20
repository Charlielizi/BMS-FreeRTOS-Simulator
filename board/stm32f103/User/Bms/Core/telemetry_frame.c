#include "telemetry_frame.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

uint16_t TelemetryFrame_Crc16(const uint8_t *p, unsigned length)
{
    uint16_t crc = 0xFFFFU;
    while (length--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void TelemetryFrame_Encode(uint8_t out[TELEMETRY_FRAME_SIZE], const TelemetryFrameData *d)
{
    memset(out, 0, TELEMETRY_FRAME_SIZE);
    out[0] = 0xA5U; out[1] = 0x5AU;
    out[2] = 1U; out[3] = 1U; out[4] = 40U;
    put16(out + 5, d->seq);
    put32(out + 7, d->ms);
    for (unsigned i = 0; i < 5; i++) put16(out + 11 + i * 2, d->cell_mv[i]);
    put16(out + 21, d->pack_mv);
    put16(out + 23, (uint16_t)d->current_ma);
    put16(out + 25, (uint16_t)d->temp_c10);
    put16(out + 27, d->soc_pct10);
    out[29] = d->mode;
    put16(out + 30, d->faults);
    out[32] = d->balance_mask;
    out[33] = d->mos_bits;
    out[34] = d->valid_mask;
    for (unsigned i = 0; i < 5; i++) put16(out + 37 + i * 2, d->age_ms[i]);
    put16(out + 47, TelemetryFrame_Crc16(out + 2, 45U));
}
