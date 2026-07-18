#include <assert.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

static int64_t g_now_us;

int64_t esp_timer_get_time(void) { return g_now_us; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data, size_t data_len,
                              int timeout_ms)
{
    (void)device; (void)data; (void)data_len; (void)timeout_ms;
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len,
                                      int timeout_ms)
{
    (void)device; (void)tx; (void)tx_len; (void)timeout_ms;
    if (rx_len) rx[0] = 0xFF;
    return ESP_OK;
}
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *bus)
{
    (void)config; *bus = (void *)1; return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device)
{
    (void)bus; (void)config; *device = (void *)2; return ESP_OK;
}

#include "../../../cyd/components/cyd_shared/mcp_input.c"

static uint8_t porta_for_ab(uint8_t ab)
{
    return (uint8_t)((((ab >> 1) & 1U) << 4) | ((ab & 1U) << 5));
}

static void step_encoder(enc_state_t *enc, uint8_t ab, uint32_t ms)
{
    g_now_us = (int64_t)ms * 1000;
    _update_encoder(enc, porta_for_ab(ab), 4, 5);
}

int main(void)
{
    enc_state_t enc = {S_REST, 0, 0};
    step_encoder(&enc, 1, 1);
    step_encoder(&enc, 0, 2);
    step_encoder(&enc, 2, 3);
    step_encoder(&enc, 3, 4);
    assert(enc.state == S_REST);
    assert(enc.count == 1);

    step_encoder(&enc, 2, 5);
    step_encoder(&enc, 0, 6);
    step_encoder(&enc, 1, 7);
    step_encoder(&enc, 3, 8);
    assert(enc.count == 0);

    enc = (enc_state_t){S_REST, 0, 0};
    step_encoder(&enc, 1, 10);
    step_encoder(&enc, 0, 11);
    step_encoder(&enc, 1, 12);
    step_encoder(&enc, 3, 13);
    assert(enc.count == 0);

    btn_state_t btn = {0};
    g_now_us = 1000;
    _update_btn(&btn, true);
    g_now_us = 31000;
    _update_btn(&btn, true);
    assert(!btn.pressed);
    g_now_us = 32000;
    _update_btn(&btn, true);
    assert(btn.pressed);
    assert(btn.event_pending);
    g_now_us = 100000;
    _update_btn(&btn, false);
    g_now_us = 132000;
    _update_btn(&btn, false);
    assert(!btn.pressed);
    assert(btn.event_pending);

    s_btns[0] = btn;
    assert(mcp_input_has_pending());
    assert(btn_get_event(0));
    assert(!btn_get_event(0));
    assert(!btn_get_event(4));

    s_re1.count = 7;
    assert(re1_get_delta() == 7);
    assert(re1_get_delta() == 0);

    return 0;
}
