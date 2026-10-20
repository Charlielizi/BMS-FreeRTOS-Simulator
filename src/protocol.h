#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "bms_core.h"
#include "line_ring.h"

typedef enum {
    CMD_SET_CURRENT,
    CMD_SET_CELL,
    CMD_SET_TEMP,
    CMD_SET_ENABLE,
    CMD_INJECT_FAULT,
    CMD_RESET,
    CMD_PING
} CommandType;

typedef enum {
    INJECT_SCD,
    INJECT_NACK,
    INJECT_CRC
} InjectKind;

typedef struct {
    uint32_t id;
    CommandType type;
    int value;
    int index;
    bool clear;
    bool charge;
    bool discharge;
    bool balance;
    bool on;
    InjectKind inject;
} Command;

bool protocol_parse_command(const char *line, Command *command, char *error, unsigned error_len);
void protocol_format_ack(char *out, unsigned len, uint32_t id, bool ok, const char *error);
void protocol_format_event(char *out, unsigned len, uint64_t ms, const char *name, uint32_t value);
void protocol_format_snapshot(char *out, unsigned len, uint32_t seq, const BmsStatus *status);

#endif
