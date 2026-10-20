#ifndef BMS_TELEMETRY_FRAME_H
#define BMS_TELEMETRY_FRAME_H

#include <stdint.h>

#define TELEMETRY_FRAME_SIZE 49U

typedef struct {
    uint16_t seq;
    uint32_t ms;
    uint16_t cell_mv[5];
    uint16_t pack_mv;
    int16_t current_ma;
    int16_t temp_c10;
    uint16_t soc_pct10;
    uint8_t mode;
    uint16_t faults;
    uint8_t balance_mask;
    uint8_t mos_bits;
    uint8_t valid_mask;
    uint16_t age_ms[5];
} TelemetryFrameData;

void TelemetryFrame_Encode(uint8_t out[TELEMETRY_FRAME_SIZE], const TelemetryFrameData *data);
uint16_t TelemetryFrame_Crc16(const uint8_t *bytes, unsigned length);

#endif
