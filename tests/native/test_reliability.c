#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/task.h"

static size_t g_free_internal;
static size_t g_largest_internal;
static int64_t g_now_us;
static esp_reset_reason_t g_reset_reason = ESP_RST_POWERON;

int64_t esp_timer_get_time(void) { return g_now_us; }
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)size; (void)caps; return NULL; }
void heap_caps_free(void *ptr) { (void)ptr; }
size_t heap_caps_get_free_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? g_free_internal : 1024U * 1024U;
}
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? g_largest_internal : 512U * 1024U;
}
esp_err_t heap_caps_register_failed_alloc_callback(
    void (*callback)(size_t, uint32_t, const char *))
{ (void)callback; return ESP_OK; }
esp_reset_reason_t esp_reset_reason(void) { return g_reset_reason; }
esp_err_t esp_core_dump_image_check(void) { return ESP_FAIL; }
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *summary)
{ (void)summary; return ESP_FAIL; }
esp_err_t esp_core_dump_image_erase(void) { return ESP_OK; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 100; }

#include "../../../waveshare/components/app_core/reliability.c"

int main(void)
{
    assert(strcmp(reset_reason_str(ESP_RST_POWERON), "power-on") == 0);
    assert(strcmp(reset_reason_str(ESP_RST_TASK_WDT), "TASK watchdog (a task hung)") == 0);
    assert(strcmp(reset_reason_str(ESP_RST_BROWNOUT), "BROWNOUT (supply voltage dip)") == 0);
    assert(strcmp(reset_reason_str((esp_reset_reason_t)999), "unknown") == 0);

    g_free_internal = 40U * 1024U;
    g_largest_internal = 16U * 1024U;
    assert(app_core_reliability_network_budget_ok(
        "inventory", 40U * 1024U, 16U * 1024U));
    g_largest_internal--;
    assert(!app_core_reliability_network_budget_ok(
        "inventory", 40U * 1024U, 16U * 1024U));

    for (int i = 0; i < 12; i++)
        app_core_reliability_register_task("task", (TaskHandle_t)(uintptr_t)(i + 1), 4096);
    assert(s_task_count == TASK_SLOT_COUNT);

    g_free_internal = APP_CORE_INTERNAL_DESIRED_FREE;
    g_largest_internal = APP_CORE_INTERNAL_DESIRED_LARGEST;
    g_now_us = 1000;
    app_core_reliability_tick();
    assert(s_last_periodic_log_us == g_now_us);
    g_now_us += 1000000;
    app_core_reliability_tick();
    assert(s_last_periodic_log_us == 1000);
    g_free_internal = 1;
    g_now_us += 10000000;
    app_core_reliability_tick();
    assert(s_last_periodic_log_us == g_now_us);

    return 0;
}
