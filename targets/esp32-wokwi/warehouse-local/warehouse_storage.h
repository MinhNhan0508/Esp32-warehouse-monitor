#ifndef WAREHOUSE_STORAGE_H
#define WAREHOUSE_STORAGE_H

/* ESP32 adapter: a single task owns SPI/SD. Only value copies cross task boundaries. */
#include "warehouse_log.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

namespace storage {
constexpr uint8_t CS_PIN = 13;
constexpr uint8_t SCK_PIN = 18;
constexpr uint8_t MISO_PIN = 19;
constexpr uint8_t MOSI_PIN = 23;
constexpr UBaseType_t QUEUE_DEPTH = 8;
constexpr uint32_t TASK_STACK_BYTES = 8192; // ESP-IDF task stack sizes use bytes.
constexpr const char *SAMPLE_PATH = "/samples.csv";
constexpr const char *EVENT_PATH = "/events.csv";

enum Code {
    STARTING,
    READY,
    MOUNT_FAILED,
    IO_FAILED,
    HEADER_FAILED,
    FULL,
    TEST_FAULT,
    TASK_FAILED
};
enum Operation { RECORD, RETRY, READ_SAMPLES, READ_EVENTS, FAIL_ON, FAIL_OFF };
struct Stats {
    Code code;
    uint32_t samples_written;
    uint32_t events_written;
    uint32_t errors;
    uint32_t skipped;
    uint32_t queue_dropped;
    uint32_t reply_dropped;
};
struct Message {
    Operation operation;
    wm_log_record_t record;
};
struct Reply {
    char text[WM_LOG_LINE_CAPACITY + 24];
};

static Stats stats = {};
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticQueue_t input_control;
static StaticQueue_t output_control;
static uint8_t input_memory[QUEUE_DEPTH * sizeof(Message)];
static uint8_t output_memory[QUEUE_DEPTH * sizeof(Reply)];
static QueueHandle_t input_queue = nullptr;
static QueueHandle_t output_queue = nullptr;
static TaskHandle_t task_handle = nullptr;
static bool mounted = false; // Worker-owned fields, never read by the main loop.
static bool injected = false;
static char tail_lines[10][WM_LOG_LINE_CAPACITY];

static inline const char *status_text(Code code) {
    switch (code) {
    case STARTING:
        return "STARTING";
    case READY:
        return "READY";
    case MOUNT_FAILED:
        return "MOUNT_FAILED";
    case IO_FAILED:
        return "IO_FAILED";
    case HEADER_FAILED:
        return "HEADER_FAILED";
    case FULL:
        return "FILE_LIMIT";
    case TEST_FAULT:
        return "TEST_FAULT";
    default:
        return "TASK_FAILED";
    }
}

static Stats snapshot() {
    portENTER_CRITICAL(&stats_lock);
    const Stats result = stats;
    portEXIT_CRITICAL(&stats_lock);
    return result;
}

static bool failed(Code code) { return code != STARTING && code != READY; }

static void change_code(Code code, bool count_error = false) {
    portENTER_CRITICAL(&stats_lock);
    stats.code = code;
    if (count_error)
        stats.errors++;
    portEXIT_CRITICAL(&stats_lock);
}

static void reply(const char *text) {
    Reply message = {};
    snprintf(message.text, sizeof(message.text), "%s", text);
    // Only this worker may block on a diagnostic reply; the input/control loop never does.
    if (xQueueSend(output_queue, &message, pdMS_TO_TICKS(100)) != pdTRUE) {
        portENTER_CRITICAL(&stats_lock);
        stats.reply_dropped++;
        portEXIT_CRITICAL(&stats_lock);
    }
}

static void fail(Code code, const char *reason) {
    change_code(code, code != FULL && code != TEST_FAULT);
    reply(reason);
}

static bool prepare_file(const char *path) {
    const size_t header_length = sizeof(WM_LOG_HEADER) - 1;
    if (SD.exists(path)) {
        File existing = SD.open(path, FILE_READ);
        if (!existing || existing.isDirectory()) {
            fail(IO_FAILED, "SD_ERROR cannot inspect log file");
            return false;
        }
        const size_t size = existing.size();
        if (size > WM_LOG_FILE_LIMIT) {
            existing.close();
            fail(FULL, "SD_ERROR log exceeds 1 MiB limit; no deletion/overwrite attempted");
            return false;
        }
        if (size) {
            char header[sizeof(WM_LOG_HEADER)] = {};
            const size_t read = existing.readBytes(header, header_length);
            const bool header_ok =
                read == header_length && memcmp(header, WM_LOG_HEADER, header_length) == 0;
            const bool tail_ok = existing.seek(size - 1) && existing.read() == '\n';
            existing.close();
            if (!header_ok || !tail_ok) {
                fail(HEADER_FAILED, "SD_ERROR schema mismatch or partial last row; not appending");
                return false;
            }
            return true;
        }
        existing.close();
    }
    File file = SD.open(path, FILE_APPEND); // Creation must never truncate an existing file.
    if (!file) {
        fail(IO_FAILED, "SD_ERROR cannot create log file");
        return false;
    }
    if (file.size() != 0) {
        file.close();
        fail(HEADER_FAILED, "SD_ERROR file changed during header preparation; not modifying");
        return false;
    }
    const size_t written = file.write((const uint8_t *)WM_LOG_HEADER, header_length);
    file.flush();
    const bool complete =
        wm_log_write_complete(header_length, written, 0, file.size(), file.getWriteError() != 0);
    file.close();
    if (!complete)
        fail(IO_FAILED, "SD_ERROR short header write");
    return complete;
}

static void mount_card() {
    if (injected) {
        fail(TEST_FAULT, "SD_TEST_FAULT active; use sd fail off before retry");
        return;
    }
    change_code(STARTING);
    SD.end();
    mounted = SD.begin(CS_PIN, SPI, 4000000, "/sd", 3, false);
    if (!mounted || SD.cardType() == CARD_NONE) {
        mounted = false;
        fail(MOUNT_FAILED, "SD_ERROR mount failed; local sensing/alarms continue");
        return;
    }
    if (!prepare_file(SAMPLE_PATH) || !prepare_file(EVENT_PATH))
        return;
    change_code(READY);
    reply("SD_READY samples.csv and events.csv; 1 MiB limit each; no automatic formatting");
}

static void write_record(const wm_log_record_t &record) {
    if (snapshot().code != READY || injected) {
        portENTER_CRITICAL(&stats_lock);
        stats.skipped++;
        portEXIT_CRITICAL(&stats_lock);
        return;
    }
    char line[WM_LOG_LINE_CAPACITY];
    const size_t length = wm_log_format(line, sizeof(line), &record);
    if (!length) {
        fail(IO_FAILED, "SD_ERROR invalid or truncated CSV record");
        portENTER_CRITICAL(&stats_lock);
        stats.skipped++;
        portEXIT_CRITICAL(&stats_lock);
        return;
    }
    File file = SD.open(record.kind == WM_LOG_SAMPLE ? SAMPLE_PATH : EVENT_PATH, FILE_APPEND);
    if (!file) {
        fail(IO_FAILED, "SD_ERROR open for append failed");
    } else {
        const size_t before = file.size();
        if (!wm_log_fits(before, length, WM_LOG_FILE_LIMIT)) {
            file.close();
            fail(FULL,
                 "SD_ERROR file reached 1 MiB limit; new records skipped, existing data retained");
        } else {
            const size_t written = file.write((const uint8_t *)line, length);
            file.flush();
            const bool complete = wm_log_write_complete(length, written, before, file.size(),
                                                        file.getWriteError() != 0);
            file.close();
            if (complete) {
                portENTER_CRITICAL(&stats_lock);
                if (record.kind == WM_LOG_SAMPLE)
                    stats.samples_written++;
                else
                    stats.events_written++;
                portEXIT_CRITICAL(&stats_lock);
                return;
            }
            fail(IO_FAILED,
                 "SD_ERROR short/failed append; partial data may remain; inspect before retry");
        }
    }
    portENTER_CRITICAL(&stats_lock);
    stats.skipped++;
    portEXIT_CRITICAL(&stats_lock);
}

static void read_tail(const char *path) {
    if (!mounted) {
        reply("SD_ERROR cannot read: card not mounted");
        return;
    }
    File file = SD.open(path, FILE_READ);
    if (!file) {
        reply("SD_ERROR cannot open requested log");
        return;
    }
    const size_t size = file.size();
    const uint32_t start = size > 4096 ? size - 4096 : 0;
    if (!file.seek(start)) {
        file.close();
        reply("SD_ERROR seek failed");
        return;
    }
    char line[WM_LOG_LINE_CAPACITY] = {};
    size_t length = 0;
    uint32_t count = 0;
    bool discard = start != 0; // Discard an incomplete line at the beginning of the window.
    for (size_t budget = 0; budget < 4096 && file.available(); budget++) {
        const int value = file.read();
        if (value < 0)
            break;
        const char ch = (char)value;
        if (ch == '\n') {
            if (!discard && length) {
                line[length] = '\0';
                snprintf(tail_lines[count % 10], sizeof(tail_lines[0]), "%s", line);
                count++;
            }
            discard = false;
            length = 0;
        } else if (ch != '\r' && !discard) {
            if (length < sizeof(line) - 1)
                line[length++] = ch;
            else
                discard = true;
        }
    }
    file.close();
    Reply heading = {};
    snprintf(heading.text, sizeof(heading.text),
             "SD_READ_BEGIN %s bytes=%lu (last 10 complete lines in 4096-byte window)", path,
             (unsigned long)size);
    reply(heading.text);
    const uint32_t first = count > 10 ? count - 10 : 0;
    for (uint32_t index = first; index < count; index++) {
        Reply output = {};
        snprintf(output.text, sizeof(output.text), "SD_DATA %s", tail_lines[index % 10]);
        reply(output.text);
    }
    reply("SD_READ_END");
}

static void worker(void *) {
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
    mount_card();
    Message message;
    for (;;) {
        if (xQueueReceive(input_queue, &message, portMAX_DELAY) != pdTRUE)
            continue;
        switch (message.operation) {
        case RECORD:
            write_record(message.record);
            break;
        case RETRY:
            mount_card();
            break;
        case READ_SAMPLES:
            read_tail(SAMPLE_PATH);
            break;
        case READ_EVENTS:
            read_tail(EVENT_PATH);
            break;
        case FAIL_ON:
            injected = true;
            fail(TEST_FAULT,
                 "SD_TEST_FAULT_ON: deliberate storage failure, not measured card damage");
            break;
        case FAIL_OFF:
            injected = false;
            reply("SD_TEST_FAULT_OFF: reopen logs; skipped records are not replayed");
            mount_card();
            break;
        }
    }
}

static void begin() {
    input_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(Message), input_memory, &input_control);
    output_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(Reply), output_memory, &output_control);
    if (!input_queue || !output_queue ||
        xTaskCreatePinnedToCore(worker, "sd_logger", TASK_STACK_BYTES, nullptr, 1, &task_handle,
                                0) != pdPASS) {
        change_code(TASK_FAILED, true);
    }
}

static bool post(const Message &message) {
    if (!task_handle || xQueueSend(input_queue, &message, 0) != pdTRUE) {
        portENTER_CRITICAL(&stats_lock);
        stats.queue_dropped++;
        portEXIT_CRITICAL(&stats_lock);
        return false;
    }
    return true;
}

static bool post_record(const wm_log_record_t &record) {
    const Message message = {RECORD, record};
    return post(message);
}

static void print_status() {
    const Stats s = snapshot();
    Serial.printf("SD_STATUS state=%s samples=%lu events=%lu errors=%lu skipped=%lu "
                  "queue_dropped=%lu reply_dropped=%lu\n",
                  status_text(s.code), (unsigned long)s.samples_written,
                  (unsigned long)s.events_written, (unsigned long)s.errors,
                  (unsigned long)s.skipped, (unsigned long)s.queue_dropped,
                  (unsigned long)s.reply_dropped);
}

static bool command(const char *text) {
    if (strcmp(text, "sd status") == 0) {
        print_status();
        return true;
    }
    Message message = {};
    if (strcmp(text, "sd read") == 0)
        message.operation = READ_SAMPLES;
    else if (strcmp(text, "sd events") == 0)
        message.operation = READ_EVENTS;
    else if (strcmp(text, "sd retry") == 0)
        message.operation = RETRY;
    else if (strcmp(text, "sd fail on") == 0)
        message.operation = FAIL_ON;
    else if (strcmp(text, "sd fail off") == 0)
        message.operation = FAIL_OFF;
    else
        return false;
    Serial.println(post(message) ? "SD_COMMAND_QUEUED (await worker result)"
                                 : "SD_BUSY command not accepted; no automatic retry");
    return true;
}

/* Single Serial owner: worker replies are printed only by Arduino loop(). */
static void pump() {
    Reply message;
    if (output_queue && xQueueReceive(output_queue, &message, 0) == pdTRUE)
        Serial.println(message.text);
}
} // namespace storage
#endif
