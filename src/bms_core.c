/*
 * Derived from PLKJ Development Team bms-education RT-Thread BMS modules.
 * Original source: CC BY-NC 4.0. See NOTICE and LICENSE-ORIGINAL.
 * The OCV table and default five-cell thresholds retain their source meaning.
 */
#include "bms_core.h"

#include <math.h>
#include <string.h>

static const int ocv_mv[101] = {
    3282,3309,3334,3357,3378,3398,3417,3434,3449,3464,3477,
    3489,3500,3510,3520,3528,3536,3543,3549,3555,3561,
    3566,3571,3575,3579,3583,3586,3590,3593,3596,3599,
    3602,3605,3608,3611,3615,3618,3621,3624,3628,3632,
    3636,3640,3644,3648,3653,3658,3663,3668,3674,3679,
    3685,3691,3698,3704,3711,3718,3725,3733,3741,3748,
    3756,3765,3773,3782,3791,3800,3809,3818,3827,3837,
    3847,3857,3867,3877,3887,3897,3908,3919,3929,3940,
    3951,3962,3973,3985,3996,4008,4019,4031,4043,4055,
    4067,4080,4092,4105,4118,4131,4145,4158,4172,4185
};

static uint64_t condition_since(uint64_t since, bool active, uint64_t now)
{
    if (!active) return 0;
    return since ? since : (now ? now : 1);
}

static bool elapsed(uint64_t since, uint64_t now, uint64_t delay)
{
    return since && now >= since && now - since >= delay;
}

static void set_fault(BmsCore *core, uint32_t bit, bool set)
{
    if (set) core->status.faults |= bit;
    else core->status.faults &= ~bit;
}

static void update_outputs(BmsCore *core)
{
    BmsStatus *s = &core->status;
    s->charge_on = s->charge_allowed && s->valid && core->charge_soc_gate && !(s->faults & BMS_CHARGE_FAULTS);
    s->discharge_on = s->discharge_allowed && s->valid && core->discharge_soc_gate && !(s->faults & BMS_DISCHARGE_FAULTS);
    if (s->mode == BMS_MODE_SLEEP) {
        s->charge_on = false;
        s->discharge_on = false;
    }
    if (!s->balance_allowed || !s->valid || s->faults ||
        (s->mode != BMS_MODE_STANDBY && s->mode != BMS_MODE_CHARGE)) {
        s->balance_mask = 0;
        core->balance_until_ms = 0;
    }
}

void bms_core_init(BmsCore *core)
{
    memset(core, 0, sizeof(*core));
    core->status.mode = BMS_MODE_STANDBY;
    core->status.soc_pct = 50.0f;
}

void bms_core_set_permissions(BmsCore *core, bool charge, bool discharge, bool balance)
{
    core->status.charge_allowed = charge;
    core->status.discharge_allowed = discharge;
    core->status.balance_allowed = balance;
    update_outputs(core);
}

int bms_ocv_mv(float soc_pct)
{
    if (soc_pct <= 0.0f) return ocv_mv[0];
    if (soc_pct >= 100.0f) return ocv_mv[100];
    int index = (int)soc_pct;
    float part = soc_pct - (float)index;
    return (int)lroundf((float)ocv_mv[index] + part * (float)(ocv_mv[index + 1] - ocv_mv[index]));
}

float bms_soc_from_ocv(int mv)
{
    if (mv <= ocv_mv[0]) return 0.0f;
    if (mv >= ocv_mv[100]) return 100.0f;
    for (int i = 0; i < 100; ++i) {
        if (mv <= ocv_mv[i + 1]) {
            return (float)i + (float)(mv - ocv_mv[i]) / (float)(ocv_mv[i + 1] - ocv_mv[i]);
        }
    }
    return 100.0f;
}

static void update_protection(BmsCore *core, const BmsSample *sample)
{
    BmsStatus *s = &core->status;
    int min_mv = sample->cell_mv[0], max_mv = sample->cell_mv[0];
    for (int i = 1; i < BMS_CELLS; ++i) {
        if (sample->cell_mv[i] < min_mv) min_mv = sample->cell_mv[i];
        if (sample->cell_mv[i] > max_mv) max_mv = sample->cell_mv[i];
    }

    core->ov_since_ms = condition_since(core->ov_since_ms, max_mv >= 4200, sample->ms);
    core->uv_since_ms = condition_since(core->uv_since_ms, min_mv <= 3100, sample->ms);
    core->occ_since_ms = condition_since(core->occ_since_ms, sample->actual_ma > 2200, sample->ms);
    core->ocd_since_ms = condition_since(core->ocd_since_ms, sample->actual_ma < -2200, sample->ms);

    if (elapsed(core->ov_since_ms, sample->ms, 2000)) set_fault(core, BMS_FAULT_OV, true);
    if (elapsed(core->uv_since_ms, sample->ms, 4000)) set_fault(core, BMS_FAULT_UV, true);
    if (elapsed(core->occ_since_ms, sample->ms, 1000)) set_fault(core, BMS_FAULT_OCC, true);
    if (elapsed(core->ocd_since_ms, sample->ms, 320)) set_fault(core, BMS_FAULT_OCD, true);
    if (max_mv <= 4180) set_fault(core, BMS_FAULT_OV, false);
    if (min_mv >= 3150) set_fault(core, BMS_FAULT_UV, false);

    core->occ_clear_since_ms = condition_since(core->occ_clear_since_ms, sample->actual_ma <= 2200, sample->ms);
    core->ocd_clear_since_ms = condition_since(core->ocd_clear_since_ms, sample->actual_ma >= -2200, sample->ms);
    core->scd_clear_since_ms = condition_since(core->scd_clear_since_ms, !sample->injected_scd, sample->ms);
    if (elapsed(core->occ_clear_since_ms, sample->ms, 60000)) set_fault(core, BMS_FAULT_OCC, false);
    if (elapsed(core->ocd_clear_since_ms, sample->ms, 60000)) set_fault(core, BMS_FAULT_OCD, false);
    if (sample->injected_scd) set_fault(core, BMS_FAULT_SCD, true);
    if (elapsed(core->scd_clear_since_ms, sample->ms, 60000)) set_fault(core, BMS_FAULT_SCD, false);

    if (sample->actual_ma > 20) {
        if (sample->temp_c >= 70) set_fault(core, BMS_FAULT_OTC, true);
        if (sample->temp_c <= -20) set_fault(core, BMS_FAULT_LTC, true);
    }
    if (sample->actual_ma < -20) {
        if (sample->temp_c >= 70) set_fault(core, BMS_FAULT_OTD, true);
        if (sample->temp_c <= -20) set_fault(core, BMS_FAULT_LTD, true);
    }
    if (sample->temp_c <= 60) {
        set_fault(core, BMS_FAULT_OTC, false);
        set_fault(core, BMS_FAULT_OTD, false);
    }
    if (sample->temp_c >= -10) {
        set_fault(core, BMS_FAULT_LTC, false);
        set_fault(core, BMS_FAULT_LTD, false);
    }
    (void)s;
}

static void update_balance(BmsCore *core, const BmsSample *sample)
{
    BmsStatus *s = &core->status;
    if (!s->balance_allowed || !s->valid || s->faults ||
        (s->mode != BMS_MODE_STANDBY && s->mode != BMS_MODE_CHARGE)) {
        s->balance_mask = 0;
        core->balance_until_ms = 0;
        return;
    }
    int min_mv = sample->cell_mv[0], max_mv = sample->cell_mv[0];
    for (int i = 1; i < BMS_CELLS; ++i) {
        if (sample->cell_mv[i] < min_mv) min_mv = sample->cell_mv[i];
        if (sample->cell_mv[i] > max_mv) max_mv = sample->cell_mv[i];
    }
    if (max_mv < 3300 || max_mv - min_mv < 50) {
        s->balance_mask = 0;
        core->balance_until_ms = 0;
        return;
    }
    if (s->balance_mask && sample->ms >= core->balance_until_ms) {
        s->balance_mask = 0;
        core->balance_until_ms = 0;
        core->balance_rest_until_ms = sample->ms + 5000;
    }
    if (s->balance_mask || sample->ms < core->balance_rest_until_ms) return;

    int used = 0;
    for (int rank = 0; rank < BMS_CELLS; ++rank) {
        int selected = -1;
        for (int i = 0; i < BMS_CELLS; ++i) {
            if (used & (1 << i)) continue;
            if (selected < 0 || sample->cell_mv[i] > sample->cell_mv[selected]) selected = i;
        }
        used |= 1 << selected;
        if (sample->cell_mv[selected] - min_mv < 50) break;
        if ((selected > 0 && (s->balance_mask & (1 << (selected - 1)))) ||
            (selected < BMS_CELLS - 1 && (s->balance_mask & (1 << (selected + 1))))) continue;
        s->balance_mask |= (uint8_t)(1 << selected);
    }
    if (s->balance_mask) core->balance_until_ms = sample->ms + 30000;
}

void bms_core_step(BmsCore *core, const BmsSample *sample)
{
    BmsStatus *s = &core->status;
    s->ms = sample->ms;
    s->requested_ma = sample->requested_ma;
    s->actual_ma = sample->actual_ma;
    s->error = sample->error;
    s->valid = sample->valid;

    if (!sample->valid) {
        core->valid_streak = 0;
        set_fault(core, BMS_FAULT_SENSOR, true);
        update_outputs(core);
        return;
    }

    if (core->valid_streak < 3) ++core->valid_streak;
    if (core->valid_streak >= 3) set_fault(core, BMS_FAULT_SENSOR, false);
    memcpy(s->cell_mv, sample->cell_mv, sizeof(s->cell_mv));
    s->pack_mv = sample->pack_mv;
    s->temp_c = sample->temp_c;

    if (!core->initialized_soc) {
        int total = 0;
        for (int i = 0; i < BMS_CELLS; ++i) total += sample->cell_mv[i];
        s->soc_pct = bms_soc_from_ocv(total / BMS_CELLS);
        core->initialized_soc = true;
        core->charge_soc_gate = s->soc_pct < 90.0f;
        core->discharge_soc_gate = s->soc_pct > 10.0f;
    } else if (sample->ms > core->last_soc_ms && sample->ms - core->last_soc_ms <= 5000) {
        double delta = (double)sample->actual_ma * (double)(sample->ms - core->last_soc_ms) * 100.0 /
                       ((double)BMS_CAPACITY_MAH * 3600000.0);
        s->soc_pct += (float)delta;
        if (s->soc_pct < 0) s->soc_pct = 0;
        if (s->soc_pct > 100) s->soc_pct = 100;
    }
    core->last_soc_ms = sample->ms;
    if (s->soc_pct >= 100.0f) core->charge_soc_gate = false;
    else if (s->soc_pct < 90.0f) core->charge_soc_gate = true;
    if (s->soc_pct <= 0.0f) core->discharge_soc_gate = false;
    else if (s->soc_pct > 10.0f) core->discharge_soc_gate = true;

    update_protection(core, sample);
    if (sample->actual_ma > 20) {
        s->mode = BMS_MODE_CHARGE;
        core->idle_since_ms = 0;
    } else if (sample->actual_ma < -20) {
        s->mode = BMS_MODE_DISCHARGE;
        core->idle_since_ms = 0;
    } else {
        if (!core->idle_since_ms) core->idle_since_ms = sample->ms ? sample->ms : 1;
        s->mode = sample->requested_ma && s->mode == BMS_MODE_SLEEP ? BMS_MODE_STANDBY :
                  (sample->ms - core->idle_since_ms >= 3600000 ? BMS_MODE_SLEEP : BMS_MODE_STANDBY);
    }
    update_outputs(core);
    update_balance(core, sample);
}

const char *bms_mode_name(BmsMode mode)
{
    switch (mode) {
        case BMS_MODE_CHARGE: return "CHARGE";
        case BMS_MODE_DISCHARGE: return "DISCHARGE";
        case BMS_MODE_SLEEP: return "SLEEP";
        default: return "STANDBY";
    }
}

const char *bms_error_name(BmsSampleError error)
{
    switch (error) {
        case BMS_SAMPLE_NACK: return "nack";
        case BMS_SAMPLE_CRC: return "crc";
        case BMS_SAMPLE_TIMEOUT: return "timeout";
        default: return "none";
    }
}
