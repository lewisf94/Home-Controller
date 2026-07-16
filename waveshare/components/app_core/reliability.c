#include "app_core_reliability.h"

#include <inttypes.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_core_dump.h"

#define TASK_SLOT_COUNT 8
#define PERIODIC_LOG_US (30LL * 1000LL * 1000LL)

typedef struct {
    const char *name;
    TaskHandle_t handle;
    size_t configured_stack_bytes;
} task_slot_t;

static const char *TAG = "reliability";
static task_slot_t s_tasks[TASK_SLOT_COUNT];
static int s_task_count;
static int64_t s_last_periodic_log_us;
static int64_t s_last_reject_log_us;

static void failed_alloc_hook(size_t size, uint32_t caps, const char *function_name)
{
    /* Do not query the heap from inside its failure callback. The allocator may
     * still own internal locks; the next periodic checkpoint supplies totals. */
    ESP_EARLY_LOGE(TAG, "ALLOC FAILED size=%u caps=0x%08lx caller=%s",
                   (unsigned)size, (unsigned long)caps,
                   function_name ? function_name : "?");
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC (exception / abort / assert)";
    case ESP_RST_INT_WDT:   return "INTERRUPT watchdog";
    case ESP_RST_TASK_WDT:  return "TASK watchdog (a task hung)";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dip)";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "unknown";
    }
}

void app_core_reliability_boot_report(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool crashy = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                   r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                   r == ESP_RST_BROWNOUT);
    if (crashy) ESP_LOGE(TAG, "PREVIOUS RESET: %s", reset_reason_str(r));
    else        ESP_LOGI(TAG, "previous reset: %s", reset_reason_str(r));

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
        if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
            ESP_LOGE(TAG, "COREDUMP: crashed in task '%s' at PC=0x%08" PRIx32,
                     sum->exc_task, sum->exc_pc);
            ESP_LOGE(TAG, "  resolve: addr2line -e build/<app>.elf 0x%08" PRIx32
                          "  (or idf.py coredump-info before the next crash)",
                     sum->exc_pc);
        } else {
            ESP_LOGE(TAG, "COREDUMP present but its summary could not be read");
        }
        free(sum);
        /* Report once: erase so the next clean boot is quiet and the slot is
         * ready for the next crash (the single slot is overwritten anyway). */
        esp_core_dump_image_erase();
    }
#endif
}

void app_core_reliability_init(void)
{
    esp_err_t err = heap_caps_register_failed_alloc_callback(failed_alloc_hook);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "failed-allocation hook unavailable: %s", esp_err_to_name(err));
    app_core_reliability_boot_report();
    app_core_reliability_checkpoint("boot");
}

void app_core_reliability_register_task(const char *name, TaskHandle_t task,
                                        size_t configured_stack_bytes)
{
    if (!task || s_task_count >= TASK_SLOT_COUNT) return;
    s_tasks[s_task_count++] = (task_slot_t) {
        .name = name,
        .handle = task,
        .configured_stack_bytes = configured_stack_bytes,
    };
}

void app_core_reliability_checkpoint(const char *tag)
{
    ESP_LOGI(TAG,
             "%s: int_free=%u int_min=%u int_largest=%u dma_free=%u dma_largest=%u psram_free=%u",
             tag ? tag : "checkpoint",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    for (int i = 0; i < s_task_count; ++i) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
        ESP_LOGI(TAG, "task=%s stack=%u hwm=%u",
                 s_tasks[i].name ? s_tasks[i].name : "?",
                 (unsigned)s_tasks[i].configured_stack_bytes, (unsigned)hwm);
    }
}

void app_core_reliability_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_periodic_log_us && now - s_last_periodic_log_us < 10000000LL)
        return;
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool low = free_internal < APP_CORE_INTERNAL_DESIRED_FREE ||
               largest_internal < APP_CORE_INTERNAL_DESIRED_LARGEST;
    if (s_last_periodic_log_us == 0 || low ||
        now - s_last_periodic_log_us >= PERIODIC_LOG_US) {
        s_last_periodic_log_us = now;
        app_core_reliability_checkpoint(low ? "LOW INTERNAL RESERVE" : "runtime");
    }
}

bool app_core_reliability_network_budget_ok(const char *operation,
                                            size_t min_free,
                                            size_t min_largest)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool ok = free_internal >= min_free && largest_internal >= min_largest;
    if (!ok) {
        int64_t now = esp_timer_get_time();
        if (s_last_reject_log_us == 0 || now - s_last_reject_log_us >= 10000000LL) {
            s_last_reject_log_us = now;
            ESP_LOGW(TAG, "%s deferred: internal reserve free=%u/%u largest=%u/%u",
                     operation ? operation : "network operation",
                     (unsigned)free_internal, (unsigned)min_free,
                     (unsigned)largest_internal, (unsigned)min_largest);
        }
    }
    return ok;
}
