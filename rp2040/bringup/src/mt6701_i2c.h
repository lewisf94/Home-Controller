#pragma once

#include <cstdint>

#include "hardware/i2c.h"

class Mt6701I2c {
public:
    static constexpr uint8_t kDefaultAddress = 0x06;

    Mt6701I2c(i2c_inst_t* bus, uint sda_pin, uint scl_pin);

    uint32_t init(uint32_t baud_hz);
    bool read_raw_angle(uint16_t* raw_angle);

private:
    bool read_register(uint8_t address, uint8_t* value);

    i2c_inst_t* bus_;
    uint sda_pin_;
    uint scl_pin_;
};
