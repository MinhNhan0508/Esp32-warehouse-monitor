#include "warehouse_logic.h"
#include <stdio.h>
static void output(const char *text) { fputs(text, stdout); }
static void finish(int code) { (void)code; }


static unsigned failures;
static wm_state_t state;
static wm_config_t config;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            output("FAIL " #value "\n");                                                           \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void ready(uint32_t now, int16_t temp, uint16_t hum) {
    wm_init(&state, now - 5000u, false);
    wm_sample(&state, now - 2500u, true, temp, hum);
    wm_sample(&state, now, true, temp, hum);
    wm_step(&state, &config, now);
}

static void startup_recovery(void) {
    wm_init(&state, 0, false);
    CHECK(!wm_sensor_ready(&state) && !state.alarms);
    wm_sample(&state, 2500, true, 2500, 5000);
    wm_step(&state, &config, 2500);
    CHECK(!wm_sensor_ready(&state) && !state.alarms);
    wm_sample(&state, 5000, true, 2500, 5000);
    wm_step(&state, &config, 5000);
    CHECK(wm_sensor_ready(&state) && !state.alarms);
    wm_sample(&state, 7500, false, 0, 0);
    wm_step(&state, &config, 7500);
    CHECK(!wm_sensor_ready(&state) && state.alarms == WM_SENSOR_FAULT);
    wm_sample(&state, 10000, true, 2500, 5000);
    wm_step(&state, &config, 10000);
    CHECK(state.alarms == WM_SENSOR_FAULT);
    wm_sample(&state, 12500, true, 2500, 5000);
    wm_step(&state, &config, 12500);
    CHECK(wm_sensor_ready(&state) && !state.alarms && state.errors == 1);
}

static void stale_boundary(void) {
    ready(5000, 2500, 5000);
    wm_step(&state, &config, 10999);
    CHECK(!state.alarms);
    wm_step(&state, &config, 11000);
    CHECK(state.alarms == WM_SENSOR_FAULT && !wm_sensor_ready(&state));
    wm_init(&state, 100, false);
    wm_step(&state, &config, 6099);
    CHECK(!state.alarms);
    wm_step(&state, &config, 6100);
    CHECK(state.alarms == WM_SENSOR_FAULT);
}

static void temperature_hysteresis(void) {
    ready(5000, 3199, 5000);
    CHECK(!state.alarms);
    wm_sample(&state, 7500, true, 3200, 5000);
    wm_step(&state, &config, 7500);
    CHECK(state.alarms == WM_TEMP_HIGH);
    wm_sample(&state, 10000, true, 3100, 5000);
    wm_step(&state, &config, 10000);
    CHECK(state.alarms == WM_TEMP_HIGH);
    wm_sample(&state, 12500, true, 3000, 5000);
    wm_step(&state, &config, 12500);
    CHECK(!state.alarms);
}

static void humidity_hysteresis(void) {
    ready(5000, 2500, 7499);
    CHECK(!state.alarms);
    wm_sample(&state, 7500, true, 2500, 7500);
    wm_step(&state, &config, 7500);
    CHECK(state.alarms == WM_HUM_HIGH);
    wm_sample(&state, 10000, true, 2500, 7200);
    wm_step(&state, &config, 10000);
    CHECK(state.alarms == WM_HUM_HIGH);
    wm_sample(&state, 12500, true, 2500, 7000);
    wm_step(&state, &config, 12500);
    CHECK(!state.alarms);
}

static void door_timing(void) {
    wm_init(&state, 0, false);
    wm_set_door(&state, 100, true);
    wm_step(&state, &config, 10099);
    CHECK(!(state.alarms & WM_DOOR_OPEN));
    wm_step(&state, &config, 10100);
    CHECK(state.alarms & WM_DOOR_OPEN);
    wm_set_door(&state, 10101, false);
    wm_step(&state, &config, 10101);
    CHECK(!(state.alarms & WM_DOOR_OPEN) && wm_door_duration(&state, 10101) == 0);
    wm_set_door(&state, 20000, true);
    CHECK(wm_door_duration(&state, 20001) == 1);
}

static void multiple_causes_and_bad_data(void) {
    ready(5000, 3200, 7500);
    wm_set_door(&state, 5000, true);
    wm_sample(&state, 15000, true, 3200, 7500);
    wm_step(&state, &config, 15000);
    CHECK(state.alarms == (WM_TEMP_HIGH | WM_HUM_HIGH | WM_DOOR_OPEN));
    wm_sample(&state, 17500, true, 8001, 5000);
    wm_step(&state, &config, 17500);
    CHECK(state.alarms == (WM_SENSOR_FAULT | WM_DOOR_OPEN));
    CHECK(state.errors == 1 && state.temperature_cc == 3200);
    wm_sample(&state, 20000, true, 2500, 10001);
    CHECK(state.errors == 2);
    wm_sample(&state, 22500, true, -4001, 5000);
    CHECK(state.errors == 3);
    wm_sample(&state, 25000, true, -4000, 0);
    wm_sample(&state, 27500, true, 8000, 10000);
    wm_step(&state, &config, 27500);
    CHECK(wm_sensor_ready(&state));
}

static void mute_new_cause(void) {
    ready(5000, 3200, 5000);
    wm_ack(&state, 5000);
    wm_step(&state, &config, 5001);
    CHECK(state.muted && state.alarms == WM_TEMP_HIGH);
    wm_sample(&state, 7500, true, 3200, 7500);
    wm_step(&state, &config, 7500);
    CHECK(!state.muted && state.alarms == (WM_TEMP_HIGH | WM_HUM_HIGH));
    wm_ack(&state, 7500);
    wm_sample(&state, 10000, true, 3200, 7000);
    wm_step(&state, &config, 10000);
    CHECK(state.muted && state.acknowledged == WM_TEMP_HIGH);
    wm_sample(&state, 12500, true, 3200, 7500);
    wm_step(&state, &config, 12500);
    CHECK(!state.muted);
}

static void mute_expiry_and_clear(void) {
    ready(5000, 3200, 5000);
    wm_ack(&state, 5000);
    wm_sample(&state, 64999, true, 3200, 5000);
    wm_step(&state, &config, 64999);
    CHECK(state.muted);
    wm_step(&state, &config, 65000);
    CHECK(!state.muted && state.alarms == WM_TEMP_HIGH);
    wm_ack(&state, 65000);
    wm_sample(&state, 67500, true, 3000, 5000);
    wm_step(&state, &config, 67500);
    CHECK(!state.alarms && !state.muted);
    wm_ack(&state, 67501);
    CHECK(!state.muted);
}

static void input_bounce(void) {
    wm_debounce_t input;
    wm_debounce_init(&input, 0, false);
    CHECK(!wm_debounce(&input, 1, true, 50));
    CHECK(!wm_debounce(&input, 10, false, 50));
    CHECK(!wm_debounce(&input, 20, true, 50));
    CHECK(!wm_debounce(&input, 69, true, 50));
    CHECK(wm_debounce(&input, 70, true, 50) && input.stable);
    CHECK(!wm_debounce(&input, 1000, true, 50));
    CHECK(!wm_debounce(&input, 1010, false, 50));
    CHECK(wm_debounce(&input, 1060, false, 50) && !input.stable);
}

static void clock_rollover(void) {
    const uint32_t before = UINT32_MAX - 3000u;
    ready(before, 3200, 5000);
    wm_ack(&state, before);
    wm_set_door(&state, before, true);
    wm_sample(&state, before + 5999u, true, 3200, 5000);
    wm_step(&state, &config, before + 5999u);
    CHECK(state.muted && !state.sensor_fault);
    CHECK(wm_door_duration(&state, before + 10000u) == 10000u);
    wm_sample(&state, before + 10000u, true, 3200, 5000);
    wm_step(&state, &config, before + 10000u);
    CHECK((state.alarms & WM_DOOR_OPEN) && !state.muted);
    wm_ack(&state, before + 10000u);
    wm_sample(&state, before + 70000u, true, 3200, 5000);
    wm_step(&state, &config, before + 70000u);
    CHECK(!state.muted);
    wm_step(&state, &config, before + 76000u);
    CHECK(state.sensor_fault);
    wm_debounce_t input;
    wm_debounce_init(&input, UINT32_MAX - 20u, false);
    CHECK(!wm_debounce(&input, UINT32_MAX - 10u, true, 50));
    CHECK(wm_debounce(&input, 39u, true, 50));
}

#define RUN(name)                                                                                  \
    do {                                                                                           \
        unsigned prior = failures;                                                                 \
        name();                                                                                    \
        output(prior == failures ? "PASS " #name "\n" : "CASE_FAILED " #name "\n");                \
    } while (0)
int main(void) {
    config = wm_default_config();
    RUN(startup_recovery);
    RUN(stale_boundary);
    RUN(temperature_hysteresis);
    RUN(humidity_hysteresis);
    RUN(door_timing);
    RUN(multiple_causes_and_bad_data);
    RUN(mute_new_cause);
    RUN(mute_expiry_and_clear);
    RUN(input_bounce);
    RUN(clock_rollover);
    output(failures ? "WAREHOUSE_LOGIC_FAILED\n" : "WAREHOUSE_LOGIC_PASS\n");
    finish(failures ? 1 : 0);
    return failures ? 1 : 0;
}
