// Warehouse W06: simulated local station, SD history and authenticated Firebase.
#include "warehouse_cloud.h"
#include "warehouse_logic.h"
#include "warehouse_storage.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <DHTesp.h>
#include <Wire.h>
#include <esp_arduino_version.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>

#ifndef WM_BUILD_ID
#define WM_BUILD_ID 0u
#endif

constexpr uint8_t DHT_PIN = 15;
constexpr uint8_t OLED_SDA = 21;
constexpr uint8_t OLED_SCL = 22;
constexpr uint8_t DOOR_PIN = 26;
constexpr uint8_t ACK_PIN = 27;
constexpr uint8_t GREEN_LED_PIN = 25;
constexpr uint8_t RED_LED_PIN = 33;
constexpr uint8_t BUZZER_PIN = 32;
constexpr uint8_t OLED_ADDRESS = 0x3c;
constexpr uint32_t SAMPLE_MS = 2500;
constexpr uint32_t DISPLAY_MS = 250;
constexpr uint32_t SERIAL_MS = 5000;
constexpr uint32_t CLOUD_SNAPSHOT_MS = 1000;

DHTesp dht;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
wm_config_t config = wm_default_config();
wm_state_t station;
wm_debounce_t door_input;
wm_debounce_t ack_input;
bool oled_ready = false;
bool sounding = false;
bool inject_sensor_error = false;
bool periodic_serial = true;
char command_buffer[320];
uint16_t command_length = 0;
bool discard_command = false;
uint32_t last_sample_at = 0;
uint32_t last_display_at = 0;
uint32_t last_serial_at = 0;
uint32_t last_cloud_at = 0;
cloud::Code last_cloud_code = cloud::CLOUD_DISABLED;
uint8_t previous_alarms = 0;
uint32_t log_session = 0;
uint32_t log_sequence = 0;
uint32_t cloud_sequence = 0;

static void publish_cloud(uint32_t now) {
    const storage::Stats sd = storage::snapshot();
    wm_cloud_frame_t frame = {};
    frame.reading = wm_log_capture(&station, log_session, ++cloud_sequence, now, WM_LOG_SAMPLE,
                                   inject_sensor_error);
    frame.sample_age_ms = station.has_sample ? wm_elapsed(now, station.last_good_at) : UINT32_MAX;
    frame.storage_code = (uint8_t)sd.code;
    frame.storage_samples = sd.samples_written;
    frame.storage_skipped = sd.skipped;
    frame.storage_queue_dropped = sd.queue_dropped;
    cloud::publish(frame);
}

static void record_event(wm_log_kind_t kind, uint32_t now) {
    storage::post_record(
        wm_log_capture(&station, log_session, ++log_sequence, now, kind, inject_sensor_error));
}

static const char *system_status() {
    if (station.alarms)
        return "ALARM";
    const storage::Code code = storage::snapshot().code;
    if (storage::failed(code))
        return "STORAGE_FAULT";
    if (code == storage::STARTING)
        return "STARTUP";
    return wm_status(&station);
}

static void print_causes(uint8_t mask) {
    if (!mask) {
        Serial.print("NONE");
        return;
    }
    if (mask & WM_TEMP_HIGH)
        Serial.print("TEMP_HIGH|");
    if (mask & WM_HUM_HIGH)
        Serial.print("HUM_HIGH|");
    if (mask & WM_DOOR_OPEN)
        Serial.print("DOOR_OPEN|");
    if (mask & WM_SENSOR_FAULT)
        Serial.print("SENSOR_FAULT|");
}

static void sample_sensor() {
    const TempAndHumidity reading = dht.getTempAndHumidity();
    const DHTesp::DHT_ERROR_t result = dht.getStatus();
    const bool valid = !inject_sensor_error && result == DHTesp::ERROR_NONE &&
                       isfinite(reading.temperature) && isfinite(reading.humidity) &&
                       reading.temperature >= -40.0f && reading.temperature <= 80.0f &&
                       reading.humidity >= 0.0f && reading.humidity <= 100.0f;
    // Validate before conversion: NaN/out-of-range floats must not become
    // integers.
    wm_sample(&station, millis(), valid, valid ? (int16_t)lroundf(reading.temperature * 100.0f) : 0,
              valid ? (uint16_t)lroundf(reading.humidity * 100.0f) : 0);
    if (!valid) {
        Serial.printf("EVENT DHT_READ_ERROR reason=%s driver=%s errors=%lu\n",
                      inject_sensor_error ? "INJECTED_TEST" : "DRIVER_OR_RANGE",
                      dht.getStatusString(), (unsigned long)station.errors);
    }
}

static void drive_outputs(uint32_t now) {
    const storage::Code code = storage::snapshot().code;
    digitalWrite(GREEN_LED_PIN,
                 !station.alarms && wm_sensor_ready(&station) && code == storage::READY ? HIGH
                                                                                        : LOW);
    digitalWrite(RED_LED_PIN, station.alarms || storage::failed(code) ? HIGH : LOW);
    const bool desired = station.alarms && !station.muted && now % 1000u < 250u;
    if (desired != sounding) {
        if (desired)
            tone(BUZZER_PIN, 2000);
        else
            noTone(BUZZER_PIN);
        sounding = desired;
    }
}

static void draw_display(uint32_t now) {
    if (!oled_ready)
        return;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.printf("W06 NET %s\n", cloud::status_text(cloud::snapshot().code));
    if (wm_sensor_ready(&station)) {
        display.printf("T %.1fC RH %.1f%%\n", station.temperature_cc / 100.0f,
                       station.humidity_crh / 100.0f);
    } else {
        display.println("T --.-C RH --.-%");
    }
    display.printf("Door %s %lus\n", station.door_open ? "OPEN" : "CLOSED",
                   (unsigned long)(wm_door_duration(&station, now) / 1000u));
    display.printf("State %s\n", system_status());
    display.print("Why ");
    if (!station.alarms)
        display.print("NONE");
    if (station.alarms & WM_TEMP_HIGH)
        display.print("T ");
    if (station.alarms & WM_HUM_HIGH)
        display.print("H ");
    if (station.alarms & WM_DOOR_OPEN)
        display.print("D ");
    if (station.alarms & WM_SENSOR_FAULT)
        display.print("S ");
    display.println();
    display.printf("Sensor %s\n",
                   station.sensor_fault ? "FAULT" : (wm_sensor_ready(&station) ? "FRESH" : "WAIT"));
    display.printf("Sound %s\n", station.muted ? "MUTED" : (station.alarms ? "BEEP" : "OFF"));
    const storage::Stats sd = storage::snapshot();
    if (sd.code == storage::READY) {
        display.printf("SD OK R%lu Q%lu", (unsigned long)sd.samples_written,
                       (unsigned long)sd.queue_dropped);
    } else {
        display.printf("SD %s", storage::status_text(sd.code));
    }
    display.display();
}

static void print_snapshot(uint32_t now) {
    Serial.printf("ms=%lu state=%s ", (unsigned long)now, system_status());
    if (wm_sensor_ready(&station)) {
        Serial.printf("temperature=%.1f C humidity=%.1f %%RH ", station.temperature_cc / 100.0f,
                      station.humidity_crh / 100.0f);
    } else {
        Serial.print("temperature=NA humidity=NA ");
    }
    Serial.printf("door=%s open_s=%lu alarms=", station.door_open ? "OPEN" : "CLOSED",
                  (unsigned long)(wm_door_duration(&station, now) / 1000u));
    print_causes(station.alarms);
    Serial.printf(" muted=%u attempts=%lu errors=%lu oled_boot=%s test_fault=%u",
                  station.muted ? 1u : 0u, (unsigned long)station.attempts,
                  (unsigned long)station.errors, oled_ready ? "READY" : "NOT_FOUND",
                  inject_sensor_error ? 1u : 0u);
    const storage::Stats sd = storage::snapshot();
    Serial.printf(" sd=%s sd_rows=%lu sd_skipped=%lu sd_queue_drop=%lu cloud=%s\n",
                  storage::status_text(sd.code), (unsigned long)sd.samples_written,
                  (unsigned long)sd.skipped, (unsigned long)sd.queue_dropped,
                  cloud::status_text(cloud::snapshot().code));
}

// Small diagnostic console: fixed-size input, bounded work, no Arduino String.
static void read_commands() {
    for (uint8_t budget = 0; budget < 64 && Serial.available(); budget++) {
        const char ch = (char)Serial.read();
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            if (discard_command) {
                Serial.println("EVENT COMMAND_TOO_LONG (maximum 319 characters; input discarded)");
            } else {
                command_buffer[command_length] = '\0';
                if (strcmp(command_buffer, "log off") == 0 ||
                    strcmp(command_buffer, "log on") == 0) {
                    periodic_serial = strcmp(command_buffer, "log on") == 0;
                    Serial.printf("LOG periodic=%s; sensor, SD, cloud and event logging continue\n",
                                  periodic_serial ? "ON (5 seconds)" : "OFF");
                } else if (strcmp(command_buffer, "status") == 0) {
                    print_snapshot(millis());
                } else if (strcmp(command_buffer, "fault on") == 0) {
                    inject_sensor_error = true;
                    Serial.println(
                        "EVENT TEST_FAULT_ON: reject DHT reads; not a measured wire failure");
                    record_event(WM_LOG_SENSOR_TEST_ON, millis());
                } else if (strcmp(command_buffer, "fault off") == 0) {
                    inject_sensor_error = false;
                    Serial.println("EVENT TEST_FAULT_OFF: require two valid reads for recovery");
                    record_event(WM_LOG_SENSOR_TEST_OFF, millis());
                } else if (command_length && !cloud::command(command_buffer) &&
                           !storage::command(command_buffer)) {
                    Serial.println(
                        "Commands: status | log on/off | cloud status/login/off/on/logout | "
                        "fault on/off | sd status/read/events/retry | sd fail on/off");
                }
            }
            command_length = 0;
            discard_command = false;
            cloud::erase(command_buffer, sizeof(command_buffer));
        } else if (!discard_command) {
            if (command_length < sizeof(command_buffer) - 1)
                command_buffer[command_length++] = ch;
            else
                discard_command = true;
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(DOOR_PIN,
            INPUT_PULLUP);          // LOW = contact closed = warehouse door closed.
    pinMode(ACK_PIN, INPUT_PULLUP); // LOW = pressed.
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    dht.setup(DHT_PIN, DHTesp::DHT22);
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setTimeOut(25);
    Wire.beginTransmission(OLED_ADDRESS);
    const bool acknowledged = Wire.endTransmission() == 0;
    if (acknowledged) {
        oled_ready = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, false, false);
    }
    if (oled_ready) {
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setTextWrap(false);
    }
    const uint32_t now = millis();
    wm_init(&station, now, digitalRead(DOOR_PIN) == HIGH);
    wm_debounce_init(&door_input, now, station.door_open);
    wm_debounce_init(&ack_input, now, digitalRead(ACK_PIN) == LOW);
    last_sample_at = now;
    log_session = esp_random(); // Demo boot label, not a timestamp or a globally unique ID.
    storage::begin();
    cloud::begin();
    record_event(WM_LOG_BOOT, now);
    Serial.println("WAREHOUSE W06 | ESP32 | DHT15 | SD CS13 | Firebase | SIMULATION ONLY");
    Serial.printf("BUILD id=%08lx Arduino-ESP32=%s ESP-IDF=%s CPU_MHz=%lu\n",
                  (unsigned long)WM_BUILD_ID, ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion(),
                  (unsigned long)ESP.getCpuFreqMHz());
    Serial.println("W06 revision: idle-priority TLS worker; periodic Serial every 5s");
    Serial.printf("TLS profile=%s | CA and hostname verification enabled\n", WAREHOUSE_TLS_PROFILE);
    Serial.println(
        "Cloud: cloud status | cloud login <device-email> <password> | cloud off/on/logout");
    Serial.println("TEMP on>=32 off<=30 C; RH on>=75 off<=70%; DOOR 10s; ACK mute 60s");
    Serial.println("DNS test: cloud dnsfail on/off (software injection; upload only)");
    Serial.println("Commands: status | log on/off | fault on/off | sd status/read/events/retry | "
                   "sd fail on/off");
    Serial.printf("Log boot label=%08lx; uptime is not UTC; Wokwi web writes are session-local\n",
                  (unsigned long)log_session);
    Serial.printf("OLED 0x3c %s; first two valid samples establish readiness\n",
                  oled_ready ? "READY" : "NOT_FOUND (check wiring; Serial still works)");
    draw_display(now);
}

void loop() {
    storage::pump();
    read_commands();
    uint32_t now = millis();
    bool door_changed = false;
    bool sampled = false;
    if (wm_debounce(&door_input, now, digitalRead(DOOR_PIN) == HIGH, 50)) {
        wm_set_door(&station, now, door_input.stable);
        Serial.printf("EVENT DOOR_%s\n", station.door_open ? "OPEN" : "CLOSED");
        door_changed = true;
    }
    const bool ack_pressed =
        wm_debounce(&ack_input, now, digitalRead(ACK_PIN) == LOW, 35) && ack_input.stable;
    if (wm_elapsed(now, last_sample_at) >= SAMPLE_MS) {
        last_sample_at = now;
        sample_sensor(); // Short synchronous driver transaction, not a 2.5 s sleep.
        sampled = true;
        now = millis();
    }
    wm_step(&station, &config, now);
    if (ack_pressed) {
        wm_ack(&station, now);
        Serial.printf("EVENT ACK muted=%u\n", station.muted ? 1u : 0u);
        record_event(WM_LOG_ACK, now);
    }
    if (door_changed)
        record_event(station.door_open ? WM_LOG_DOOR_OPENED : WM_LOG_DOOR_CLOSED, now);
    if (station.alarms != previous_alarms) {
        Serial.print("EVENT ALARMS ");
        print_causes(station.alarms);
        Serial.println();
        previous_alarms = station.alarms;
        record_event(WM_LOG_ALARMS, now);
    }
    if (sampled)
        record_event(WM_LOG_SAMPLE, now);
    drive_outputs(now);
    if (wm_elapsed(now, last_display_at) >= DISPLAY_MS) {
        last_display_at = now;
        draw_display(now);
    }
    const cloud::Stats network = cloud::snapshot();
    if (network.code != last_cloud_code) {
        last_cloud_code = network.code;
        Serial.printf("EVENT CLOUD state=%s http=%d\n", cloud::status_text(network.code),
                      network.last_http);
    }
    if (wm_elapsed(now, last_cloud_at) >= CLOUD_SNAPSHOT_MS) {
        last_cloud_at = now;
        publish_cloud(now);
    }
    if (wm_elapsed(now, last_serial_at) >= SERIAL_MS) {
        last_serial_at = now;
        if (periodic_serial)
            print_snapshot(now);
    }
    // Cooperative scheduling: no 2500 ms loop delay. DHT/I2C still take finite
    // time.
    delay(5);
}
