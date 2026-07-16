#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal SRAM is the transport reserve. PSRAM cannot satisfy ESP-Hosted's
 * DMA-capable SDIO packet allocations, even when tens of MB remain free. */
#define APP_CORE_INTERNAL_DESIRED_FREE       (48U * 1024U)
#define APP_CORE_INTERNAL_DESIRED_LARGEST    (24U * 1024U)
#define APP_CORE_INTERNAL_NETWORK_MIN_FREE   (40U * 1024U)
#define APP_CORE_INTERNAL_NETWORK_MIN_LARGEST (16U * 1024U)

void app_core_reliability_init(void);
/* Log the previous reset reason and, if a crash core dump is stored in flash,
 * the crashing task + PC (then erase it). Called automatically by
 * app_core_reliability_init(); safe to call again. */
void app_core_reliability_boot_report(void);
void app_core_reliability_register_task(const char *name, TaskHandle_t task,
                                        size_t configured_stack_bytes);
void app_core_reliability_checkpoint(const char *tag);
void app_core_reliability_tick(void);

/* Gate nonessential bursty work. Essential playback/control traffic must not
 * use this as a reason to disappear; installation-wide snapshots and cover
 * downloads should. Rejections are rate-limited in the log. */
bool app_core_reliability_network_budget_ok(const char *operation,
                                            size_t min_free,
                                            size_t min_largest);

#ifdef __cplusplus
}
#endif
