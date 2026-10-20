#include "telemetry.h"
#include "telemetry_frame.h"

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "bms_analysis.h"
#include "bms_energy.h"
#include "bms_global.h"
#include "bms_monitor.h"
#include "bms_protect.h"
#include "drv_softi2c_bq769x0.h"

#include <limits.h>
/* Wire format is defined in docs/BOARD_SERIAL_PROTOCOL.md. */
#define PERIOD_MS 200U

static volatile uint8_t valid_mask;
static volatile uint32_t sample_tick[5];
static volatile uint8_t mos_readback;
static volatile uint8_t balance_readback;
static volatile uint32_t output_tick;
static uint16_t sequence;

static const osThreadAttr_t telemetry_attributes = {
    .name = "telemetry", .stack_size = 768U, .priority = osPriorityLow
};

static uint16_t clamp_u16(float value)
{
    if (!(value >= 0.0f)) return 0;
    if (value >= 65535.0f) return UINT16_MAX;
    return (uint16_t)(value + 0.5f);
}

static int16_t clamp_i16(float value)
{
    if (!(value >= -32768.0f)) return INT16_MIN;
    if (value >= 32767.0f) return INT16_MAX;
    return (int16_t)(value >= 0 ? value + 0.5f : value - 0.5f);
}

static uint16_t fault_mask(BMS_ProtectAlertTypedef alert)
{
    uint16_t result = 0;
    if (alert & FlAG_ALERT_OV)  result |= 1U << 0;
    if (alert & FlAG_ALERT_OCC) result |= 1U << 1;
    if (alert & FlAG_ALERT_OTC) result |= 1U << 2;
    if (alert & FlAG_ALERT_LTC) result |= 1U << 3;
    if (alert & FlAG_ALERT_UV)  result |= 1U << 4;
    if (alert & FlAG_ALERT_OCD) result |= 1U << 5;
    if (alert & FlAG_ALERT_SCD) result |= 1U << 6;
    if (alert & FlAG_ALERT_OTD) result |= 1U << 7;
    if (alert & FlAG_ALERT_LTD) result |= 1U << 8;
    return result;
}

void Telemetry_NoteSample(unsigned bit, bool success)
{
    if (bit >= 7U) return;
    taskENTER_CRITICAL();
    if (success) {
        valid_mask |= (uint8_t)(1U << bit);
        if (bit < 5U) sample_tick[bit] = osKernelGetTickCount();
    } else {
        valid_mask &= (uint8_t)~(1U << bit);
    }
    taskEXIT_CRITICAL();
}

void Telemetry_NoteSoc(void)
{
    uint32_t now = osKernelGetTickCount();
    taskENTER_CRITICAL();
    bool inputs_valid = (valid_mask & 0x05U) == 0x05U &&
                        now - sample_tick[TELEMETRY_CELLS] <= 750U &&
                        now - sample_tick[TELEMETRY_CURRENT] <= 1500U;
    taskEXIT_CRITICAL();
    Telemetry_NoteSample(TELEMETRY_SOC, inputs_valid);
}

void Telemetry_RefreshOutputs(void)
{
    uint8_t mos = 0, balance = 0;
    bool ok = BQ769X0_ReadOutputStatus(&mos, &balance);
    taskENTER_CRITICAL();
    if (ok) {
        mos_readback = mos & 0x03U;
        balance_readback = balance & 0x1FU;
        output_tick = osKernelGetTickCount();
    }
    taskEXIT_CRITICAL();
    Telemetry_NoteSample(TELEMETRY_MOS, ok);
    Telemetry_NoteSample(TELEMETRY_BALANCE, ok);
}

static void encode_frame(uint8_t frame[TELEMETRY_FRAME_SIZE])
{
    uint32_t now = osKernelGetTickCount();
    float cells[5], pack, current, temp, soc;
    BMS_SysModeTypedef mode;
    BMS_ProtectAlertTypedef alert;
    uint8_t mask, mos, balance;
    uint32_t ticks[5], last_output_tick;

    taskENTER_CRITICAL();
    for (unsigned i = 0; i < 5; i++) cells[i] = BMS_MonitorData.CellVoltage[i];
    pack = BMS_MonitorData.BatteryVoltage;
    current = BMS_MonitorData.BatteryCurrent;
    temp = BMS_MonitorData.CellTemp[0];
    soc = BMS_AnalysisData.SOC;
    mode = BMS_GlobalParam.SysMode;
    alert = BMS_ProtectAlert;
    mask = valid_mask;
    mos = mos_readback;
    balance = balance_readback;
    for (unsigned i = 0; i < 5; i++) ticks[i] = sample_tick[i];
    last_output_tick = output_tick;
    taskEXIT_CRITICAL();

    const uint16_t max_age[5] = {750U, 750U, 1500U, 5000U, 2000U};
    for (unsigned i = 0; i < 5; i++) {
        if (now - ticks[i] > max_age[i]) mask &= (uint8_t)~(1U << i);
    }
    if (now - last_output_tick > 750U) mask &= (uint8_t)~((1U << 5) | (1U << 6));
    uint16_t faults = fault_mask(alert);
    if ((mask & 0x0FU) != 0x0FU) faults |= 1U << 9;
    TelemetryFrameData data = {0};
    data.seq = sequence++;
    data.ms = now;
    for (unsigned i = 0; i < 5; i++) data.cell_mv[i] = clamp_u16(cells[i] * 1000.0f);
    data.pack_mv = clamp_u16(pack * 1000.0f);
    data.current_ma = clamp_i16(current * 1000.0f);
    data.temp_c10 = clamp_i16(temp * 10.0f);
    data.soc_pct10 = clamp_u16(soc * 1000.0f);
    data.mode = mode == BMS_MODE_CHARGE ? 1U : mode == BMS_MODE_DISCHARGE ? 2U :
                mode == BMS_MODE_SLEEP ? 3U : mode == BMS_MODE_STANDBY ? 0U : 255U;
    data.faults = faults;
    data.balance_mask = balance;
    data.mos_bits = mos;
    data.valid_mask = mask;
    for (unsigned i = 0; i < 5; i++) {
        uint32_t age = (mask & (1U << i)) ? now - ticks[i] : UINT16_MAX;
        data.age_ms[i] = age > UINT16_MAX ? UINT16_MAX : (uint16_t)age;
    }
    TelemetryFrame_Encode(frame, &data);
}

static void telemetry_task(void *argument)
{
    (void)argument;
    uint8_t frame[TELEMETRY_FRAME_SIZE];
    for (;;) {
        encode_frame(frame);
        /* Bounded to this low-priority task; no I/O is performed by the protection task. */
        (void)HAL_UART_Transmit(&huart1, frame, TELEMETRY_FRAME_SIZE, 25U);
        osDelay(PERIOD_MS);
    }
}

void Telemetry_Init(void)
{
    (void)osThreadNew(telemetry_task, NULL, &telemetry_attributes);
}
