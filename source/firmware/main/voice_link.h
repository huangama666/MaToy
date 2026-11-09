#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

void voice_link_start(const char *host, const uint8_t key[32], EventGroupHandle_t wifi,
                      EventBits_t ready, SemaphoreHandle_t audio_owner);
void voice_link_enable(bool enabled);
void voice_link_monitor_target(bool monitoring);
void voice_pointer_mode(bool enabled);
void voice_speaker_mode(bool enabled);
void voice_push_to_talk(bool pressed);
bool voice_connected(void);
bool voice_talking(void);
