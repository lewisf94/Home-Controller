#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf[2];
    int idx;
} art_buffer_t;

bool art_buffer_alloc(art_buffer_t *buffer, size_t bytes);
uint8_t *art_buffer_idle(art_buffer_t *buffer);
void art_buffer_publish(art_buffer_t *buffer, uint16_t width, uint16_t height);
