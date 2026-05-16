#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define GPIO_BACKLIGHT 21
static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller IDF boot");

    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << GPIO_BACKLIGHT),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);

    while (1) {
        gpio_set_level(GPIO_BACKLIGHT, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(GPIO_BACKLIGHT, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
