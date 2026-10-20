#include "telemetry_frame.h"

#include <stdio.h>

int main(void)
{
    const TelemetryFrameData data = {
        .seq = 7, .ms = 1200,
        .cell_mv = {3700, 3710, 3720, 3730, 3740},
        .pack_mv = 18600, .current_ma = -850, .temp_c10 = 285,
        .soc_pct10 = 736, .mode = 2, .faults = (1U << 4) | (1U << 9),
        .balance_mask = 5, .mos_bits = 2, .valid_mask = 0x7F,
        .age_ms = {30, 40, 600, 1900, 150},
    };
    uint8_t frame[TELEMETRY_FRAME_SIZE];
    TelemetryFrame_Encode(frame, &data);
    for (unsigned i = 0; i < TELEMETRY_FRAME_SIZE; i++) printf("%02x", frame[i]);
    putchar('\n');
    return 0;
}
