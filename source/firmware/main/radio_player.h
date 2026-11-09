#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

typedef enum {
    RADIO_STOPPED, RADIO_WAIT_WIFI, RADIO_WAIT_TIME, RADIO_CONNECTING,
    RADIO_BUFFERING, RADIO_PLAYING, RADIO_RETRYING,
} radio_status_t;

typedef struct {
    uint32_t sample_rate, bitrate, elapsed_seconds;
    uint8_t channels, levels[16];
} radio_telemetry_t;

esp_err_t radio_player_init(EventGroupHandle_t wifi, EventBits_t ready, SemaphoreHandle_t audio_owner);
void radio_player_select(int channel);  // -1 stops playback asynchronously.
void radio_player_volume(unsigned percent);
radio_status_t radio_player_status(void);
void radio_player_telemetry(radio_telemetry_t *result);
