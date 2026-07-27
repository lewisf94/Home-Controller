#include "mt6701_i2c.h"

#include "pico/stdlib.h"

namespace {

constexpr uint8_t kAngleHighRegister = 0x03;
constexpr uint8_t kAngleLowRegister = 0x04;
constexpr uint32_t kTransactionTimeoutUs = 1000;

}  // namespace

Mt6701I2c::Mt6701I2c(i2c_inst_t* bus, uint sda_pin, uint scl_pin)
    : bus_(bus), sda_pin_(sda_pin), scl_pin_(scl_pin) {}

uint32_t Mt6701I2c::init(uint32_t baud_hz) {
    const uint32_t actual_baud_hz = i2c_init(bus_, baud_hz);

    gpio_set_function(sda_pin_, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin_, GPIO_FUNC_I2C);

    // These are a fallback for an unpopulated breakout. External 4.7k pull-ups
    // to 3.3 V remain the correct setup for a reliable high-speed bus.
    gpio_pull_up(sda_pin_);
    gpio_pull_up(scl_pin_);

    return actual_baud_hz;
}

bool Mt6701I2c::read_register(uint8_t address, uint8_t* value) {
    const int write_result = i2c_write_timeout_us(
        bus_,
        kDefaultAddress,
        &address,
        1,
        true,
        kTransactionTimeoutUs);
    if (write_result != 1) {
        return false;
    }

    const int read_result = i2c_read_timeout_us(
        bus_,
        kDefaultAddress,
        value,
        1,
        false,
        kTransactionTimeoutUs);
    return read_result == 1;
}

bool Mt6701I2c::read_raw_angle(uint16_t* raw_angle) {
    if (raw_angle == nullptr) {
        return false;
    }

    uint8_t high = 0;
    uint8_t low = 0;
    if (!read_register(kAngleHighRegister, &high)) {
        return false;
    }
    if (!read_register(kAngleLowRegister, &low)) {
        return false;
    }

    *raw_angle = static_cast<uint16_t>(
        (static_cast<uint16_t>(high) << 6) | (low >> 2));
    return true;
}
