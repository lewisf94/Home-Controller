/*
 * app_core_wifi -- shared STA WiFi connect + resilient background reconnect,
 * used by both waveshare/esp-idf (direct Spotify) and waveshare/esp-idf-ha.
 *
 * Behaviour (matches the direct-Spotify build's original, hardware-verified
 * logic): connects synchronously, retrying immediately up to max_retry times.
 * If those fast retries are exhausted, a background esp_timer keeps trying
 * every 20 s forever -- a router reboot or brief out-of-range no longer needs
 * a power cycle to recover from. The initial call still returns ESP_FAIL in
 * that case so the caller can log/continue; the background timer keeps going
 * regardless and the caller is not notified when it eventually succeeds
 * (poll WiFi state via esp_netif / the next network operation as usual).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up esp_netif/event loop/STA WiFi and blocks until either connected
 * or max_retry immediate retries are exhausted (background reconnect is then
 * armed regardless of the return value). ssid/password are read once during
 * this call -- no need to keep them alive afterwards.
 *
 * on_first_connect, if non-NULL, fires exactly once, the first time an IP
 * lease is ever acquired (whether that's this call or a later background
 * reconnect). Runs on the WiFi event task -- keep it short and non-blocking.
 * Pass NULL for no callback. */
esp_err_t app_core_wifi_connect(const char *ssid, const char *password,
                                uint32_t max_retry,
                                void (*on_first_connect)(void));

#ifdef __cplusplus
}
#endif
