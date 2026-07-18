#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *exc_task;
    uint32_t exc_pc;
} esp_core_dump_summary_t;

esp_err_t esp_core_dump_image_check(void);
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *summary);
esp_err_t esp_core_dump_image_erase(void);
