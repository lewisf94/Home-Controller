#include <cctype>
#include <cinttypes>
#include <cstdio>

#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

namespace {

constexpr uint kMotorUhPin = 0;
constexpr uint kMotorUlPin = 1;
constexpr uint kMotorVhPin = 2;
constexpr uint kMotorVlPin = 3;
constexpr uint kMotorWhPin = 4;
constexpr uint kMotorWlPin = 5;
constexpr uint kDriverVioPin = 6;
constexpr uint kDriverDiagPin = 7;

constexpr uint kBridgePins[] = {
    kMotorUhPin,
    kMotorUlPin,
    kMotorVhPin,
    kMotorVlPin,
    kMotorWhPin,
    kMotorWlPin,
};

constexpr uint32_t kPwmFrequencyHz = 20000;
constexpr uint32_t kTestDutyPermille = 120;
constexpr uint32_t kArmTimeoutMs = 10000;
constexpr uint32_t kStepPeriodMs = 100;
constexpr uint32_t kTestStepCount = 24;
constexpr uint32_t kStatusPeriodMs = 1000;

struct CommutationStep {
    uint source_high_pin;
    uint sink_low_pin;
};

constexpr CommutationStep kCommutationSequence[] = {
    {kMotorUhPin, kMotorVlPin},
    {kMotorUhPin, kMotorWlPin},
    {kMotorVhPin, kMotorWlPin},
    {kMotorVhPin, kMotorUlPin},
    {kMotorWhPin, kMotorUlPin},
    {kMotorWhPin, kMotorVlPin},
};

uint16_t s_pwm_top = 0;
bool s_armed = false;
absolute_time_t s_arm_deadline;

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
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
}

void initialise_safe_gpio_latches() {
    for (uint pin : kBridgePins) {
        gpio_init(pin);
        gpio_put(pin, false);
        gpio_set_dir(pin, GPIO_OUT);
    }

    gpio_init(kDriverVioPin);
    gpio_put(kDriverVioPin, false);
    gpio_set_dir(kDriverVioPin, GPIO_OUT);

    gpio_init(kDriverDiagPin);
    gpio_set_dir(kDriverDiagPin, GPIO_IN);
    gpio_pull_down(kDriverDiagPin);
}

void initialise_bridge_pwm() {
    const uint32_t system_clock_hz = clock_get_hz(clk_sys);
    const uint32_t calculated_top =
        (system_clock_hz / kPwmFrequencyHz) - 1u;
    hard_assert(calculated_top <= UINT16_MAX);
    s_pwm_top = static_cast<uint16_t>(calculated_top);

    for (uint slice = 0; slice < 3; ++slice) {
        pwm_config config = pwm_get_default_config();
        pwm_config_set_wrap(&config, s_pwm_top);
        pwm_init(slice, &config, false);
        pwm_set_both_levels(slice, 0, 0);
    }

    for (uint pin : kBridgePins) {
        gpio_set_function(pin, GPIO_FUNC_PWM);
    }

    for (uint slice = 0; slice < 3; ++slice) {
        pwm_set_enabled(slice, true);
    }
}

void bridge_off() {
    for (uint pin : kBridgePins) {
        pwm_set_gpio_level(pin, 0);
    }
}

bool diagnostic_fault_active() {
    return gpio_get(kDriverDiagPin) != 0;
}

void disable_driver(const char* reason) {
    bridge_off();
    sleep_us(50);
    gpio_put(kDriverVioPin, false);
    s_armed = false;
    set_led(false);
    std::printf("tmc6300=DISABLED reason=%s diag=%u\r\n",
                reason,
                diagnostic_fault_active() ? 1u : 0u);
}

void print_status() {
    std::printf(
        "tmc6300=%s vio=%u diag=%u pwm=%" PRIu32
        "Hz duty=%" PRIu32 ".%" PRIu32 "%%\r\n",
        s_armed ? "ARMED" : "DISABLED",
        gpio_get(kDriverVioPin),
        diagnostic_fault_active() ? 1u : 0u,
        kPwmFrequencyHz,
        kTestDutyPermille / 10u,
        kTestDutyPermille % 10u);
}

void print_help() {
    std::printf(
        "commands: e=arm-for-10s t=run-2.4s-test x=stop s=status h=help\r\n");
}

void arm_driver() {
    bridge_off();
    gpio_put(kDriverVioPin, true);
    sleep_ms(10);

    if (diagnostic_fault_active()) {
        disable_driver("DIAG_ON_ARM");
        return;
    }

    s_armed = true;
    s_arm_deadline = make_timeout_time_ms(kArmTimeoutMs);
    set_led(true);
    std::printf(
        "tmc6300=ARMED outputs=OFF timeout=%" PRIu32 "ms diag=0\r\n",
        kArmTimeoutMs);
}

bool wait_for_step_or_abort() {
    const absolute_time_t deadline = make_timeout_time_ms(kStepPeriodMs);

    while (!time_reached(deadline)) {
        if (diagnostic_fault_active()) {
            std::printf("motor_test=ABORT reason=DIAG\r\n");
            return false;
        }

        const int input = getchar_timeout_us(1000);
        if (input == 'x' || input == 'X') {
            std::printf("motor_test=ABORT reason=USER\r\n");
            return false;
        }
    }

    return true;
}

void apply_commutation_step(const CommutationStep& step) {
    bridge_off();
    sleep_us(50);

    const uint16_t source_level = static_cast<uint16_t>(
        ((static_cast<uint32_t>(s_pwm_top) + 1u) *
         kTestDutyPermille) /
        1000u);
    pwm_set_gpio_level(step.sink_low_pin,
                       static_cast<uint16_t>(s_pwm_top + 1u));
    pwm_set_gpio_level(step.source_high_pin, source_level);
}

void run_motor_test() {
    if (!s_armed) {
        std::printf("motor_test=REJECTED reason=NOT_ARMED press=e-first\r\n");
        return;
    }
    if (diagnostic_fault_active()) {
        disable_driver("DIAG_BEFORE_TEST");
        return;
    }

    std::printf(
        "motor_test=START steps=%" PRIu32 " step_ms=%" PRIu32
        " duty=%" PRIu32 ".%" PRIu32 "%% stop=x\r\n",
        kTestStepCount,
        kStepPeriodMs,
        kTestDutyPermille / 10u,
        kTestDutyPermille % 10u);

    bool completed = true;
    for (uint32_t index = 0; index < kTestStepCount; ++index) {
        apply_commutation_step(
            kCommutationSequence[
                index %
                (sizeof(kCommutationSequence) /
                 sizeof(kCommutationSequence[0]))]);
        if (!wait_for_step_or_abort()) {
            completed = false;
            break;
        }
    }

    bridge_off();
    disable_driver(completed ? "TEST_COMPLETE" : "TEST_ABORTED");
    std::printf("motor_test=%s\r\n", completed ? "PASS" : "ABORTED");
}

}  // namespace

int main() {
    initialise_safe_gpio_latches();
    stdio_init_all();
    init_led();
    initialise_bridge_pwm();

    sleep_ms(1500);

    std::printf(
        "\r\nSMARTKNOB_MOTOR_TEST sdk=%s clock=%" PRIu32 "Hz\r\n"
        "pins UH=GP%u UL=GP%u VH=GP%u VL=GP%u WH=GP%u WL=GP%u"
        " VIO=GP%u DIAG=GP%u\r\n"
        "startup=SAFE bridge_inputs=LOW vio=LOW motor=DISABLED\r\n",
        PICO_SDK_VERSION_STRING,
        clock_get_hz(clk_sys),
        kMotorUhPin,
        kMotorUlPin,
        kMotorVhPin,
        kMotorVlPin,
        kMotorWhPin,
        kMotorWlPin,
        kDriverVioPin,
        kDriverDiagPin);
    print_help();

    absolute_time_t next_status = make_timeout_time_ms(kStatusPeriodMs);

    while (true) {
        const int input = getchar_timeout_us(10000);
        if (input != PICO_ERROR_TIMEOUT) {
            const int command =
                std::tolower(static_cast<unsigned char>(input));
            switch (command) {
                case 'e':
                    arm_driver();
                    break;
                case 't':
                    run_motor_test();
                    break;
                case 'x':
                    disable_driver("USER");
                    break;
                case 's':
                    print_status();
                    break;
                case 'h':
                    print_help();
                    break;
                case '\r':
                case '\n':
                    break;
                default:
                    std::printf("command=UNKNOWN value=0x%02x\r\n",
                                static_cast<unsigned int>(command));
                    print_help();
                    break;
            }
        }

        if (s_armed && diagnostic_fault_active()) {
            disable_driver("DIAG");
        } else if (s_armed && time_reached(s_arm_deadline)) {
            disable_driver("ARM_TIMEOUT");
        }

        if (time_reached(next_status)) {
            print_status();
            next_status = delayed_by_ms(next_status, kStatusPeriodMs);
        }
    }
}
