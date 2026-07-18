#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 7

typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    int clk_source;
    int glitch_ignore_cnt;
    struct {
        unsigned enable_internal_pullup : 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    int dev_addr_length;
    int device_address;
    int scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data, size_t data_len,
                              int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len,
                                      int timeout_ms);
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *bus);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device);
