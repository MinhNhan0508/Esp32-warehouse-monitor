#ifndef WAREHOUSE_LOGIC_H
#define WAREHOUSE_LOGIC_H

/* Portable decision logic: no Arduino, GPIO, display or wall clock
 * dependencies. */
#include <stdbool.h>
#include <stdint.h>

enum { WM_TEMP_HIGH = 1u, WM_HUM_HIGH = 2u, WM_DOOR_OPEN = 4u, WM_SENSOR_FAULT = 8u };

typedef struct {
    int16_t temp_on_cc;
    int16_t temp_off_cc;
    uint16_t hum_on_crh;
    uint16_t hum_off_crh;
    uint32_t door_limit_ms;
    uint32_t stale_ms;
    uint32_t mute_ms;
} wm_config_t;

typedef struct {
    uint32_t started_at;
    uint32_t last_good_at;
    uint32_t door_open_at;
    uint32_t muted_at;
    uint32_t attempts;
    uint32_t errors;
    int16_t temperature_cc;
    uint16_t humidity_crh;
    uint8_t good_streak;
    uint8_t alarms;
    uint8_t acknowledged;
    bool has_sample;
    bool sensor_fault;
    bool door_open;
    bool hot;
    bool humid;
    bool muted;
} wm_state_t;

typedef struct {
    uint32_t changed_at;
    bool candidate;
    bool stable;
} wm_debounce_t;

static inline wm_config_t wm_default_config(void) {
    const wm_config_t config = {3200, 3000, 7500, 7000, 10000, 6000, 60000};
    return config;
}

/* Unsigned subtraction is valid across millis() rollover for these short
 * intervals. */
static inline uint32_t wm_elapsed(uint32_t now, uint32_t then) { return now - then; }

static inline void wm_init(wm_state_t *state, uint32_t now, bool door_open) {
    const wm_state_t empty = {0};
    *state = empty;
    state->started_at = now;
    state->door_open_at = now;
    state->door_open = door_open;
}

static inline void wm_debounce_init(wm_debounce_t *input, uint32_t now, bool raw) {
    input->changed_at = now;
    input->candidate = raw;
    input->stable = raw;
}

/* Return true once per debounced transition, on both rising and falling edges.
 */
static inline bool wm_debounce(wm_debounce_t *input, uint32_t now, bool raw, uint32_t settle_ms) {
    if (raw != input->candidate) {
        input->candidate = raw;
        input->changed_at = now;
    }
    if (input->stable != input->candidate && wm_elapsed(now, input->changed_at) >= settle_ms) {
        input->stable = input->candidate;
        return true;
    }
    return false;
}

static inline bool wm_sensor_ready(const wm_state_t *state) {
    return state->has_sample && !state->sensor_fault && state->good_streak >= 2;
}

/* cc = 0.01 degree C; crh = 0.01 percent RH. DHT22 supported range: -40..80 C.
 */
static inline void wm_sample(wm_state_t *state, uint32_t now, bool valid, int16_t temperature_cc,
                             uint16_t humidity_crh) {
    state->attempts++;
    if (!valid || temperature_cc < -4000 || temperature_cc > 8000 || humidity_crh > 10000) {
        state->errors++;
        state->good_streak = 0;
        state->sensor_fault = true;
        return;
    }
    state->has_sample = true;
    state->last_good_at = now;
    state->temperature_cc = temperature_cc;
    state->humidity_crh = humidity_crh;
    if (state->good_streak < 2) {
        state->good_streak++;
    }
    if (state->good_streak >= 2) {
        state->sensor_fault = false;
    }
}

static inline void wm_set_door(wm_state_t *state, uint32_t now, bool open) {
    if (open && !state->door_open) {
        state->door_open_at = now;
    }
    state->door_open = open;
}

static inline uint32_t wm_door_duration(const wm_state_t *state, uint32_t now) {
    return state->door_open ? wm_elapsed(now, state->door_open_at) : 0;
}

static inline void wm_step(wm_state_t *state, const wm_config_t *config, uint32_t now) {
    const uint32_t reference = state->has_sample ? state->last_good_at : state->started_at;
    if (wm_elapsed(now, reference) >= config->stale_ms) {
        state->sensor_fault = true;
        state->good_streak = 0;
    }
    if (wm_sensor_ready(state)) {
        if (state->temperature_cc >= config->temp_on_cc) {
            state->hot = true;
        } else if (state->temperature_cc <= config->temp_off_cc) {
            state->hot = false;
        }
        if (state->humidity_crh >= config->hum_on_crh) {
            state->humid = true;
        } else if (state->humidity_crh <= config->hum_off_crh) {
            state->humid = false;
        }
    } else {
        /* Stored values are historical: do not classify them as fresh readings. */
        state->hot = false;
        state->humid = false;
    }
    state->alarms =
        (state->hot ? WM_TEMP_HIGH : 0) | (state->humid ? WM_HUM_HIGH : 0) |
        (state->door_open && wm_door_duration(state, now) >= config->door_limit_ms ? WM_DOOR_OPEN
                                                                                   : 0) |
        (state->sensor_fault ? WM_SENSOR_FAULT : 0);
    if (state->muted) {
        /* A cause that clears then recurs is a new cause, even during another
         * alarm. */
        state->acknowledged &= state->alarms;
        if (!state->alarms || (state->alarms & (uint8_t)~state->acknowledged) ||
            wm_elapsed(now, state->muted_at) >= config->mute_ms) {
            state->muted = false;
            state->acknowledged = 0;
        }
    }
}

static inline void wm_ack(wm_state_t *state, uint32_t now) {
    if (state->alarms) {
        state->muted = true;
        state->muted_at = now;
        state->acknowledged = state->alarms;
    }
}

static inline const char *wm_status(const wm_state_t *state) {
    return state->alarms ? "ALARM" : (wm_sensor_ready(state) ? "NORMAL" : "STARTUP");
}

#endif
