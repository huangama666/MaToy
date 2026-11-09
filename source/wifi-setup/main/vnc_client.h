#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { int fd; uint16_t width, height; } vnc_client_t;
typedef bool (*vnc_row_callback_t)(void *ctx, unsigned x, unsigned y, unsigned width, const uint8_t *pixels);

bool vnc_handshake(vnc_client_t *vnc, const char *password);
bool vnc_request(const vnc_client_t *vnc, bool incremental);
typedef void (*vnc_band_callback_t)(void *ctx, unsigned x, unsigned y, unsigned w, unsigned h);

bool vnc_key(const vnc_client_t *vnc, uint32_t keysym, bool down);
bool vnc_pointer(const vnc_client_t *vnc, uint16_t x, uint16_t y, uint8_t mask);
int vnc_receive(const vnc_client_t *vnc, vnc_row_callback_t row, vnc_band_callback_t band, void *ctx, uint8_t *scratch, uint32_t *encoding);
