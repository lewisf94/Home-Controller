#include "app_core_art.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ui.h"

static const char *TAG = "app_core_art";

bool art_buffer_alloc(art_buffer_t *ab, size_t bytes)
{
    ab->buf[0] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    ab->buf[1] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    ab->idx    = 0;
    if (!ab->buf[0] || !ab->buf[1]) {
        ESP_LOGE(TAG, "failed to allocate %u-byte art buffers", (unsigned)bytes);
        return false;
    }
    return true;
}

uint8_t *art_buffer_idle(art_buffer_t *ab)
{
    return ab->buf[ab->idx ^ 1];
}

void art_buffer_publish(art_buffer_t *ab, uint16_t w, uint16_t h)
{
    int next = ab->idx ^ 1;
    ui_art_refresh(ab->buf[next], w, h);
    ab->idx = next;
}
