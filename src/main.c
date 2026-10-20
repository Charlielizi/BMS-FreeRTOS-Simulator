#include <windows.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "bms_core.h"
#include "line_ring.h"
#include "protocol.h"
#include "sim_afe.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char line[256]; } Message;

static SimAfe plant;
static BmsCore core;
static BmsStatus shared_status;
static SemaphoreHandle_t plant_mutex;
static SemaphoreHandle_t status_mutex;
static QueueHandle_t sample_queue;
static QueueHandle_t message_queue;
static LineRing input_ring;
static LineRing output_ring;
static atomic_bool input_closed;
static atomic_ullong input_closed_at;

static uint64_t now_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

static void queue_message(const char *line)
{
    Message msg = {0};
    snprintf(msg.line, sizeof(msg.line), "%s", line);
    (void)xQueueSend(message_queue, &msg, 0);
}

static void send_ack(uint32_t id, bool ok, const char *error)
{
    char line[256];
    protocol_format_ack(line, sizeof(line), id, ok, error);
    queue_message(line);
}

static void send_event(const char *name, uint32_t value)
{
    char line[256];
    protocol_format_event(line, sizeof(line), now_ms(), name, value);
    queue_message(line);
}

static void publish_status(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    shared_status = core.status;
    xSemaphoreGive(status_mutex);
}

static void apply_actuators(void)
{
    xSemaphoreTake(plant_mutex, portMAX_DELAY);
    sim_afe_set_actuators(&plant, &core.status);
    xSemaphoreGive(plant_mutex);
}

static void handle_command(const Command *cmd)
{
    switch (cmd->type) {
        case CMD_SET_CURRENT:
            xSemaphoreTake(plant_mutex, portMAX_DELAY);
            plant.requested_ma = cmd->value;
            xSemaphoreGive(plant_mutex);
            break;
        case CMD_SET_CELL:
            xSemaphoreTake(plant_mutex, portMAX_DELAY);
            plant.cell_override_mv[cmd->index - 1] = cmd->clear ? 0 : cmd->value;
            xSemaphoreGive(plant_mutex);
            if (core.status.balance_mask) {
                bool charge = core.status.charge_allowed;
                bool discharge = core.status.discharge_allowed;
                bool balance = core.status.balance_allowed;
                bms_core_set_permissions(&core, charge, discharge, false);
                bms_core_set_permissions(&core, charge, discharge, balance);
                apply_actuators();
                publish_status();
                send_event("balance", 0);
            }
            break;
        case CMD_SET_TEMP:
            xSemaphoreTake(plant_mutex, portMAX_DELAY);
            plant.temp_c = cmd->value;
            xSemaphoreGive(plant_mutex);
            break;
        case CMD_SET_ENABLE:
        {
            uint8_t old_balance = core.status.balance_mask;
            bool old_charge = core.status.charge_on;
            bool old_discharge = core.status.discharge_on;
            bms_core_set_permissions(&core, cmd->charge, cmd->discharge, cmd->balance);
            apply_actuators();
            publish_status();
            if (old_balance != core.status.balance_mask) send_event("balance", core.status.balance_mask);
            if (old_charge != core.status.charge_on || old_discharge != core.status.discharge_on) {
                send_event("mos", (core.status.charge_on ? 1u : 0u) |
                                  (core.status.discharge_on ? 2u : 0u));
            }
            break;
        }
        case CMD_INJECT_FAULT:
            xSemaphoreTake(plant_mutex, portMAX_DELAY);
            if (cmd->inject == INJECT_SCD) plant.inject_scd = cmd->on;
            else if (cmd->inject == INJECT_NACK) plant.inject_nack = cmd->on;
            else plant.inject_crc = cmd->on;
            xSemaphoreGive(plant_mutex);
            break;
        case CMD_RESET:
            xSemaphoreTake(plant_mutex, portMAX_DELAY);
            sim_afe_init(&plant);
            xSemaphoreGive(plant_mutex);
            bms_core_init(&core);
            xQueueReset(sample_queue);
            publish_status();
            send_event("reset", 0);
            break;
        case CMD_PING:
            break;
    }
    send_ack(cmd->id, true, "");
}

static void plant_task(void *argument)
{
    (void)argument;
    TickType_t last = xTaskGetTickCount();
    uint64_t previous_ms = now_ms();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
        uint64_t current_ms = now_ms();
        xSemaphoreTake(plant_mutex, portMAX_DELAY);
        sim_afe_advance(&plant, (uint32_t)(current_ms - previous_ms));
        xSemaphoreGive(plant_mutex);
        previous_ms = current_ms;
    }
}

static void acquisition_task(void *argument)
{
    (void)argument;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(250));
        BmsSample sample;
        xSemaphoreTake(plant_mutex, portMAX_DELAY);
        sim_afe_sample(&plant, now_ms(), &sample);
        xSemaphoreGive(plant_mutex);
        xQueueOverwrite(sample_queue, &sample);
    }
}

static void control_task(void *argument)
{
    (void)argument;
    char line[LINE_SIZE];
    BmsSample sample;
    uint64_t last_sample_arrival_ms = now_ms();
    bool stale_reported = false;
    for (;;) {
        for (int i = 0; i < 32 && line_ring_pop(&input_ring, line); ++i) {
            Command cmd;
            char error[80];
            if (protocol_parse_command(line, &cmd, error, sizeof(error))) handle_command(&cmd);
            else send_ack(cmd.id, false, error);
        }
        if (xQueueReceive(sample_queue, &sample, 0) == pdPASS) {
            last_sample_arrival_ms = now_ms();
            stale_reported = false;
            uint32_t old_faults = core.status.faults;
            uint8_t old_balance = core.status.balance_mask;
            BmsMode old_mode = core.status.mode;
            bool old_charge = core.status.charge_on;
            bool old_discharge = core.status.discharge_on;
            bms_core_step(&core, &sample);
            apply_actuators();
            publish_status();
            if (old_faults != core.status.faults) send_event("faults", core.status.faults);
            if (old_balance != core.status.balance_mask) send_event("balance", core.status.balance_mask);
            if (old_mode != core.status.mode) send_event("mode", (uint32_t)core.status.mode);
            if (old_charge != core.status.charge_on || old_discharge != core.status.discharge_on) {
                send_event("mos", (core.status.charge_on ? 1u : 0u) | (core.status.discharge_on ? 2u : 0u));
            }
        } else if (!stale_reported && now_ms() - last_sample_arrival_ms > 750) {
            BmsSample timeout_sample = {0};
            timeout_sample.ms = now_ms();
            timeout_sample.requested_ma = core.status.requested_ma;
            timeout_sample.error = BMS_SAMPLE_TIMEOUT;
            bms_core_step(&core, &timeout_sample);
            apply_actuators();
            publish_status();
            send_event("faults", core.status.faults);
            stale_reported = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void telemetry_task(void *argument)
{
    (void)argument;
    uint32_t seq = 0;
    uint64_t last_snapshot_ms = 0;
    Message msg;
    char line[LINE_SIZE];
    for (;;) {
        while (xQueueReceive(message_queue, &msg, 0) == pdPASS) {
            (void)line_ring_push(&output_ring, msg.line);
        }
        if (!last_snapshot_ms || now_ms() - last_snapshot_ms >= 200) {
            BmsStatus status;
            xSemaphoreTake(status_mutex, portMAX_DELAY);
            status = shared_status;
            xSemaphoreGive(status_mutex);
            protocol_format_snapshot(line, sizeof(line), ++seq, &status);
            (void)line_ring_push(&output_ring, line);
            last_snapshot_ms = now_ms();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static DWORD WINAPI input_thread(LPVOID argument)
{
    (void)argument;
    char line[LINE_SIZE];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[len - 1] = 0;
        while (!line_ring_push(&input_ring, line)) Sleep(5);
    }
    atomic_store(&input_closed_at, (unsigned long long)GetTickCount64());
    atomic_store(&input_closed, true);
    return 0;
}

static DWORD WINAPI output_thread(LPVOID argument)
{
    (void)argument;
    char line[LINE_SIZE];
    for (;;) {
        if (line_ring_pop(&output_ring, line)) {
            fputs(line, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        } else Sleep(5);
        if (atomic_load(&input_closed) &&
            GetTickCount64() - atomic_load(&input_closed_at) >= 1000) ExitProcess(0);
    }
}

void vApplicationMallocFailedHook(void)
{
    fputs("FreeRTOS heap exhausted\n", stderr);
    ExitProcess(2);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    fprintf(stderr, "FreeRTOS stack overflow: %s\n", name);
    ExitProcess(3);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    sim_afe_init(&plant);
    bms_core_init(&core);
    shared_status = core.status;
    plant_mutex = xSemaphoreCreateMutex();
    status_mutex = xSemaphoreCreateMutex();
    sample_queue = xQueueCreate(1, sizeof(BmsSample));
    message_queue = xQueueCreate(64, sizeof(Message));
    if (!plant_mutex || !status_mutex || !sample_queue || !message_queue) return 2;
    HANDLE reader = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    HANDLE writer = CreateThread(NULL, 0, output_thread, NULL, 0, NULL);
    if (!reader || !writer) return 2;
    CloseHandle(reader);
    CloseHandle(writer);
    if (xTaskCreate(plant_task, "Plant", 2048, NULL, 2, NULL) != pdPASS ||
        xTaskCreate(acquisition_task, "Acquisition", 2048, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(control_task, "Control", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(telemetry_task, "Telemetry", 4096, NULL, 1, NULL) != pdPASS) return 2;
    vTaskStartScheduler();
    fputs("FreeRTOS scheduler failed\n", stderr);
    return 2;
}
