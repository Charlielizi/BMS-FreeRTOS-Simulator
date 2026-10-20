#include "protocol.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[32];
    char value[64];
    char kind;
} Field;

static void skip_space(const char **p)
{
    while (**p && isspace((unsigned char)**p)) ++*p;
}

static bool quoted(const char **p, char *out, size_t size)
{
    if (**p != '"') return false;
    ++*p;
    size_t n = 0;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)**p;
        if (c < 0x20 || c == '\\' || n + 1 >= size) return false;
        out[n++] = (char)c;
        ++*p;
    }
    if (**p != '"') return false;
    ++*p;
    out[n] = 0;
    return true;
}

static bool parse_fields(const char *p, Field *fields, int *count)
{
    *count = 0;
    skip_space(&p);
    if (*p++ != '{') return false;
    skip_space(&p);
    if (*p == '}') { ++p; skip_space(&p); return *p == 0; }
    for (;;) {
        if (*count >= 16 || !quoted(&p, fields[*count].key, sizeof(fields[*count].key))) return false;
        skip_space(&p);
        if (*p++ != ':') return false;
        skip_space(&p);
        Field *f = &fields[*count];
        if (*p == '"') {
            f->kind = 's';
            if (!quoted(&p, f->value, sizeof(f->value))) return false;
        } else {
            f->kind = 'n';
            size_t n = 0;
            while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
                if (n + 1 >= sizeof(f->value)) return false;
                f->value[n++] = *p++;
            }
            if (!n) return false;
            f->value[n] = 0;
        }
        for (int i = 0; i < *count; ++i) if (!strcmp(fields[i].key, f->key)) return false;
        ++*count;
        skip_space(&p);
        if (*p == '}') { ++p; skip_space(&p); return *p == 0; }
        if (*p++ != ',') return false;
        skip_space(&p);
    }
}

static const Field *field(const Field *fields, int count, const char *name)
{
    for (int i = 0; i < count; ++i) if (!strcmp(fields[i].key, name)) return &fields[i];
    return NULL;
}

static bool integer(const Field *f, int min, int max, int *out)
{
    if (!f || f->kind != 'n') return false;
    char *end;
    long value = strtol(f->value, &end, 10);
    if (!f->value[0] || *end || value < min || value > max) return false;
    *out = (int)value;
    return true;
}

static bool boolean(const Field *f, bool *out)
{
    if (!f || f->kind != 'n') return false;
    if (!strcmp(f->value, "true")) { *out = true; return true; }
    if (!strcmp(f->value, "false")) { *out = false; return true; }
    return false;
}

bool protocol_parse_command(const char *line, Command *command, char *error, unsigned error_len)
{
    Field fields[16] = {0};
    int count, version, id;
    memset(command, 0, sizeof(*command));
    if (!parse_fields(line, fields, &count)) goto bad_json;
    if (!integer(field(fields, count, "v"), 1, 1, &version)) goto bad_version;
    if (!integer(field(fields, count, "id"), 0, 1000000000, &id)) goto bad_id;
    command->id = (uint32_t)id;
    const Field *type = field(fields, count, "type");
    if (!type || type->kind != 's') goto bad_command;

    if (!strcmp(type->value, "set_current")) {
        command->type = CMD_SET_CURRENT;
        if (!integer(field(fields, count, "ma"), -5000, 5000, &command->value)) goto bad_value;
    } else if (!strcmp(type->value, "set_cell")) {
        command->type = CMD_SET_CELL;
        if (!integer(field(fields, count, "index"), 1, BMS_CELLS, &command->index)) goto bad_value;
        const Field *mv = field(fields, count, "mv");
        if (mv && mv->kind == 'n' && !strcmp(mv->value, "null")) command->clear = true;
        else if (!integer(mv, 2500, 4400, &command->value)) goto bad_value;
    } else if (!strcmp(type->value, "set_temp")) {
        command->type = CMD_SET_TEMP;
        if (!integer(field(fields, count, "c"), -40, 100, &command->value)) goto bad_value;
    } else if (!strcmp(type->value, "set_enable")) {
        command->type = CMD_SET_ENABLE;
        if (!boolean(field(fields, count, "charge"), &command->charge) ||
            !boolean(field(fields, count, "discharge"), &command->discharge) ||
            !boolean(field(fields, count, "balance"), &command->balance)) goto bad_value;
    } else if (!strcmp(type->value, "inject_fault")) {
        command->type = CMD_INJECT_FAULT;
        const Field *kind = field(fields, count, "kind");
        if (!kind || kind->kind != 's' || !boolean(field(fields, count, "on"), &command->on)) goto bad_value;
        if (!strcmp(kind->value, "scd")) command->inject = INJECT_SCD;
        else if (!strcmp(kind->value, "nack")) command->inject = INJECT_NACK;
        else if (!strcmp(kind->value, "crc")) command->inject = INJECT_CRC;
        else goto bad_value;
    } else if (!strcmp(type->value, "reset")) command->type = CMD_RESET;
    else if (!strcmp(type->value, "ping")) command->type = CMD_PING;
    else goto bad_command;
    (void)version;
    if (error_len) error[0] = 0;
    return true;

bad_json: snprintf(error, error_len, "invalid_json"); return false;
bad_version: snprintf(error, error_len, "invalid_version"); return false;
bad_id: snprintf(error, error_len, "invalid_id"); return false;
bad_command: snprintf(error, error_len, "unknown_command"); return false;
bad_value: snprintf(error, error_len, "invalid_value"); return false;
}

void protocol_format_ack(char *out, unsigned len, uint32_t id, bool ok, const char *error)
{
    snprintf(out, len, "{\"v\":1,\"type\":\"ack\",\"id\":%u,\"ok\":%s,\"error\":\"%s\"}",
             id, ok ? "true" : "false", error ? error : "");
}

void protocol_format_event(char *out, unsigned len, uint64_t ms, const char *name, uint32_t value)
{
    snprintf(out, len, "{\"v\":1,\"type\":\"event\",\"ms\":%" PRIu64 ",\"name\":\"%s\",\"value\":%u}",
             ms, name, value);
}

void protocol_format_snapshot(char *out, unsigned len, uint32_t seq, const BmsStatus *s)
{
    snprintf(out, len,
        "{\"v\":1,\"type\":\"snapshot\",\"seq\":%u,\"ms\":%" PRIu64
        ",\"cell_mv\":[%d,%d,%d,%d,%d],\"pack_mv\":%d,\"requested_ma\":%d,\"actual_ma\":%d"
        ",\"temp_c\":%d,\"soc_pct\":%.3f,\"mode\":\"%s\",\"faults\":%u,\"balance_mask\":%u"
        ",\"charge_allowed\":%s,\"discharge_allowed\":%s,\"balance_allowed\":%s"
        ",\"charge_on\":%s,\"discharge_on\":%s,\"valid\":%s,\"error\":\"%s\"}",
        seq, s->ms, s->cell_mv[0], s->cell_mv[1], s->cell_mv[2], s->cell_mv[3], s->cell_mv[4],
        s->pack_mv, s->requested_ma, s->actual_ma, s->temp_c, (double)s->soc_pct,
        bms_mode_name(s->mode), s->faults, (unsigned)s->balance_mask,
        s->charge_allowed ? "true" : "false", s->discharge_allowed ? "true" : "false",
        s->balance_allowed ? "true" : "false", s->charge_on ? "true" : "false",
        s->discharge_on ? "true" : "false", s->valid ? "true" : "false", bms_error_name(s->error));
}
