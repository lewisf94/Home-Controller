#include "sendspin_player.h"

#include "audio_stream_bridge.h"
#include "sendspin/client.h"
#include "sendspin/player_role.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <algorithm>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace {

using sendspin::AudioSupportedFormatObject;
using sendspin::MemoryLocation;
using sendspin::PlayerRole;
using sendspin::PlayerRoleConfig;
using sendspin::PlayerRoleListener;
using sendspin::SendspinClient;
using sendspin::SendspinClientConfig;
using sendspin::SendspinClientState;
using sendspin::SendspinCodecFormat;
using sendspin::SendspinNetworkProvider;
using sendspin::SendspinPersistenceProvider;

constexpr char TAG[] = "sendspin_player";
constexpr char PLAYER_NAME[] = "Home Controller";
constexpr char NVS_NAMESPACE[] = "sendspin";
constexpr uint8_t DEFAULT_VOLUME = 60;
constexpr size_t AUDIO_BUFFER_BYTES = 512 * 1024;

SendspinClient *s_client = nullptr;
PlayerRole *s_player = nullptr;
TaskHandle_t s_loop_task = nullptr;
bool s_started = false;
uint8_t s_volume = DEFAULT_VOLUME;
bool s_muted = false;
uint8_t s_channels = 1;
uint8_t s_bits_per_sample = 16;

bool nvs_write_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

std::optional<uint32_t> nvs_read_u32(const char *key)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return std::nullopt;
    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(handle, key, &value);
    nvs_close(handle);
    if (err != ESP_OK) return std::nullopt;
    return value;
}

bool nvs_write_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

uint8_t nvs_read_u8(const char *key, uint8_t fallback)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

class NetworkProvider final : public SendspinNetworkProvider {
public:
    bool is_network_ready() override
    {
        wifi_ap_record_t ap{};
        return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    }
};

class PersistenceProvider final : public SendspinPersistenceProvider {
public:
    bool save_last_server_hash(uint32_t hash) override
    {
        return nvs_write_u32("last_srv", hash);
    }

    std::optional<uint32_t> load_last_server_hash() override
    {
        return nvs_read_u32("last_srv");
    }

    bool save_static_delay(uint16_t delay_ms) override
    {
        return nvs_write_u32("delay_ms", delay_ms);
    }

    std::optional<uint16_t> load_static_delay() override
    {
        auto value = nvs_read_u32("delay_ms");
        if (!value || *value > 5000) return std::nullopt;
        return static_cast<uint16_t>(*value);
    }
};

class PlayerListener final : public PlayerRoleListener {
public:
    size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override
    {
        size_t written = audio_stream_write(data, length, timeout_ms);
        const size_t bytes_per_frame = static_cast<size_t>(s_channels) * s_bits_per_sample / 8;
        if (written && bytes_per_frame && s_player) {
            s_player->notify_audio_played(static_cast<uint32_t>(written / bytes_per_frame),
                                          esp_timer_get_time());
        }
        return written;
    }

    void on_stream_start() override
    {
        if (!s_player) return;
        const auto &params = s_player->get_current_stream_params();
        if (!params.is_complete()) {
            ESP_LOGE(TAG, "stream start missing format parameters");
            return;
        }

        s_channels = *params.channels;
        s_bits_per_sample = *params.bit_depth;
        bool ok = audio_stream_begin(*params.sample_rate, s_channels, s_bits_per_sample);
        if (s_client) {
            s_client->update_state(ok ? SendspinClientState::SYNCHRONIZED
                                      : SendspinClientState::ERROR);
        }
        if (!ok) ESP_LOGE(TAG, "ES8311 rejected negotiated Sendspin format");
    }

    void on_stream_end() override
    {
        audio_stream_end();
        if (s_client) s_client->update_state(SendspinClientState::SYNCHRONIZED);
    }

    void on_volume_changed(uint8_t volume) override
    {
        s_volume = std::min<uint8_t>(volume, 100);
        audio_stream_set_volume(s_volume);
        nvs_write_u8("volume", s_volume);
    }

    void on_mute_changed(bool muted) override
    {
        s_muted = muted;
        audio_stream_set_muted(s_muted);
        nvs_write_u8("muted", s_muted ? 1 : 0);
    }
};

NetworkProvider s_network_provider;
PersistenceProvider s_persistence_provider;
PlayerListener s_player_listener;

void sendspin_loop(void *)
{
    for (;;) {
        if (s_client) s_client->loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void advertise_mdns()
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("home-controller");
    mdns_instance_name_set(PLAYER_NAME);
    mdns_txt_item_t txt[] = {
        {"path", "/sendspin"},
        {"name", PLAYER_NAME},
    };
    err = mdns_service_add(PLAYER_NAME, "_sendspin", "_tcp", 8928, txt,
                           sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) ESP_LOGW(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
}

}  // namespace

extern "C" bool sendspin_player_start(void)
{
    if (s_started) return true;

    uint8_t mac[6]{};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        ESP_LOGE(TAG, "could not read device MAC");
        return false;
    }
    char client_id[40];
    snprintf(client_id, sizeof(client_id), "home-controller-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    SendspinClientConfig client_config;
    client_config.client_id = client_id;
    client_config.name = PLAYER_NAME;
    client_config.product_name = "Waveshare ESP32-P4 Music Controller";
    client_config.manufacturer = "Home Controller";
    client_config.software_version = "1.0";
    client_config.httpd_psram_stack = true;
    client_config.httpd_priority = 5;
    /* WebSocket RX must out-prioritise the LVGL render task (priority 6) so the
     * FLAC ring keeps filling while a heavy now-playing frame is drawing. */
    client_config.websocket_priority = 8;
    client_config.websocket_payload_location = MemoryLocation::PREFER_EXTERNAL;

    s_client = new (std::nothrow) SendspinClient(std::move(client_config));
    if (!s_client) {
        ESP_LOGE(TAG, "client allocation failed");
        return false;
    }

    PlayerRoleConfig player_config;
    player_config.audio_formats = {
        AudioSupportedFormatObject{SendspinCodecFormat::FLAC, 1, 48000, 16},
        AudioSupportedFormatObject{SendspinCodecFormat::FLAC, 1, 44100, 16},
        AudioSupportedFormatObject{SendspinCodecFormat::OPUS, 1, 48000, 16},
        AudioSupportedFormatObject{SendspinCodecFormat::PCM, 1, 48000, 16},
        AudioSupportedFormatObject{SendspinCodecFormat::PCM, 1, 44100, 16},
    };
    player_config.audio_buffer_capacity = AUDIO_BUFFER_BYTES;
    player_config.psram_stack = true;
    /* Decode + I2S-feed task. The ES8311 DMA holds only ~5 ms of audio, so this
     * task MUST preempt the LVGL render task (priority 6) or a single heavy
     * now-playing frame starves the feed and the audio glitches. Kept well below
     * the WiFi/hosted tasks (~18-23) so networking is never starved. */
    player_config.priority = 12;
    player_config.decode_buffer_location = MemoryLocation::PREFER_EXTERNAL;

    s_player = &s_client->add_player(std::move(player_config));
    s_player->set_listener(&s_player_listener);
    s_client->set_network_provider(&s_network_provider);
    s_client->set_persistence_provider(&s_persistence_provider);

    s_volume = std::min<uint8_t>(nvs_read_u8("volume", DEFAULT_VOLUME), 100);
    s_muted = nvs_read_u8("muted", 0) != 0;
    s_player->update_volume(s_volume);
    s_player->update_muted(s_muted);
    audio_stream_set_volume(s_volume);
    audio_stream_set_muted(s_muted);

    if (!s_client->start_server()) {
        ESP_LOGE(TAG, "Sendspin server failed to start");
        return false;
    }
    advertise_mdns();

    if (xTaskCreatePinnedToCoreWithCaps(sendspin_loop, "sendspin_loop", 8192,
                                        nullptr, 7, &s_loop_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "loop task creation failed");
        return false;
    }

    s_started = true;
    ESP_LOGI(TAG, "%s ready on _sendspin._tcp:8928 (FLAC/Opus/PCM mono)", PLAYER_NAME);
    return true;
}
