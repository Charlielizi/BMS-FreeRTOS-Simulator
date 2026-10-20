#ifndef BMS_CORE_H
#define BMS_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define BMS_CELLS 5
#define BMS_CAPACITY_MAH 2200

enum {
    BMS_FAULT_OV     = 1u << 0,
    BMS_FAULT_OCC    = 1u << 1,
    BMS_FAULT_OTC    = 1u << 2,
    BMS_FAULT_LTC    = 1u << 3,
    BMS_FAULT_UV     = 1u << 4,
    BMS_FAULT_OCD    = 1u << 5,
    BMS_FAULT_SCD    = 1u << 6,
    BMS_FAULT_OTD    = 1u << 7,
    BMS_FAULT_LTD    = 1u << 8,
    BMS_FAULT_SENSOR = 1u << 9
};

#define BMS_CHARGE_FAULTS (BMS_FAULT_OV | BMS_FAULT_OCC | BMS_FAULT_OTC | BMS_FAULT_LTC | BMS_FAULT_SENSOR)
#define BMS_DISCHARGE_FAULTS (BMS_FAULT_UV | BMS_FAULT_OCD | BMS_FAULT_SCD | BMS_FAULT_OTD | BMS_FAULT_LTD | BMS_FAULT_SENSOR)

typedef enum {
    BMS_MODE_STANDBY,
    BMS_MODE_CHARGE,
    BMS_MODE_DISCHARGE,
    BMS_MODE_SLEEP
} BmsMode;

typedef enum {
    BMS_SAMPLE_OK,
    BMS_SAMPLE_NACK,
    BMS_SAMPLE_CRC,
    BMS_SAMPLE_TIMEOUT
} BmsSampleError;

typedef struct {
    uint64_t ms;
    int cell_mv[BMS_CELLS];
    int pack_mv;
    int requested_ma;
    int actual_ma;
    int temp_c;
    bool valid;
    bool injected_scd;
    BmsSampleError error;
} BmsSample;

typedef struct {
    uint64_t ms;
    int cell_mv[BMS_CELLS];
    int pack_mv;
    int requested_ma;
    int actual_ma;
    int temp_c;
    float soc_pct;
    BmsMode mode;
    uint32_t faults;
    uint8_t balance_mask;
    bool charge_allowed;
    bool discharge_allowed;
    bool balance_allowed;
    bool charge_on;
    bool discharge_on;
    bool valid;
    BmsSampleError error;
} BmsStatus;

typedef struct {
    BmsStatus status;
    bool initialized_soc;
    bool charge_soc_gate;
    bool discharge_soc_gate;
    uint64_t last_soc_ms;
    uint64_t idle_since_ms;
    uint64_t ov_since_ms;
    uint64_t uv_since_ms;
    uint64_t occ_since_ms;
    uint64_t ocd_since_ms;
    uint64_t occ_clear_since_ms;
    uint64_t ocd_clear_since_ms;
    uint64_t scd_clear_since_ms;
    uint64_t balance_until_ms;
    uint64_t balance_rest_until_ms;
    unsigned valid_streak;
} BmsCore;

void bms_core_init(BmsCore *core);
void bms_core_set_permissions(BmsCore *core, bool charge, bool discharge, bool balance);
void bms_core_step(BmsCore *core, const BmsSample *sample);
int bms_ocv_mv(float soc_pct);
float bms_soc_from_ocv(int mv);
const char *bms_mode_name(BmsMode mode);
const char *bms_error_name(BmsSampleError error);

#endif
