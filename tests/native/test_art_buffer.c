#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

static int g_alloc_call;
static int g_fail_call;
static int g_free_calls;
static const uint8_t *g_published;
static uint16_t g_width;
static uint16_t g_height;

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    g_alloc_call++;
    if (g_alloc_call == g_fail_call) return NULL;
    return malloc(size);
}
void heap_caps_free(void *ptr)
{
    if (ptr) g_free_calls++;
    free(ptr);
}
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 0; }
esp_err_t heap_caps_register_failed_alloc_callback(
    void (*callback)(size_t, uint32_t, const char *))
{ (void)callback; return ESP_OK; }

void ui_art_refresh(const uint8_t *data, uint16_t w, uint16_t h)
{
    g_published = data;
    g_width = w;
    g_height = h;
}

#include "../../../waveshare/components/app_core/art_buffer.c"

int main(void)
{
    art_buffer_t buffer = {0};
    assert(art_buffer_alloc(&buffer, 32));
    assert(buffer.buf[0] != NULL && buffer.buf[1] != NULL);
    assert(buffer.idx == 0);
    assert(art_buffer_idle(&buffer) == buffer.buf[1]);

    uint8_t *first_idle = art_buffer_idle(&buffer);
    art_buffer_publish(&buffer, 4, 4);
    assert(g_published == first_idle);
    assert(g_width == 4 && g_height == 4);
    assert(buffer.idx == 1);
    assert(art_buffer_idle(&buffer) == buffer.buf[0]);

    uint8_t *second_idle = art_buffer_idle(&buffer);
    art_buffer_publish(&buffer, 2, 3);
    assert(g_published == second_idle);
    assert(buffer.idx == 0);

    heap_caps_free(buffer.buf[0]);
    heap_caps_free(buffer.buf[1]);

    g_alloc_call = 0;
    g_fail_call = 2;
    g_free_calls = 0;
    buffer = (art_buffer_t){0};
    assert(!art_buffer_alloc(&buffer, 32));
    assert(buffer.buf[0] == NULL && buffer.buf[1] == NULL);
    assert(g_free_calls == 1);

    return 0;
}
