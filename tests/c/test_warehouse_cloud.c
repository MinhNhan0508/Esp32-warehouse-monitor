#include "warehouse_cloud_model.h"
static void output(const char *text) { fputs(text, stdout); }
static void finish(int code) { (void)code; }

int main(void) {
    unsigned failures = 0;
    const uint32_t expected[] = {2000, 1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        failures += wm_cloud_backoff(i) != expected[i];
    failures += wm_cloud_backoff(UINT32_MAX) != 30000;
    output(failures ? "FAIL retry_bounds\n" : "PASS retry_bounds\n");
    unsigned prior = failures;
    const uint32_t issued = UINT32_MAX - 999u;
    failures += wm_cloud_token_expiring(issued + 3539999u, issued, 3600000u);
    failures += !wm_cloud_token_expiring(issued + 3540000u, issued, 3600000u);
    failures += !wm_cloud_token_expiring(0, 0, 60000);
    failures += !wm_cloud_token_expiring(0, 0, 0);
    output(prior == failures ? "PASS token_rollover\n" : "FAIL token_rollover\n");
    prior = failures;
    failures += !wm_cloud_identifier("warehouse-01_A", 14);
    failures += wm_cloud_identifier("warehouse-01_A", 13);
    failures += wm_cloud_identifier("a/b", 128);
    failures += wm_cloud_identifier("a.b", 128);
    failures += wm_cloud_identifier("", 128);
    failures += wm_cloud_identifier(NULL, 128);
    output(prior == failures ? "PASS identifier_bounds\n" : "FAIL identifier_bounds\n");
    prior = failures;
    uint64_t epoch = 0;
    failures += !wm_cloud_http_date_ms("Tue, 03 Nov 2026 01:54:21 GMT", &epoch);
    failures += epoch != UINT64_C(1793670861000);
    failures += !wm_cloud_http_date_ms("Thu, 29 Feb 2024 00:00:00 GMT", &epoch);
    failures += epoch != UINT64_C(1709164800000);
    failures += !wm_cloud_http_date_ms("Sun, 01 Jan 2040 00:00:00 GMT", &epoch);
    failures += epoch != UINT64_C(2208988800000);
    const char *bad_dates[] = {
        "Mon, 03 Nov 2026 01:54:21 GMT",       "Tue, 03 Nov 2026 24:00:00 GMT",
        "Tue, 03 Nov 2026 01:60:00 GMT",       "Tue, 03 Nov 2026 01:54:60 GMT",
        "Thu, 29 Feb 2025 00:00:00 GMT",       "Tue, 31 Nov 2026 01:54:21 GMT",
        "Tue, 03 Xxx 2026 01:54:21 GMT",       "Tue, 03 Nov 2026 01:54:21 UTC",
        "Tue, 03 Nov 2026 01:54:21 GMT extra", "",
        "Wed, 01 Jan 2100 00:00:00 GMT"};
    for (unsigned i = 0; i < sizeof(bad_dates) / sizeof(bad_dates[0]); ++i) {
        epoch = 99;
        failures += wm_cloud_http_date_ms(bad_dates[i], &epoch);
        failures += epoch != 99;
    }
    failures += wm_cloud_http_date_ms(NULL, &epoch);
    failures += wm_cloud_http_date_ms("Tue, 03 Nov 2026 01:54:21 GMT", NULL);
    output(prior == failures ? "PASS utc_http_date\n" : "FAIL utc_http_date\n");
    prior = failures;
    const uint64_t deadline = UINT64_C(1793670861000) + 3600000u;
    failures += wm_cloud_utc_token_expiring(deadline - 60001u, deadline);
    failures += !wm_cloud_utc_token_expiring(deadline - 60000u, deadline);
    failures += !wm_cloud_utc_token_expiring(deadline + 1000u, deadline);
    failures += !wm_cloud_utc_token_expiring(0, 0);
    output(prior == failures ? "PASS utc_token_expiry\n" : "FAIL utc_token_expiry\n");
    prior = failures;
    wm_cloud_network_t network = {0};
    failures += !wm_cloud_wifi_due(&network, 0);
    network.attempted = true;
    network.last_begin = UINT32_MAX - 999u;
    failures += wm_cloud_wifi_due(&network, 58999u);
    failures += !wm_cloud_wifi_due(&network, 59000u);
    wm_cloud_dns_result(&network, -1001);
    wm_cloud_dns_result(&network, -1001);
    failures += wm_cloud_dns_reset_due(&network, 0);
    wm_cloud_dns_result(&network, 204);
    failures += network.dns_failures != 0;
    for (unsigned i = 0; i < 10; i++)
        wm_cloud_dns_result(&network, -1001);
    failures += network.dns_failures != 3;
    failures += !wm_cloud_dns_reset_due(&network, UINT32_MAX - 999u);
    wm_cloud_network_reset(&network, UINT32_MAX - 999u);
    failures += network.attempted || network.dns_failures != 0;
    failures += !wm_cloud_wifi_due(&network, 0);
    for (unsigned i = 0; i < 3; i++)
        wm_cloud_dns_result(&network, -1001);
    failures += wm_cloud_dns_reset_due(&network, 58999u);
    failures += !wm_cloud_dns_reset_due(&network, 59000u);
    wm_cloud_dns_result(&network, -1); // TLS failure is not a DNS failure.
    failures += wm_cloud_dns_reset_due(&network, 59000u);
    output(prior == failures ? "PASS network_recovery\n" : "FAIL network_recovery\n");
    prior = failures;
    failures += wm_cloud_dns_route(false, false) != WM_CLOUD_DNS_LOOKUP;
    failures += wm_cloud_dns_route(true, false) != WM_CLOUD_DNS_REUSE;
    failures += wm_cloud_dns_route(false, true) != WM_CLOUD_DNS_FAIL;
    failures += wm_cloud_dns_route(true, true) != WM_CLOUD_DNS_FAIL;
    output(prior == failures ? "PASS dns_injection_route\n" : "FAIL dns_injection_route\n");
    output(failures ? "WAREHOUSE_CLOUD_FAILED\n" : "WAREHOUSE_CLOUD_PASS\n");
    finish(failures ? 1 : 0);
    return failures ? 1 : 0;
}
