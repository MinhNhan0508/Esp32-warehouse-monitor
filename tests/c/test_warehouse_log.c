#include "warehouse_log.h"
#include <string.h>
static void output(const char *text) { fputs(text, stdout); }
static void finish(int code) { (void)code; }


static unsigned failures;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            output("FAIL " #value "\n");                                                           \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
static char line[WM_LOG_LINE_CAPACITY];

static wm_log_record_t example(void) {
    wm_state_t state;
    wm_init(&state, 0, false);
    wm_sample(&state, 2500, true, 2560, 6450);
    wm_sample(&state, 5000, true, 2560, 6450);
    return wm_log_capture(&state, 0x1234, 7, 9000, WM_LOG_SAMPLE, false);
}

static void known_sample_vector(void) {
    wm_log_record_t record = example();
    const char *expected = "00001234,7,9000,SAMPLE,25.60,64.50,1,0,0,0,0,2,0,0\n";
    CHECK(wm_log_format(line, sizeof(line), &record) == strlen(expected));
    CHECK(strcmp(line, expected) == 0);
    unsigned commas = 0;
    for (const char *p = WM_LOG_HEADER; *p; p++)
        commas += *p == ',';
    CHECK(commas == 13);
}

static void invalid_sensor_fields(void) {
    wm_log_record_t record = example();
    record.sensor_valid = false;
    record.alarm_mask = WM_SENSOR_FAULT;
    record.read_errors = 1;
    record.test_sensor_fault = true;
    CHECK(wm_log_format(line, sizeof(line), &record) > 0);
    CHECK(strstr(line, ",SAMPLE,,,0,0,0,8,0,2,1,1\n") != NULL);
    CHECK(strstr(line, "25.60") == NULL && strstr(line, "64.50") == NULL);
}

static void signed_and_extreme_values(void) {
    wm_log_record_t record = example();
    record.temperature_cc = -1;
    CHECK(wm_log_format(line, sizeof(line), &record) > 0);
    CHECK(strstr(line, ",-0.01,64.50,") != NULL);
    record.temperature_cc = -4000;
    record.humidity_crh = 0;
    CHECK(wm_log_format(line, sizeof(line), &record) > 0);
    CHECK(strstr(line, ",-40.00,0.00,") != NULL);
    record.temperature_cc = 8000;
    record.humidity_crh = 10000;
    record.session_id = UINT32_MAX;
    record.sequence = UINT32_MAX;
    record.uptime_ms = UINT32_MAX;
    record.door_open_ms = UINT32_MAX;
    record.read_attempts = UINT32_MAX;
    record.read_errors = UINT32_MAX;
    CHECK(wm_log_format(line, sizeof(line), &record) > 0);
    CHECK(strstr(line, ",80.00,100.00,") != NULL);
    CHECK(strlen(line) < WM_LOG_LINE_CAPACITY);
    record.temperature_cc = 8001;
    CHECK(!wm_log_format(line, sizeof(line), &record) && !line[0]);
    record.temperature_cc = 2000;
    record.humidity_crh = 10001;
    CHECK(!wm_log_format(line, sizeof(line), &record));
}

static void bounded_format(void) {
    wm_log_record_t record = example();
    char small[12];
    memset(small, 'X', sizeof(small));
    CHECK(wm_log_format(small, 10, &record) == 0 && small[0] == '\0');
    CHECK(small[10] == 'X' && small[11] == 'X');
    CHECK(wm_log_format(NULL, 10, &record) == 0);
    CHECK(wm_log_format(small, 0, &record) == 0);
    CHECK(wm_log_format(small, sizeof(small), NULL) == 0);
    record.kind = (wm_log_kind_t)-1;
    CHECK(wm_log_format(line, sizeof(line), &record) == 0);
    record.kind = WM_LOG_KIND_COUNT;
    CHECK(wm_log_format(line, sizeof(line), &record) == 0);
}

static void file_limit_boundaries(void) {
    CHECK(wm_log_fits(WM_LOG_FILE_LIMIT - 100, 100, WM_LOG_FILE_LIMIT));
    CHECK(!wm_log_fits(WM_LOG_FILE_LIMIT - 99, 100, WM_LOG_FILE_LIMIT));
    CHECK(!wm_log_fits(WM_LOG_FILE_LIMIT + 1, 1, WM_LOG_FILE_LIMIT));
    CHECK(!wm_log_fits(0, WM_LOG_FILE_LIMIT + 1, WM_LOG_FILE_LIMIT));
    CHECK(!wm_log_fits(UINT64_MAX, 1, UINT64_MAX));
    CHECK(wm_log_fits(UINT64_MAX - 1, 1, UINT64_MAX));
}

static void short_write_detection(void) {
    CHECK(wm_log_write_complete(80, 80, 100, 180, false));
    CHECK(!wm_log_write_complete(80, 79, 100, 179, false));
    CHECK(!wm_log_write_complete(80, 80, 100, 180, true));
    CHECK(!wm_log_write_complete(80, 80, 100, 100, false));
    CHECK(!wm_log_write_complete(80, 80, 100, 181, false));
    CHECK(!wm_log_write_complete(0, 0, 100, 100, false));
    CHECK(!wm_log_write_complete(80, 80, UINT64_MAX - 20, 59, false));
}

static void snapshot_and_rollover(void) {
    wm_state_t state;
    wm_init(&state, UINT32_MAX - 20, true);
    wm_sample(&state, UINT32_MAX - 10, true, 2500, 5000);
    wm_sample(&state, 5, true, 2500, 5000);
    state.muted = true;
    const wm_log_record_t record = wm_log_capture(&state, 42, 3, 29, WM_LOG_ACK, false);
    CHECK(record.door_open_ms == 50 && record.sensor_valid && record.muted);
    state.temperature_cc = 3000;
    CHECK(record.temperature_cc == 2500); // A queued value copy, not a pointer to mutable state.
}

static void event_schema(void) {
    const char *names[] = {"SAMPLE",         "BOOT", "DOOR_OPEN",      "DOOR_CLOSED",
                           "ALARMS_CHANGED", "ACK",  "SENSOR_TEST_ON", "SENSOR_TEST_OFF"};
    wm_log_record_t record = example();
    for (unsigned i = 0; i < WM_LOG_KIND_COUNT; i++) {
        CHECK(strcmp(wm_log_kind_name((wm_log_kind_t)i), names[i]) == 0);
        record.kind = (wm_log_kind_t)i;
        CHECK(wm_log_format(line, sizeof(line), &record) > 0);
        unsigned commas = 0;
        for (const char *p = line; *p; p++)
            commas += *p == ',';
        CHECK(commas == 13 && line[strlen(line) - 1] == '\n');
    }
}

#define RUN(name)                                                                                  \
    do {                                                                                           \
        unsigned prior = failures;                                                                 \
        name();                                                                                    \
        output(prior == failures ? "PASS " #name "\n" : "CASE_FAILED " #name "\n");                \
    } while (0)
int main(void) {
    RUN(known_sample_vector);
    RUN(invalid_sensor_fields);
    RUN(signed_and_extreme_values);
    RUN(bounded_format);
    RUN(file_limit_boundaries);
    RUN(short_write_detection);
    RUN(snapshot_and_rollover);
    RUN(event_schema);
    output(failures ? "WAREHOUSE_LOG_FORMAT_FAILED\n" : "WAREHOUSE_LOG_FORMAT_PASS\n");
    finish(failures ? 1 : 0);
    return failures ? 1 : 0;
}
