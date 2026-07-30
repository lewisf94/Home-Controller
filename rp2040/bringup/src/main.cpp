#include <cinttypes>
#include <climits>
#include <cstdio>

#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "mt6701_i2c.h"

namespace {

constexpr uint kMt6701SdaPin = 4;
constexpr uint kMt6701SclPin = 5;
constexpr uint32_t kMt6701BaudHz = 100000;
constexpr uint32_t kSamplePeriodUs = 1000;
constexpr uint32_t kReportPeriodMs = 1000;
constexpr uint32_t kScanPeriodReports = 5;

void scan_i2c_bus() {
    uint32_t devices_found = 0;

    std::printf("i2c_scan:");
    for (uint8_t address = 0x01; address <= 0x7e; ++address) {
        uint8_t value = 0;
        const int result = i2c_read_timeout_us(
            i2c0, address, &value, 1, false, 1000);
        if (result >= 0) {
            std::printf(" 0x%02x", address);
            ++devices_found;
        }
    }

    if (devices_found == 0) {
        std::printf(" none");
    }
    std::printf("\r\n");
}

void set_led(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

void init_led() {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    set_led(false);
#endif
}

}  // namespace

int main() {
    stdio_init_all();
    init_led();

    sleep_ms(1500);

    const uint32_t system_clock_hz = clock_get_hz(clk_sys);
    Mt6701I2c sensor(i2c0, kMt6701SdaPin, kMt6701SclPin);
    const uint32_t i2c_baud_hz = sensor.init(kMt6701BaudHz);

    std::printf(
        "\r\nSMARTKNOB_RP2040_OK sdk=%s clock=%" PRIu32 "Hz\r\n"
        "MT6701_I2C address=0x%02x sda=GP%u scl=GP%u baud=%" PRIu32 "Hz\r\n",
        PICO_SDK_VERSION_STRING,
        system_clock_hz,
        Mt6701I2c::kDefaultAddress,
        kMt6701SdaPin,
        kMt6701SclPin,
        i2c_baud_hz);

    uint32_t reads_ok = 0;
    uint32_t read_errors = 0;
    uint64_t read_time_total_us = 0;
    uint32_t read_time_min_us = UINT32_MAX;
    uint32_t read_time_max_us = 0;
    uint16_t raw_angle = 0;
    uint32_t reports_since_scan = kScanPeriodReports;
    absolute_time_t next_sample = get_absolute_time();
    absolute_time_t next_report = make_timeout_time_ms(kReportPeriodMs);

    while (true) {
        next_sample = delayed_by_us(next_sample, kSamplePeriodUs);

        const uint64_t read_start_us = time_us_64();
        const bool read_ok = sensor.read_raw_angle(&raw_angle);
        const uint32_t read_time_us =
            static_cast<uint32_t>(time_us_64() - read_start_us);

        if (read_ok) {
            ++reads_ok;
            read_time_total_us += read_time_us;
            if (read_time_us < read_time_min_us) {
                read_time_min_us = read_time_us;
            }
            if (read_time_us > read_time_max_us) {
                read_time_max_us = read_time_us;
            }
        } else {
            ++read_errors;
        }

        if (time_reached(next_report)) {
            if (reads_ok > 0) {
                const uint32_t angle_centidegrees =
                    (static_cast<uint32_t>(raw_angle) * 36000u) / 16384u;
                const uint32_t average_read_us =
                    static_cast<uint32_t>(read_time_total_us / reads_ok);
                std::printf(
                    "mt6701=OK raw=%u angle=%" PRIu32 ".%02" PRIu32
                    "deg reads=%" PRIu32 " errors=%" PRIu32
                    " read_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\r\n",
                    raw_angle,
                    angle_centidegrees / 100u,
                    angle_centidegrees % 100u,
                    reads_ok,
                    read_errors,
                    read_time_min_us,
                    average_read_us,
                    read_time_max_us);
                set_led(true);
            } else {
                std::printf(
                    "mt6701=NOT_FOUND address=0x%02x errors=%" PRIu32
                    " sda=%u scl=%u\r\n",
                    Mt6701I2c::kDefaultAddress,
                    read_errors,
                    gpio_get(kMt6701SdaPin),
                    gpio_get(kMt6701SclPin));
                set_led(false);

                if (++reports_since_scan >= kScanPeriodReports) {
                    reports_since_scan = 0;
                    scan_i2c_bus();
                }
            }

            reads_ok = 0;
            read_errors = 0;
            read_time_total_us = 0;
            read_time_min_us = UINT32_MAX;
            read_time_max_us = 0;
            next_report = delayed_by_ms(next_report, kReportPeriodMs);
        }

        sleep_until(next_sample);
    }
}
