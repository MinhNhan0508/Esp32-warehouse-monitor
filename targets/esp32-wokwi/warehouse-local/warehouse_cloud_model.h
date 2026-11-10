#ifndef WAREHOUSE_CLOUD_MODEL_H
#define WAREHOUSE_CLOUD_MODEL_H
#include "warehouse_log.h"
#include <string.h>

// Strict IMF-fixdate parser. UTC arithmetic is independent of TZ and time_t width.
static inline bool wm_cloud_http_date_ms(const char *text, uint64_t *epoch_ms) {
    if (!text || !epoch_ms || strlen(text) != 29 || text[3] != ',' || text[4] != ' ' ||
        text[7] != ' ' || text[11] != ' ' || text[16] != ' ' || text[19] != ':' ||
        text[22] != ':' || strcmp(text + 25, " GMT") != 0)
        return false;
    const unsigned positions[] = {5, 6, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i)
        if (text[positions[i]] < '0' || text[positions[i]] > '9')
            return false;
    const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    unsigned month = 0;
    for (; month < 12; ++month)
        if (memcmp(text + 8, months + month * 3, 3) == 0)
            break;
    const unsigned year = (text[12] - '0') * 1000u + (text[13] - '0') * 100u +
                          (text[14] - '0') * 10u + text[15] - '0';
    const unsigned day = (text[5] - '0') * 10u + text[6] - '0';
    const unsigned hour = (text[17] - '0') * 10u + text[18] - '0';
    const unsigned minute = (text[20] - '0') * 10u + text[21] - '0';
    const unsigned second = (text[23] - '0') * 10u + text[24] - '0';
    if (year < 2024 || year > 2099 || month >= 12 || hour > 23 || minute > 59 || second > 59)
        return false;
    static const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (!day || day > lengths[month] + (month == 1 && leap ? 1u : 0u))
        return false;
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; ++y)
        days += 365u + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 1u : 0u);
    for (unsigned m = 0; m < month; ++m)
        days += lengths[m] + (m == 1 && leap ? 1u : 0u);
    days += day - 1;
    const char weekdays[] = "SunMonTueWedThuFriSat";
    if (memcmp(text, weekdays + ((days + 4) % 7) * 3, 3) != 0)
        return false;
    *epoch_ms = ((days * 24u + hour) * 3600u + minute * 60u + second) * 1000u;
    return true;
}
static inline bool wm_cloud_utc_token_expiring(uint64_t now_ms, uint64_t deadline_ms) {
    return !deadline_ms || now_ms >= deadline_ms || deadline_ms - now_ms <= 60000u;
}
typedef struct {
    wm_log_record_t reading;
    uint32_t sample_age_ms;
    uint32_t storage_samples;
    uint32_t storage_skipped;
    uint32_t storage_queue_dropped;
    uint8_t storage_code;
} wm_cloud_frame_t;
// Explicit diagnostic failure overrides even a live socket, allowing the real
// retry/radio-recovery path to be exercised without changing host networking.
typedef enum {
    WM_CLOUD_DNS_FAIL = 0,
    WM_CLOUD_DNS_REUSE,
    WM_CLOUD_DNS_LOOKUP
} wm_cloud_dns_route_t;
static inline wm_cloud_dns_route_t wm_cloud_dns_route(bool live_socket, bool injected) {
    return injected ? WM_CLOUD_DNS_FAIL : live_socket ? WM_CLOUD_DNS_REUSE : WM_CLOUD_DNS_LOOKUP;
}
// Bounded radio recovery policy, independent of Arduino and tested at tick wrap.
#define WM_CLOUD_WIFI_RETRY_MS 60000u
typedef struct {
    uint32_t last_begin;
    uint32_t last_reset;
    uint8_t dns_failures;
    bool attempted;
    bool reset_seen;
} wm_cloud_network_t;
static inline bool wm_cloud_wifi_due(const wm_cloud_network_t *n, uint32_t now) {
    return !n->attempted || wm_elapsed(now, n->last_begin) >= WM_CLOUD_WIFI_RETRY_MS;
}
static inline void wm_cloud_dns_result(wm_cloud_network_t *n, int code) {
    if (code != -1001)
        n->dns_failures = 0;
    else if (n->dns_failures < 3)
        n->dns_failures++;
}
static inline bool wm_cloud_dns_reset_due(const wm_cloud_network_t *n, uint32_t now) {
    return n->dns_failures >= 3 &&
           (!n->reset_seen || wm_elapsed(now, n->last_reset) >= WM_CLOUD_WIFI_RETRY_MS);
}
static inline void wm_cloud_network_reset(wm_cloud_network_t *n, uint32_t now) {
    n->last_reset = now;
    n->reset_seen = true;
    n->attempted = false;
    n->dns_failures = 0;
}
static inline uint32_t wm_cloud_backoff(uint32_t failures) {
    if (!failures)
        return 2000;
    if (failures >= 6)
        return 30000;
    return 1000u << (failures - 1u);
}
static inline bool wm_cloud_token_expiring(uint32_t now, uint32_t issued, uint32_t valid_ms) {
    return valid_ms <= 60000u || wm_elapsed(now, issued) >= valid_ms - 60000u;
}
static inline bool wm_cloud_identifier(const char *text, size_t maximum) {
    if (!text || !*text)
        return false;
    size_t length = 0;
    for (; text[length]; length++) {
        const char c = text[length];
        if (length >= maximum || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                   (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}
#endif
