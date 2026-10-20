#include "sim_afe.h"

#include <math.h>
#include <string.h>

static int cell_mv(const SimAfe *afe, int index)
{
    if (afe->cell_override_mv[index]) return afe->cell_override_mv[index];
    return bms_ocv_mv(afe->cell_soc[index]);
}

void sim_afe_init(SimAfe *afe)
{
    const int initial_mv[BMS_CELLS] = {3700, 3710, 3690, 3740, 3720};
    memset(afe, 0, sizeof(*afe));
    for (int i = 0; i < BMS_CELLS; ++i) afe->cell_soc[i] = bms_soc_from_ocv(initial_mv[i]);
    afe->temp_c = 25;
}

void sim_afe_set_actuators(SimAfe *afe, const BmsStatus *status)
{
    afe->charge_on = status->charge_on;
    afe->discharge_on = status->discharge_on;
    afe->balance_mask = status->balance_mask;
}

void sim_afe_advance(SimAfe *afe, uint32_t dt_ms)
{
    if (dt_ms > 1000) dt_ms = 1000;
    if (afe->requested_ma > 0 && afe->charge_on) afe->actual_ma = afe->requested_ma;
    else if (afe->requested_ma < 0 && afe->discharge_on) afe->actual_ma = afe->requested_ma;
    else afe->actual_ma = 0;

    for (int i = 0; i < BMS_CELLS; ++i) {
        float balance_ma = 0.0f;
        if (afe->balance_mask & (1u << i)) {
            /* The original board uses a 39 ohm bleed resistor. */
            balance_ma = (float)cell_mv(afe, i) / 39.0f;
        }
        float delta_pct = ((float)afe->actual_ma - balance_ma) * (float)dt_ms * 100.0f /
                          ((float)BMS_CAPACITY_MAH * 3600000.0f);
        afe->cell_soc[i] += delta_pct;
        if (afe->cell_soc[i] < 0.0f) afe->cell_soc[i] = 0.0f;
        if (afe->cell_soc[i] > 100.0f) afe->cell_soc[i] = 100.0f;
    }
}

void sim_afe_sample(const SimAfe *afe, uint64_t ms, BmsSample *out)
{
    memset(out, 0, sizeof(*out));
    out->ms = ms;
    out->requested_ma = afe->requested_ma;
    out->actual_ma = afe->actual_ma;
    out->temp_c = afe->temp_c;
    out->injected_scd = afe->inject_scd;
    for (int i = 0; i < BMS_CELLS; ++i) {
        out->cell_mv[i] = cell_mv(afe, i);
        out->pack_mv += out->cell_mv[i];
    }
    out->error = afe->inject_nack ? BMS_SAMPLE_NACK : (afe->inject_crc ? BMS_SAMPLE_CRC : BMS_SAMPLE_OK);
    out->valid = out->error == BMS_SAMPLE_OK;
}
