#ifndef WAREHOUSE_TLS_H
#define WAREHOUSE_TLS_H
#include <WiFiClientSecure.h>
#include <esp_arduino_version.h>

// This adapter uses the TLS preparation/handshake split in the pinned core.
// No application bytes may be written before the verified handshake succeeds.
#if ESP_ARDUINO_VERSION_MAJOR == 3 && ESP_ARDUINO_VERSION_MINOR == 3 &&                            \
    ESP_ARDUINO_VERSION_PATCH == 12
static constexpr const char *WAREHOUSE_TLS_PROFILE = "ECDHE_RSA";
class WarehouseTLSClient : public WiFiClientSecure {
  public:
    using WiFiClientSecure::connect;
    int connect(const char *host, uint16_t port) override { return connect(host, port, 10000); }
    int connect(const char *host, uint16_t port, int32_t timeout) override {
        if (!host || !_CA_cert)
            return 0;
        _timeout = timeout;
        setPlainStart();
        // Avoid the three-argument base overload: it virtually dispatches back here.
        if (!WiFiClientSecure::connect(host, port, _CA_cert, _cert, _private_key))
            return 0;
        // Rebuild the SSL context before modifying its configuration. TCP is open,
        // but no handshake/application data has been sent by setPlainStart().
        mbedtls_ssl_free(&sslclient->ssl_ctx);
        mbedtls_ssl_init(&sslclient->ssl_ctx);
        static const int suites[] = {MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                                     MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 0};
        mbedtls_ssl_conf_ciphersuites(&sslclient->ssl_conf, suites);
        mbedtls_ssl_conf_authmode(&sslclient->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_min_tls_version(&sslclient->ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&sslclient->ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
        int result = mbedtls_ssl_setup(&sslclient->ssl_ctx, &sslclient->ssl_conf);
        if (!result)
            result = mbedtls_ssl_set_hostname(&sslclient->ssl_ctx, host);
        if (!result) {
            mbedtls_ssl_set_bio(&sslclient->ssl_ctx, &sslclient->socket, mbedtls_net_send,
                                mbedtls_net_recv, nullptr);
            result = ssl_starttls_handshake(sslclient.get());
        }
        if (result >= 0 && mbedtls_ssl_get_verify_result(&sslclient->ssl_ctx) != 0)
            result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
        sslclient->last_error = result < 0 ? result : 0;
        if (result < 0) {
            stop();
            return 0;
        }
        _stillinPlainStart = false;
        return 1;
    }
};
#else
#if WAREHOUSE_CLOUD_ENABLED
#error                                                                                             \
    "Cloud demo requires Arduino-ESP32 3.3.12. Build tools/warehouse_firmware.py and upload the merged image."
#endif
static constexpr const char *WAREHOUSE_TLS_PROFILE = "CLOUD_DISABLED";
using WarehouseTLSClient = WiFiClientSecure;
#endif
#endif
