#ifndef WAREHOUSE_LOG_H
#define WAREHOUSE_LOG_H

/* CSV schema and bounds shared by the firmware and portable C tests. */
#include "warehouse_logic.h"
#include <stddef.h>
#include <stdio.h>

#define WM_LOG_LINE_CAPACITY 256u
#define WM_LOG_FILE_LIMIT (1024u * 1024u)
#define WM_LOG_HEADER                                                                              \
    "session_id,sequence,uptime_ms,kind,temperature_c,humidity_rh_pct,sensor_valid,door_open,"     \
    "door_open_ms,alarm_mask,muted,read_attempts,read_errors,test_sensor_fault\n"

typedef enum {
    WM_LOG_SAMPLE = 0,
    WM_LOG_BOOT,
    WM_LOG_DOOR_OPENED,
    WM_LOG_DOOR_CLOSED,
    WM_LOG_ALARMS,
    WM_LOG_ACK,
    WM_LOG_SENSOR_TEST_ON,
    WM_LOG_SENSOR_TEST_OFF,
    WM_LOG_KIND_COUNT
} wm_log_kind_t;

typedef struct {
    uint32_t session_id;
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t door_open_ms;
    uint32_t read_attempts;
    uint32_t read_errors;
    int16_t temperature_cc;
    uint16_t humidity_crh;
    wm_log_kind_t kind;
    uint8_t alarm_mask;
    bool sensor_valid;
    bool door_open;
    bool muted;
    bool test_sensor_fault;
} wm_log_record_t;

static inline const char *wm_log_kind_name(wm_log_kind_t kind) {
    switch (kind) {
    case WM_LOG_SAMPLE:
        return "SAMPLE";
    case WM_LOG_BOOT:
        return "BOOT";
    case WM_LOG_DOOR_OPENED:
        return "DOOR_OPEN";
    case WM_LOG_DOOR_CLOSED:
        return "DOOR_CLOSED";
    case WM_LOG_ALARMS:
        return "ALARMS_CHANGED";
    case WM_LOG_ACK:
        return "ACK";
    case WM_LOG_SENSOR_TEST_ON:
        return "SENSOR_TEST_ON";
    case WM_LOG_SENSOR_TEST_OFF:
        return "SENSOR_TEST_OFF";
    default:
        return "INVALID";
    }
}

static inline wm_log_record_t wm_log_capture(const wm_state_t *state, uint32_t session,
                                             uint32_t sequence, uint32_t now, wm_log_kind_t kind,
                                             bool injected) {
    wm_log_record_t record = {0};
    record.session_id = session;
    record.sequence = sequence;
    record.uptime_ms = now;
    record.door_open_ms = wm_door_duration(state, now);
    record.read_attempts = state->attempts;
    record.read_errors = state->errors;
    record.temperature_cc = state->temperature_cc;
    record.humidity_crh = state->humidity_crh;
    record.kind = kind;
    record.alarm_mask = state->alarms;
    record.sensor_valid = wm_sensor_ready(state);
    record.door_open = state->door_open;
    record.muted = state->muted;
    record.test_sensor_fault = injected;
    return record;
}

/* Subtraction avoids integer overflow and includes the CSV header in file size. */
static inline bool wm_log_fits(uint64_t size, size_t length, uint64_t limit) {
    return length <= limit && size <= limit - length;
}

static inline bool wm_log_write_complete(size_t wanted, size_t written, uint64_t before,
                                         uint64_t after, bool error) {
    return wanted > 0 && written == wanted && !error && before <= UINT64_MAX - wanted &&
           after == before + wanted;
}

/* Return zero for invalid/truncated output. Unavailable sensors use empty CSV fields. */
static inline size_t wm_log_format(char *buffer, size_t capacity, const wm_log_record_t *r) {
    if (!buffer || !capacity || !r)
        return 0;
    buffer[0] = '\0';
    if ((unsigned)r->kind >= WM_LOG_KIND_COUNT)
        return 0;
    char temperature[16] = "";
    char humidity[16] = "";
    if (r->sensor_valid) {
        if (r->temperature_cc < -4000 || r->temperature_cc > 8000 || r->humidity_crh > 10000)
            return 0;
        const int32_t absolute =
            r->temperature_cc < 0 ? -(int32_t)r->temperature_cc : r->temperature_cc;
        snprintf(temperature, sizeof(temperature), "%s%ld.%02ld", r->temperature_cc < 0 ? "-" : "",
                 (long)(absolute / 100), (long)(absolute % 100));
        snprintf(humidity, sizeof(humidity), "%u.%02u", (unsigned)(r->humidity_crh / 100u),
                 (unsigned)(r->humidity_crh % 100u));
    }
    const int length =
        snprintf(buffer, capacity, "%08lx,%lu,%lu,%s,%s,%s,%u,%u,%lu,%u,%u,%lu,%lu,%u\n",
                 (unsigned long)r->session_id, (unsigned long)r->sequence,
                 (unsigned long)r->uptime_ms, wm_log_kind_name(r->kind), temperature, humidity,
                 r->sensor_valid ? 1u : 0u, r->door_open ? 1u : 0u, (unsigned long)r->door_open_ms,
                 (unsigned)r->alarm_mask, r->muted ? 1u : 0u, (unsigned long)r->read_attempts,
                 (unsigned long)r->read_errors, r->test_sensor_fault ? 1u : 0u);
    if (length < 0 || (size_t)length >= capacity) {
        buffer[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

#endif
