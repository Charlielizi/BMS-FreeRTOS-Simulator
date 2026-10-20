#include "bms_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static BmsSample sample_at(uint64_t ms)
{
    BmsSample s = {0};
    s.ms = ms;
    s.valid = true;
    s.temp_c = 25;
    for (int i = 0; i < BMS_CELLS; ++i) {
        s.cell_mv[i] = 3700;
        s.pack_mv += 3700;
    }
    return s;
}

static void test_fault_arbitration(void)
{
    BmsCore c;
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, false);
    BmsSample s = sample_at(1000);
    bms_core_step(&c, &s);
    assert(c.status.charge_on && c.status.discharge_on);
    s.cell_mv[0] = 4250;
    s.temp_c = 75;
    s.actual_ma = 1000;
    for (s.ms = 1250; s.ms <= 3500; s.ms += 250) bms_core_step(&c, &s);
    assert((c.status.faults & (BMS_FAULT_OV | BMS_FAULT_OTC)) == (BMS_FAULT_OV | BMS_FAULT_OTC));
    assert(!c.status.charge_on && c.status.discharge_on);
    s.temp_c = 60;
    s.ms = 3750;
    bms_core_step(&c, &s);
    assert((c.status.faults & BMS_FAULT_OV) && !(c.status.faults & BMS_FAULT_OTC));
    assert(!c.status.charge_on);
    s.cell_mv[0] = 4170;
    s.ms = 4000;
    bms_core_step(&c, &s);
    assert(!(c.status.faults & BMS_FAULT_OV) && c.status.charge_on);
}

static void test_sensor_recovery(void)
{
    BmsCore c;
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, true);
    BmsSample s = sample_at(1000);
    bms_core_step(&c, &s);
    s.ms = 1250;
    s.valid = false;
    s.error = BMS_SAMPLE_CRC;
    bms_core_step(&c, &s);
    assert((c.status.faults & BMS_FAULT_SENSOR) && !c.status.charge_on && !c.status.discharge_on);
    for (int i = 0; i < 2; ++i) {
        s.ms += 250;
        s.valid = true;
        s.error = BMS_SAMPLE_OK;
        bms_core_step(&c, &s);
        assert(c.status.faults & BMS_FAULT_SENSOR);
    }
    s.ms += 250;
    bms_core_step(&c, &s);
    assert(!(c.status.faults & BMS_FAULT_SENSOR) && c.status.charge_on);
}

static void test_balance_exit(void)
{
    BmsCore c;
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, true);
    BmsSample s = sample_at(1000);
    const int cells[5] = {3800, 3700, 3780, 3710, 3760};
    memcpy(s.cell_mv, cells, sizeof(cells));
    bms_core_step(&c, &s);
    assert(c.status.balance_mask == 0x15);
    bms_core_set_permissions(&c, true, true, false);
    assert(c.status.balance_mask == 0);
    bms_core_set_permissions(&c, true, true, true);
    s.ms = 1250;
    s.actual_ma = -1000;
    bms_core_step(&c, &s);
    assert(c.status.balance_mask == 0);
}

static void test_soc_elapsed_time(void)
{
    BmsCore c;
    bms_core_init(&c);
    BmsSample s = sample_at(1000);
    bms_core_step(&c, &s);
    float initial = c.status.soc_pct;
    s.actual_ma = 1000;
    for (int i = 1; i <= 60; ++i) {
        s.ms = 1000 + (uint64_t)i * 1000;
        bms_core_step(&c, &s);
    }
    float increase = c.status.soc_pct - initial;
    assert(increase > 0.75f && increase < 0.77f);
}

static void test_afe_faults(void)
{
    BmsCore c;
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, false);
    BmsSample s = sample_at(1000);
    bms_core_step(&c, &s);
    s.cell_mv[0] = 3000;
    for (s.ms = 1250; s.ms <= 5750; s.ms += 250) bms_core_step(&c, &s);
    assert((c.status.faults & BMS_FAULT_UV) && !c.status.discharge_on);
    s.cell_mv[0] = 3200;
    s.ms = 6000;
    bms_core_step(&c, &s);
    assert(!(c.status.faults & BMS_FAULT_UV));
    s.injected_scd = true;
    s.ms = 6250;
    bms_core_step(&c, &s);
    assert((c.status.faults & BMS_FAULT_SCD) && !c.status.discharge_on);
    s.injected_scd = false;
    s.ms = 6500;
    bms_core_step(&c, &s);
    assert(c.status.faults & BMS_FAULT_SCD);
    s.ms = 66500;
    bms_core_step(&c, &s);
    assert(!(c.status.faults & BMS_FAULT_SCD) && c.status.discharge_on);
}

static void test_current_and_temperature_faults(void)
{
    BmsCore c;
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, false);
    BmsSample s = sample_at(1000);
    bms_core_step(&c, &s);
    s.actual_ma = 3000;
    for (s.ms = 1250; s.ms <= 2500; s.ms += 250) bms_core_step(&c, &s);
    assert((c.status.faults & BMS_FAULT_OCC) && !c.status.charge_on);
    s.temp_c = 75;
    s.actual_ma = -3000;
    for (s.ms = 2750; s.ms <= 3500; s.ms += 250) bms_core_step(&c, &s);
    assert((c.status.faults & BMS_FAULT_OCD) && (c.status.faults & BMS_FAULT_OTD));
    assert(!c.status.discharge_on);
    s.temp_c = 60;
    s.actual_ma = 0;
    s.ms = 3750;
    bms_core_step(&c, &s);
    assert(!(c.status.faults & BMS_FAULT_OTD));
    assert(c.status.faults & BMS_FAULT_OCD);
    s.ms = 64000;
    bms_core_step(&c, &s);
    assert(!(c.status.faults & (BMS_FAULT_OCC | BMS_FAULT_OCD)));
}

static void test_soc_hysteresis(void)
{
    BmsCore c;
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, false);
    BmsSample s = sample_at(1000);
    for (int i = 0; i < BMS_CELLS; ++i) s.cell_mv[i] = 4100;
    bms_core_step(&c, &s);
    assert(!c.status.charge_on && c.status.discharge_on);
    bms_core_init(&c);
    bms_core_set_permissions(&c, true, true, false);
    s = sample_at(1000);
    for (int i = 0; i < BMS_CELLS; ++i) s.cell_mv[i] = 3300;
    bms_core_step(&c, &s);
    assert(c.status.charge_on && !c.status.discharge_on);
}

int main(void)
{
    test_fault_arbitration();
    test_sensor_recovery();
    test_balance_exit();
    test_soc_elapsed_time();
    test_afe_faults();
    test_current_and_temperature_faults();
    test_soc_hysteresis();
    puts("core scenarios passed");
    return 0;
}
