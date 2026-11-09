#pragma once

#include <stdint.h>
#include "launcher_state.h"

#define RADIO_UI_WIDTH 240
#define RADIO_UI_HEIGHT 320
#define RADIO_UI_BARS 16

enum { RADIO_DRAW_FULL = 1, RADIO_DRAW_HEADER = 2, RADIO_DRAW_STATS = 4, RADIO_DRAW_METER = 8 };

typedef struct {
    const launcher_t *app;
    const char *status;
    uint32_t elapsed_seconds, sample_rate, bitrate;
    uint8_t channels, levels[RADIO_UI_BARS];
    int battery, signal;
} radio_ui_t;

typedef void (*radio_ui_flush_t)(unsigned x, unsigned y, unsigned width, unsigned rows);

void radio_ui_draw(const radio_ui_t *ui, unsigned flags, uint8_t *line, radio_ui_flush_t flush);
