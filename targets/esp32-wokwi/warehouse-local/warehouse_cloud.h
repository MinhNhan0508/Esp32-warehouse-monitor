#ifndef WAREHOUSE_CLOUD_H
#define WAREHOUSE_CLOUD_H
#include "warehouse_ca.h"
#include "warehouse_cloud_config.h"
#include "warehouse_cloud_model.h"
#include "warehouse_tls.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

namespace cloud {
// TLS work can stay CPU-ready for seconds in simulation. Share priority 0
// with IDLE0 so tick time-slicing can service the idle watchdog during crypto.
// Keep both the watchdog and TLS certificate verification enabled.
constexpr UBaseType_t WORKER_PRIORITY = tskIDLE_PRIORITY;
enum Code {
    CLOUD_DISABLED,
    CONFIG_REQUIRED,
    PAUSED,
    WIFI_CONNECTING,
    TIME_SYNC,
    NEED_LOGIN,
    AUTHENTICATING,
    ONLINE,
    BACKOFF,
    AUTH_FAILED,
    PERMISSION_DENIED,
    NETWORK_ERROR
};
struct Stats {
    Code code;
    uint32_t published;
    uint32_t failures;
    uint32_t coalesced;
    uint32_t last_success_ms;
    int last_http;
    uint32_t wifi_recoveries;
    uint32_t wifi_begin_calls;
    uint32_t wifi_disconnects;
    uint32_t wifi_got_ip;
    int wifi_begin_result;
    uint8_t wifi_last_reason;
    bool wifi_mode_ready;
};
struct Credentials {
    char email[96];
    char password[160];
};
struct Transport {
    const char *phase;
    const char *kind;
    char host[128];
    char tls_message[128];
    char server_date[48];
    const char *error_reason;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    uint32_t epoch_s;
    uint32_t heap_before;
    uint32_t heap_after;
    bool active;
    bool dns_ok;
    bool tcp_tested;
    bool tcp_ok;
    int http_code;
    int tls_code;
    bool clock_synced;
    int64_t clock_adjust_ms;
    uint32_t tls_ready_ms;
    int clock_http;
    bool connection_reused;
};
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Stats stats = {};
static Transport transport = {};
struct UploadEvidence {
    char payload[1024]; // Public telemetry only, never URL, credentials or tokens.
    char server_date[48];
    const char *error_reason;
    uint32_t token_age_ms;
    uint32_t token_ttl_ms;
    int http;
    uint8_t slot;
    bool available;
    bool truncated;
    bool clock_synced;
    int64_t clock_adjust_ms;
    bool snapshot_after_tls;
    int clock_http;
    bool connection_reused;
};
static UploadEvidence upload_evidence = {};
static wm_cloud_frame_t latest = {};
static bool has_frame = false;
static bool requested = true;
static bool dns_test_fault = false; // Serial-only diagnostic; never enabled at boot.
static bool pending_login = false;
static bool pending_logout = false;
static bool pending_probe = false;
static bool probe_active = false;
static Credentials credentials = {};
static TaskHandle_t task_handle = nullptr;

static uint64_t utc_now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}
// Only call with Date from our certificate/hostname-verified HTTPS requests.
// Date has one-second resolution; it is not a precise network clock measurement.
static bool sync_http_date(const String &date, int64_t &adjust_ms) {
    uint64_t epoch_ms;
    if (!wm_cloud_http_date_ms(date.c_str(), &epoch_ms))
        return false;
    const int64_t delta = (int64_t)epoch_ms - (int64_t)utc_now_ms();
    struct timeval tv = {};
    tv.tv_sec = (time_t)(epoch_ms / 1000u);
    if (settimeofday(&tv, nullptr) != 0)
        return false;
    adjust_ms = delta;
    return true;
}

// Arduino invokes events on a different task. Copy counters under the same lock;
// do not print, reconnect, or perform TLS inside the callback.
static void wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    portENTER_CRITICAL(&lock);
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        stats.wifi_disconnects++;
        stats.wifi_last_reason = info.wifi_sta_disconnected.reason;
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        stats.wifi_got_ip++;
    }
    portEXIT_CRITICAL(&lock);
}

static void erase(void *data, size_t size) {
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    while (size--)
        *bytes++ = 0;
}
static const char *status_text(Code code) {
    switch (code) {
    case CLOUD_DISABLED:
        return "DISABLED";
    case CONFIG_REQUIRED:
        return "CONFIG_REQUIRED";
    case PAUSED:
        return "PAUSED";
    case WIFI_CONNECTING:
        return "WIFI_CONNECTING";
    case TIME_SYNC:
        return "TIME_SYNC";
    case NEED_LOGIN:
        return "NEED_LOGIN";
    case AUTHENTICATING:
        return "AUTHENTICATING";
    case ONLINE:
        return "ONLINE";
    case AUTH_FAILED:
        return "AUTH_FAILED";
    case PERMISSION_DENIED:
        return "PERMISSION_DENIED";
    case NETWORK_ERROR:
        return "NETWORK_ERROR";
    default:
        return "BACKOFF";
    }
}
static Stats snapshot() {
    portENTER_CRITICAL(&lock);
    const Stats copy = stats;
    portEXIT_CRITICAL(&lock);
    return copy;
}
static void state(Code code, int http = 0) {
    portENTER_CRITICAL(&lock);
    stats.code = code;
    stats.last_http = http;
    portEXIT_CRITICAL(&lock);
}
static void publish(const wm_cloud_frame_t &frame) {
    portENTER_CRITICAL(&lock);
    if (has_frame)
        stats.coalesced++;
    latest = frame;
    has_frame = true;
    portEXIT_CRITICAL(&lock);
}
static bool configured() {
    const String url(WM_FIREBASE_DATABASE_URL);
    return WAREHOUSE_CLOUD_ENABLED && wm_cloud_identifier(WM_FIREBASE_API_KEY, 128) &&
           wm_cloud_identifier(WM_DEVICE_UID, 128) && url.startsWith("https://") &&
           (url.endsWith(".firebaseio.com") || url.endsWith(".firebasedatabase.app")) &&
           url.indexOf('/', 8) < 0 && url.indexOf('?') < 0 && url.indexOf('@') < 0;
}
static String encode(const String &text) {
    String result;
    result.reserve(text.length() * 3);
    const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < text.length(); i++) {
        const uint8_t c = (uint8_t)text[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            result += (char)c;
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}
class BoundedResponse : public Stream {
    String &text;

  public:
    explicit BoundedResponse(String &output) : text(output) {}
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t *bytes, size_t size) override {
        if (size > 8192u - text.length())
            return 0;
        return text.concat(reinterpret_cast<const char *>(bytes), size) ? size : 0;
    }
};
// Credentials/tokens never go to Serial/SD. Verified TLS; no setInsecure() fallback.
static void request_phase(const char *phase) {
    portENTER_CRITICAL(&lock);
    transport.phase = phase;
    portEXIT_CRITICAL(&lock);
}
static int request_finished(int code, WiFiClientSecure *client = nullptr) {
    char message[128] = {};
    const int tls_code = client ? client->lastError(message, sizeof(message)) : 0;
    const uint32_t now = millis();
    const uint32_t heap = ESP.getFreeHeap();
    portENTER_CRITICAL(&lock);
    transport.http_code = code;
    transport.tls_code = tls_code;
    memcpy(transport.tls_message, message, sizeof(message));
    transport.phase = code > 0 ? "HTTP_RESPONSE" : "TRANSPORT_FAILED";
    transport.elapsed_ms = wm_elapsed(now, transport.started_ms);
    transport.heap_after = heap;
    transport.active = false;
    portEXIT_CRITICAL(&lock);
    return code;
}
// Expose the pinned HTTPClient's protected connection step so an upload can take
// its snapshot AFTER verified TLS, rather than aging it during the handshake.
class PreparedHTTPClient : public HTTPClient {
  public:
    bool prepare() { return connect(); }
};
struct RequestChannel {
    WarehouseTLSClient client;
    PreparedHTTPClient http;
    bool active = false;
};
static RequestChannel &database_channel() {
    static RequestChannel channel;
    return channel;
}
static void close_channel(RequestChannel &channel) {
    channel.http.setReuse(false);
    channel.http.end();
    channel.client.stop();
    channel.active = false;
}
static void close_database() {
    RequestChannel &channel = database_channel();
    if (channel.active)
        close_channel(channel);
}
using PrepareBody = bool (*)(String &, void *, uint64_t);
static int request(const String &url, const char *method, const String &body,
                   const char *content_type, String &response, bool probe_tcp = false,
                   PrepareBody prepare_body = nullptr, void *context = nullptr) {
    // One worker owns this connection. Release it before auth/refresh/probe so
    // two live TLS sessions do not compete for ESP32 heap.
    if (!prepare_body)
        close_database();
    // Keep only the hostname in diagnostics: never store URL query tokens/body.
    const int slash = url.indexOf('/', 8);
    const String host = url.substring(8, slash < 0 ? url.length() : (unsigned)slash);
    Transport initial = {};
    initial.kind = probe_tcp                                  ? "PROBE"
                   : host == "identitytoolkit.googleapis.com" ? "LOGIN"
                   : host == "securetoken.googleapis.com"     ? "REFRESH"
                                                              : "UPLOAD";
    initial.phase = "DNS";
    host.toCharArray(initial.host, sizeof(initial.host));
    initial.active = true;
    initial.started_ms = millis();
    initial.epoch_s = (uint32_t)time(nullptr);
    initial.heap_before = ESP.getFreeHeap();
    portENTER_CRITICAL(&lock);
    transport = initial;
    const bool injected_dns = prepare_body && dns_test_fault;
    portEXIT_CRITICAL(&lock);
    IPAddress address;
    // A live verified database socket needs no new DNS lookup. A transient DNS
    // outage must not discard a connection that can still deliver snapshots.
    RequestChannel &database = database_channel();
    const bool live_database = prepare_body && database.active && database.client.connected();
    const wm_cloud_dns_route_t dns_route = wm_cloud_dns_route(live_database, injected_dns);
    const bool resolved =
        dns_route == WM_CLOUD_DNS_REUSE ||
        (dns_route == WM_CLOUD_DNS_LOOKUP && WiFi.hostByName(host.c_str(), address) == 1);
    portENTER_CRITICAL(&lock);
    transport.dns_ok = resolved;
    if (injected_dns)
        transport.error_reason = "DNS_TEST_FAULT";
    portEXIT_CRITICAL(&lock);
    if (!resolved) {
        close_database();
        return request_finished(-1001); // Local diagnostic code: DNS lookup failed.
    }
    if (probe_tcp) {
        request_phase("TCP_PROBE");
        WiFiClient tcp;
        const bool connected = tcp.connect(address, 443, 10000) == 1;
        tcp.stop();
        portENTER_CRITICAL(&lock);
        transport.tcp_tested = true;
        transport.tcp_ok = connected;
        portEXIT_CRITICAL(&lock);
        if (!connected)
            return request_finished(-1003); // Local diagnostic code: TCP probe failed.
    }
    request_phase("HTTPS_CONNECT_AND_HTTP");
    RequestChannel transient;
    RequestChannel &channel = prepare_body ? database_channel() : transient;
    WarehouseTLSClient &client = channel.client;
    PreparedHTTPClient &http = channel.http;
    const bool reused = prepare_body && channel.active && client.connected();
    channel.active = true;
    client.setCACert(WM_ROOT_CA);
    client.setHandshakeTimeout(30);
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.useHTTP10(!prepare_body);
    const auto fail = [&](int code, const char *reason) {
        portENTER_CRITICAL(&lock);
        transport.error_reason = reason;
        portEXIT_CRITICAL(&lock);
        const int result = request_finished(code, &client);
        close_channel(channel);
        return result;
    };
    // The clock probe deliberately has no token and no application data.
    // HEAD may return 405 on RTDB; any verified HTTP response with a valid Date
    // can discipline UTC. Its status is separate from the actual PATCH status.
    if (!http.begin(client, prepare_body ? String("https://") + host + "/" : url))
        return fail(-1002, "HTTP_SETUP_FAILED");
    const char *response_headers[] = {"Date"};
    http.collectHeaders(response_headers, 1);
    String prepared;
    if (prepare_body) {
        http.setReuse(true);
        if (!http.prepare())
            return fail(-1, "TLS_CONNECT_FAILED");
        portENTER_CRITICAL(&lock);
        transport.tls_ready_ms = wm_elapsed(millis(), transport.started_ms);
        transport.connection_reused = reused;
        portEXIT_CRITICAL(&lock);
        request_phase("CLOCK_PROBE");
        const int clock_code = http.sendRequest("HEAD");
        const String clock_date = http.header("Date");
        uint64_t epoch_ms = 0;
        int64_t adjustment = 0;
        const bool clock_ok = clock_code > 0 &&
                              wm_cloud_http_date_ms(clock_date.c_str(), &epoch_ms) &&
                              sync_http_date(clock_date, adjustment);
        portENTER_CRITICAL(&lock);
        transport.clock_http = clock_code;
        portEXIT_CRITICAL(&lock);
        if (!clock_ok)
            return fail(-1005, "CLOCK_PROBE_FAILED");
        if (!client.connected())
            return fail(-1006, "CLOCK_CONNECTION_CLOSED");
        // Same verified origin; rebuild the URI with the token only for PATCH.
        if (!http.begin(client, url))
            return fail(-1002, "HTTP_SETUP_FAILED");
        http.collectHeaders(response_headers, 1);
        request_phase("PREPARE_FRESH_UPLOAD");
        if (!prepare_body(prepared, context, epoch_ms))
            return fail(-1004, "NO_FRESH_SNAPSHOT");
        request_phase("HTTP_UPLOAD");
    }
    http.addHeader("Content-Type", content_type);
    const int code = http.sendRequest(method, prepare_body ? prepared : body);
    // Auth responses are bounded; successful PATCH uses print=silent and returns 204.
    const bool upload_error = strcmp(initial.kind, "UPLOAD") == 0 && code >= 400;
    if ((code == 200 || upload_error) && http.getSize() <= 8192) {
        request_phase("READ_BODY");
        BoundedResponse sink(response);
        if (http.writeToStream(&sink) < 0)
            response = "";
    }
    const char *reason = code >= 400 ? "HTTP_ERROR" : "NONE";
    if (upload_error) {
        JsonDocument error_json;
        if (!deserializeJson(error_json, response)) {
            String message = error_json["error"] | "";
            message.toLowerCase();
            // Whitelist labels only. Never log arbitrary server response bodies.
            if (message.indexOf("permission denied") >= 0)
                reason = "RULES_OR_VALIDATION";
            else if (message.indexOf("expired") >= 0)
                reason = "TOKEN_EXPIRED";
            else if (message.indexOf("auth") >= 0 || message.indexOf("token") >= 0)
                reason = "AUTH_TOKEN_ERROR";
        }
    }
    const String server_date = http.header("Date");
    int64_t clock_adjust_ms = 0;
    const bool clock_synced = code > 0 && sync_http_date(server_date, clock_adjust_ms);
    portENTER_CRITICAL(&lock);
    server_date.toCharArray(transport.server_date, sizeof(transport.server_date));
    transport.error_reason = reason;
    transport.clock_synced = clock_synced;
    transport.clock_adjust_ms = clock_adjust_ms;
    portEXIT_CRITICAL(&lock);
    request_finished(code, &client);
    http.end();
    if (code <= 0)
        close_channel(channel);
    return code;
}
static uint32_t lifetime(JsonVariantConst value) {
    const long seconds =
        value.is<const char *>() ? atol(value.as<const char *>()) : value.as<long>();
    return seconds >= 120 && seconds <= 7200 ? (uint32_t)seconds * 1000u : 0;
}
static void make_payload(JsonDocument &payload, const wm_cloud_frame_t &f, uint64_t epoch_ms) {
    char boot[9];
    snprintf(boot, sizeof(boot), "%08lx", (unsigned long)f.reading.session_id);
    payload["schema_version"] = 1;
    payload["boot_id"] = boot;
    payload["sequence"] = f.reading.sequence;
    payload["uptime_ms"] = f.reading.uptime_ms;
    payload["captured_at_ms"] = epoch_ms - wm_elapsed(millis(), f.reading.uptime_ms);
    payload["received_at_ms"][".sv"] = "timestamp";
    payload["sensor_valid"] = f.reading.sensor_valid;
    if (f.reading.sensor_valid) {
        payload["temperature_c"] = f.reading.temperature_cc / 100.0;
        payload["humidity_rh_pct"] = f.reading.humidity_crh / 100.0;
    }
    payload["sample_age_ms"] = f.sample_age_ms;
    payload["door_open"] = f.reading.door_open;
    payload["door_open_ms"] = f.reading.door_open_ms;
    payload["alarm_mask"] = f.reading.alarm_mask;
    payload["muted"] = f.reading.muted;
    payload["sensor_errors"] = f.reading.read_errors;
    payload["storage_state"] = f.storage_code;
    payload["storage_samples"] = f.storage_samples;
    payload["storage_skipped"] = f.storage_skipped;
    payload["storage_queue_dropped"] = f.storage_queue_dropped;
    payload["test_sensor_fault"] = f.reading.test_sensor_fault;
}

struct UploadPreparation {
    UploadEvidence *evidence;
    uint32_t token_issued;
    uint32_t valid_ms;
};
static bool prepare_upload(String &body, void *context, uint64_t epoch_ms) {
    const UploadPreparation &preparation = *static_cast<UploadPreparation *>(context);
    wm_cloud_frame_t frame = {};
    portENTER_CRITICAL(&lock);
    const bool available = has_frame;
    if (available) {
        frame = latest;
        has_frame = false;
    }
    portEXIT_CRITICAL(&lock);
    if (!available)
        return false;
    UploadEvidence &attempt = *preparation.evidence;
    JsonDocument payload, patch;
    make_payload(payload, frame, epoch_ms);
    attempt.truncated = measureJson(payload) >= sizeof(attempt.payload);
    if (!attempt.truncated)
        serializeJson(payload, attempt.payload, sizeof(attempt.payload));
    attempt.token_age_ms = wm_elapsed(millis(), preparation.token_issued);
    attempt.token_ttl_ms = preparation.valid_ms;
    attempt.snapshot_after_tls = true;
    patch["latest"] = payload.as<JsonObject>();
    patch[String("history/") + attempt.slot] = payload.as<JsonObject>();
    serializeJson(patch, body);
    return true;
}

static void worker(void *) {
    if (!configured()) {
        state(WAREHOUSE_CLOUD_ENABLED ? CONFIG_REQUIRED : CLOUD_DISABLED);
        vTaskDelete(nullptr);
    }
    WiFi.persistent(false);
    WiFi.onEvent(wifi_event);
    const bool mode_ready = WiFi.mode(WIFI_STA);
    portENTER_CRITICAL(&lock);
    stats.wifi_mode_ready = mode_ready;
    stats.wifi_begin_result = -1;
    portEXIT_CRITICAL(&lock);
    String id_token, refresh_token;
    uint64_t token_deadline_ms = 0;
    uint32_t token_issued = 0, valid_ms = 0, last_attempt = 0, failures = 0;
    wm_cloud_network_t network = {};
    uint8_t history_slot = 0;
    bool time_started = false;
    for (;;) {
        Credentials login = {};
        portENTER_CRITICAL(&lock);
        const bool enabled = requested;
        const bool logout = pending_logout;
        pending_logout = false;
        bool signing_in = enabled && pending_login;
        if (signing_in) {
            login = credentials;
            erase(&credentials, sizeof(credentials));
            pending_login = false;
        }
        portEXIT_CRITICAL(&lock);
        if (logout) {
            close_database();
            id_token = "";
            refresh_token = "";
            valid_ms = 0;
            token_deadline_ms = 0;
        }
        if (!enabled) {
            close_database();
            if (WiFi.getMode() != WIFI_OFF) {
                WiFi.disconnect(true, false);
                WiFi.mode(WIFI_OFF);
            }
            network.attempted = false;
            network.dns_failures = 0;
            state(PAUSED);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!signing_in && wm_cloud_dns_reset_due(&network, millis())) {
            close_database();
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_OFF);
            wm_cloud_network_reset(&network, millis());
            portENTER_CRITICAL(&lock);
            stats.wifi_recoveries++;
            portEXIT_CRITICAL(&lock);
            state(WIFI_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            close_database();
            if (signing_in) {
                portENTER_CRITICAL(&lock);
                if (!pending_login) {
                    credentials = login;
                    pending_login = true;
                }
                portEXIT_CRITICAL(&lock);
                erase(&login, sizeof(login));
            }
            if (wm_cloud_wifi_due(&network, millis())) {
                if (network.attempted) {
                    // Give association/DHCP a full minute before a controlled retry.
                    WiFi.disconnect(true, false);
                    WiFi.mode(WIFI_OFF);
                    wm_cloud_network_reset(&network, millis());
                    portENTER_CRITICAL(&lock);
                    stats.wifi_recoveries++;
                    portEXIT_CRITICAL(&lock);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                const bool ready = WiFi.mode(WIFI_STA);
                const int result = WiFi.begin("Wokwi-GUEST", "", 6);
                portENTER_CRITICAL(&lock);
                stats.wifi_mode_ready = ready;
                stats.wifi_begin_result = result;
                stats.wifi_begin_calls++;
                portEXIT_CRITICAL(&lock);
                network.last_begin = millis();
                network.attempted = true;
            }
            state(WIFI_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!time_started) {
            // The SDK default is hours of simulated time. Refresh more often,
            // and discipline UTC from each verified HTTP response as well.
            esp_sntp_set_sync_interval(15000);
            configTime(0, 0, "time.google.com", "pool.ntp.org");
            time_started = true;
        }
        if (time(nullptr) < 1704067200) {
            if (signing_in) {
                portENTER_CRITICAL(&lock);
                if (!pending_login) {
                    credentials = login;
                    pending_login = true;
                }
                portEXIT_CRITICAL(&lock);
                erase(&login, sizeof(login));
            }
            state(TIME_SYNC);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        portENTER_CRITICAL(&lock);
        const bool probing = pending_probe && !signing_in;
        if (probing) {
            pending_probe = false;
            probe_active = true;
        }
        portEXIT_CRITICAL(&lock);
        if (probing) {
            String unused_response;
            const int result = request("https://identitytoolkit.googleapis.com/", "GET", "",
                                       "application/json", unused_response, true);
            portENTER_CRITICAL(&lock);
            stats.last_http = result;
            probe_active = false;
            portEXIT_CRITICAL(&lock);
            // No credentials are sent; a root-path 404 is a valid HTTP/TLS result.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (signing_in) {
            state(AUTHENTICATING);
            JsonDocument auth;
            auth["email"] = login.email;
            auth["password"] = login.password;
            auth["returnSecureToken"] = true;
            String body, response;
            serializeJson(auth, body);
            const int code = request(
                String(
                    "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") +
                    WM_FIREBASE_API_KEY,
                "POST", body, "application/json", response);
            erase(&login, sizeof(login));
            auth.clear();
            body = "";
            JsonDocument result;
            if (code != 200 || deserializeJson(result, response) ||
                String(result["localId"] | "") != WM_DEVICE_UID ||
                !(valid_ms = lifetime(result["expiresIn"]))) {
                id_token = "";
                refresh_token = "";
                state(code <= 0 ? NETWORK_ERROR : AUTH_FAILED, code);
            } else {
                id_token = result["idToken"].as<String>();
                refresh_token = result["refreshToken"].as<String>();
                token_issued = millis();
                token_deadline_ms = utc_now_ms() + valid_ms;
                failures = 0;
                last_attempt = millis() - 5000u;
                state(ONLINE);
            }
        }
        if (!id_token.length()) {
            const Code current = snapshot().code;
            if (current != AUTH_FAILED && current != NETWORK_ERROR)
                state(NEED_LOGIN);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (wm_elapsed(millis(), last_attempt) < wm_cloud_backoff(failures)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        last_attempt = millis();
        if (wm_cloud_token_expiring(millis(), token_issued, valid_ms) ||
            wm_cloud_utc_token_expiring(utc_now_ms(), token_deadline_ms)) {
            String response;
            const int code = request(
                String("https://securetoken.googleapis.com/v1/token?key=") + WM_FIREBASE_API_KEY,
                "POST", String("grant_type=refresh_token&refresh_token=") + encode(refresh_token),
                "application/x-www-form-urlencoded", response);
            JsonDocument result;
            if (code != 200 || deserializeJson(result, response) ||
                String(result["user_id"] | "") != WM_DEVICE_UID ||
                !(valid_ms = lifetime(result["expires_in"]))) {
                if (code == 400 || code == 401 || code == 403) {
                    id_token = "";
                    refresh_token = "";
                    state(AUTH_FAILED, code);
                } else {
                    failures++;
                    state(BACKOFF, code);
                }
                continue;
            }
            id_token = result["id_token"].as<String>();
            refresh_token = result["refresh_token"].as<String>();
            token_issued = millis();
            token_deadline_ms = utc_now_ms() + valid_ms;
        }
        portENTER_CRITICAL(&lock);
        const bool available = has_frame;
        portEXIT_CRITICAL(&lock);
        if (!available)
            continue;
        UploadEvidence attempt = {};
        attempt.slot = history_slot;
        UploadPreparation preparation = {&attempt, token_issued, valid_ms};
        String response;
        const int code =
            request(String(WM_FIREBASE_DATABASE_URL) + "/devices/" + WM_DEVICE_ID +
                        ".json?print=silent&auth=" + encode(id_token),
                    "PATCH", "", "application/json", response, false, prepare_upload, &preparation);
        wm_cloud_dns_result(&network, code);
        attempt.http = code;
        attempt.available = attempt.snapshot_after_tls;
        portENTER_CRITICAL(&lock);
        memcpy(attempt.server_date, transport.server_date, sizeof(attempt.server_date));
        attempt.error_reason = transport.error_reason;
        attempt.clock_synced = transport.clock_synced;
        attempt.clock_adjust_ms = transport.clock_adjust_ms;
        attempt.clock_http = transport.clock_http;
        attempt.connection_reused = transport.connection_reused;
        upload_evidence = attempt;
        portEXIT_CRITICAL(&lock);
        if (code == 200 || code == 204) {
            failures = 0;
            history_slot = (history_slot + 1u) % 60u;
            portENTER_CRITICAL(&lock);
            stats.code = ONLINE;
            stats.last_http = code;
            stats.published++;
            stats.last_success_ms = millis();
            portEXIT_CRITICAL(&lock);
        } else {
            if (attempt.error_reason && strcmp(attempt.error_reason, "TOKEN_EXPIRED") == 0)
                token_deadline_ms = 0; // Refresh once on the next backoff opportunity.
            failures++;
            state(code == 401 || code == 403 ? PERMISSION_DENIED : BACKOFF, code);
            portENTER_CRITICAL(&lock);
            stats.failures++;
            portEXIT_CRITICAL(&lock);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
static void begin() {
    state(WAREHOUSE_CLOUD_ENABLED ? CONFIG_REQUIRED : CLOUD_DISABLED);
    if (WAREHOUSE_CLOUD_ENABLED &&
        xTaskCreatePinnedToCore(worker, "firebase", 12288, nullptr, WORKER_PRIORITY, &task_handle,
                                0) != pdPASS)
        state(BACKOFF);
}
static bool command(const char *text) {
    if (strcmp(text, "cloud payload") == 0) {
        portENTER_CRITICAL(&lock);
        const UploadEvidence evidence = upload_evidence;
        portEXIT_CRITICAL(&lock);
        Serial.printf(
            "UPLOAD_DIAG revision=1 available=%u http=%d reason=%s slot=%u "
            "token_age_ms=%lu token_ttl_ms=%lu truncated=%u clock_synced=%u "
            "clock_adjust_ms=%lld snapshot_after_tls=%u clock_http=%d reused=%u server_date=%s\n",
            evidence.available ? 1u : 0u, evidence.http,
            evidence.error_reason ? evidence.error_reason : "NONE", evidence.slot,
            (unsigned long)evidence.token_age_ms, (unsigned long)evidence.token_ttl_ms,
            evidence.truncated ? 1u : 0u, evidence.clock_synced ? 1u : 0u,
            (long long)evidence.clock_adjust_ms, evidence.snapshot_after_tls ? 1u : 0u,
            evidence.clock_http, evidence.connection_reused ? 1u : 0u, evidence.server_date);
        if (evidence.available && !evidence.truncated)
            Serial.printf("UPLOAD_PAYLOAD %s\n", evidence.payload);
        return true;
    }
    if (strcmp(text, "cloud status") == 0) {
        const Stats s = snapshot();
        Serial.printf("CLOUD_STATUS state=%s published=%lu failures=%lu coalesced=%lu last_http=%d "
                      "last_success_ms=%lu worker_priority=%u\n",
                      status_text(s.code), (unsigned long)s.published, (unsigned long)s.failures,
                      (unsigned long)s.coalesced, s.last_http, (unsigned long)s.last_success_ms,
                      (unsigned)WORKER_PRIORITY);
        Serial.printf("WIFI_DIAG revision=1 status=%d mode=%d mode_ready=%u begin_calls=%lu "
                      "begin_result=%d disconnects=%lu last_reason=%u got_ip=%lu ip=%s\n",
                      (int)WiFi.status(), (int)WiFi.getMode(), s.wifi_mode_ready ? 1u : 0u,
                      (unsigned long)s.wifi_begin_calls, s.wifi_begin_result,
                      (unsigned long)s.wifi_disconnects, (unsigned)s.wifi_last_reason,
                      (unsigned long)s.wifi_got_ip, WiFi.localIP().toString().c_str());
        portENTER_CRITICAL(&lock);
        const bool test_dns = dns_test_fault;
        portEXIT_CRITICAL(&lock);
        Serial.printf("NETWORK_RECOVERY revision=2 radio_resets=%lu retry_ms=%lu dns_test=%u\n",
                      (unsigned long)s.wifi_recoveries, (unsigned long)WM_CLOUD_WIFI_RETRY_MS,
                      test_dns ? 1u : 0u);
        portENTER_CRITICAL(&lock);
        const Transport t = transport;
        portEXIT_CRITICAL(&lock);
        Serial.printf(
            "HTTPS_DIAG revision=1 kind=%s phase=%s active=%u dns=%u tcp_tested=%u "
            "tcp_ok=%u http=%d tls=%d elapsed_ms=%lu epoch_s=%lu heap_before=%lu "
            "heap_after=%lu tls_ready_ms=%lu clock_http=%d reused=%u host=%s tls_message=%s\n",
            t.kind ? t.kind : "NONE", t.phase ? t.phase : "IDLE", t.active ? 1u : 0u,
            t.dns_ok ? 1u : 0u, t.tcp_tested ? 1u : 0u, t.tcp_ok ? 1u : 0u, t.http_code, t.tls_code,
            (unsigned long)(t.active ? wm_elapsed(millis(), t.started_ms) : t.elapsed_ms),
            (unsigned long)t.epoch_s, (unsigned long)t.heap_before, (unsigned long)t.heap_after,
            (unsigned long)t.tls_ready_ms, t.clock_http, t.connection_reused ? 1u : 0u, t.host,
            t.tls_message);
        return true;
    }
    if (strcmp(text, "cloud dnsfail on") == 0 || strcmp(text, "cloud dnsfail off") == 0) {
        const bool enabled = strcmp(text, "cloud dnsfail on") == 0;
        portENTER_CRITICAL(&lock);
        dns_test_fault = enabled;
        portEXIT_CRITICAL(&lock);
        Serial.printf("DNS_TEST_FAULT active=%u; simulated upload DNS failure; "
                      "in-flight requests finish before the change applies\n",
                      enabled ? 1u : 0u);
        return true;
    }
    if (strcmp(text, "cloud probe") == 0) {
        if (!task_handle || !configured()) {
            Serial.println("CLOUD_CONFIG_REQUIRED");
            return true;
        }
        portENTER_CRITICAL(&lock);
        const bool busy = pending_probe || probe_active;
        if (!busy)
            pending_probe = true;
        portEXIT_CRITICAL(&lock);
        Serial.println(busy ? "CLOUD_PROBE_BUSY"
                            : "CLOUD_PROBE_QUEUED; no Firebase credentials sent");
        return true;
    }
    if (strcmp(text, "cloud off") == 0 || strcmp(text, "cloud on") == 0 ||
        strcmp(text, "cloud logout") == 0) {
        portENTER_CRITICAL(&lock);
        if (strcmp(text, "cloud logout") == 0) {
            pending_logout = true;
            pending_login = false;
            erase(&credentials, sizeof(credentials));
        } else
            requested = strcmp(text, "cloud on") == 0;
        portEXIT_CRITICAL(&lock);
        Serial.println("CLOUD_CONTROL_REQUESTED; inspect cloud status for completion");
        return true;
    }
    if (strncmp(text, "cloud login ", 12) != 0)
        return false;
    if (!task_handle || !configured()) {
        Serial.println("CLOUD_CONFIG_REQUIRED; configure public identifiers first");
        return true;
    }
    const char *email = text + 12, *space = strchr(email, ' ');
    if (!space || space == email || (size_t)(space - email) >= sizeof(credentials.email) ||
        strlen(space + 1) < 6 || strlen(space + 1) >= sizeof(credentials.password)) {
        Serial.println(
            "CLOUD_LOGIN_FORMAT: cloud login <device-email> <password>; password not logged");
        return true;
    }
    portENTER_CRITICAL(&lock);
    const bool busy = pending_login;
    if (!busy) {
        erase(&credentials, sizeof(credentials));
        memcpy(credentials.email, email, space - email);
        memcpy(credentials.password, space + 1, strlen(space + 1));
        pending_login = true;
    }
    portEXIT_CRITICAL(&lock);
    Serial.println(busy ? "CLOUD_LOGIN_BUSY"
                        : "CLOUD_LOGIN_QUEUED; credentials stay in RAM and are not logged");
    return true;
}
} // namespace cloud
#endif
