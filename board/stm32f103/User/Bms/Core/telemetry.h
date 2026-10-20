#ifndef BMS_BOARD_TELEMETRY_H
#define BMS_BOARD_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

enum {
    TELEMETRY_CELLS = 0,
    TELEMETRY_PACK = 1,
    TELEMETRY_CURRENT = 2,
    TELEMETRY_TEMP = 3,
    TELEMETRY_SOC = 4,
    TELEMETRY_MOS = 5,
    TELEMETRY_BALANCE = 6,
};

void Telemetry_Init(void);
void Telemetry_NoteSample(unsigned bit, bool success);
void Telemetry_NoteSoc(void);
void Telemetry_RefreshOutputs(void);

#endif
