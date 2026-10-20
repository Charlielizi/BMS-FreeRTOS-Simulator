#ifndef SIM_AFE_H
#define SIM_AFE_H

#include "bms_core.h"

typedef struct {
    float cell_soc[BMS_CELLS];
    int cell_override_mv[BMS_CELLS];
    int requested_ma;
    int actual_ma;
    int temp_c;
    bool charge_on;
    bool discharge_on;
    uint8_t balance_mask;
    bool inject_scd;
    bool inject_nack;
    bool inject_crc;
} SimAfe;

void sim_afe_init(SimAfe *afe);
void sim_afe_advance(SimAfe *afe, uint32_t dt_ms);
void sim_afe_sample(const SimAfe *afe, uint64_t ms, BmsSample *out);
void sim_afe_set_actuators(SimAfe *afe, const BmsStatus *status);

#endif
