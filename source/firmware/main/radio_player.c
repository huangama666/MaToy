#include "radio_player.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "bsp_audio.h"
#include "esp_audio_simple_dec.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "launcher_state.h"
#include "radio_pcm.h"

#define RADIO_INPUT_BYTES 8192
#define RADIO_OUTPUT_BYTES 4608

static const char *TAG = "pocket_radio";
static EventGroupHandle_t wifi_events;
static EventBits_t wifi_ready;
static SemaphoreHandle_t audio_lock;
static atomic_int requested = -1;
static atomic_uint volume = 45;
static atomic_int status = RADIO_STOPPED;
static portMUX_TYPE telemetry_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_telemetry_t telemetry;
static uint64_t elapsed_us;
static int last_channel = -1;

static esp_err_t radio_header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "icy-metaint")) {
        bool *has_metadata = event->user_data;
        *has_metadata = strtoul(event->header_value, NULL, 10) != 0;
    }
    return ESP_OK;
}

void radio_player_select(int channel)
{
    if (channel < -1 || channel >= RADIO_CHANNEL_COUNT) return;
    taskENTER_CRITICAL(&telemetry_lock);
    atomic_store(&requested, channel);
    if (channel >= 0 && channel != last_channel) {
        memset(&telemetry, 0, sizeof(telemetry));
        elapsed_us = 0;
        last_channel = channel;
    }
    taskEXIT_CRITICAL(&telemetry_lock);
}

void radio_player_volume(unsigned percent)
{
    atomic_store(&volume, percent > 90 ? 90 : percent);
}

radio_status_t radio_player_status(void) { return atomic_load(&status); }

void radio_player_telemetry(radio_telemetry_t *result)
{
    taskENTER_CRITICAL(&telemetry_lock);
    *result = telemetry;
    taskEXIT_CRITICAL(&telemetry_lock);
}

static void publish_pcm(int channel, const esp_audio_simple_dec_info_t *info, const int16_t *pcm, size_t bytes)
{
    const size_t count = bytes / sizeof(*pcm);
    uint8_t levels[16] = {0};
    for (unsigned bar = 0; bar < 16; ++bar) {
        unsigned peak = 0;
        for (size_t i = bar * count / 16; i < (bar + 1) * count / 16; ++i) {
            const int32_t sample = pcm[i];
            const unsigned absolute = sample < 0 ? -sample : sample;
            if (absolute > peak) peak = absolute;
        }
        const unsigned normalized = peak * 255 / 12000;
        levels[bar] = normalized > 255 ? 255 : normalized;
    }
    taskENTER_CRITICAL(&telemetry_lock);
    if (atomic_load(&requested) == channel) {
        telemetry.sample_rate = info->sample_rate;
        telemetry.channels = info->channel;
        // The MP3 implementation in codec 2.4.1 reports kbps, despite the API's bps label.
        telemetry.bitrate = info->bitrate && info->bitrate < 1000 ? info->bitrate * 1000 : info->bitrate;
        elapsed_us += count * 1000000ULL / info->sample_rate;
        telemetry.elapsed_seconds = elapsed_us / 1000000;
        for (unsigned bar = 0; bar < 16; ++bar) {
            const unsigned decay = telemetry.levels[bar] * 3 / 4;
            telemetry.levels[bar] = levels[bar] > decay ? levels[bar] : decay;
        }
    }
    taskEXIT_CRITICAL(&telemetry_lock);
}

static bool active(int channel)
{
    return atomic_load(&requested) == channel &&
           (xEventGroupGetBits(wifi_events) & wifi_ready);
}

static void log_memory(const char *phase)
{
    ESP_LOGI(TAG, "%s heap=%u largest=%u stack=%u", phase,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static bool play(int channel)
{
    bool played = false, audio_awake = false;
    uint8_t *input = NULL;
    int16_t *pcm = NULL;
    bool has_metadata = false;
    char url[112];
    snprintf(url, sizeof(url), "https://lhttp-hw.qtfm.cn/live/%lu/64k.mp3",
             (unsigned long)radio_stations[channel].id);
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 4000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .max_redirection_count = 4,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "AI-Passport-Radio/1.0",
        .event_handler = radio_header,
        .user_data = &has_metadata,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_config);
    if (!http) return false;
    atomic_store(&status, RADIO_CONNECTING);
    log_memory("connect");
    // The server sends audio only. ESP HTTP handles HTTP chunk framing and redirects.
    esp_http_client_set_header(http, "Icy-MetaData", "0");
    bool opened = false;
    for (unsigned redirects = 0; redirects <= 4 && active(channel); ++redirects) {
        if (esp_http_client_open(http, 0) != ESP_OK) goto cleanup;
        if (esp_http_client_fetch_headers(http) < 0) goto cleanup;
        const int code = esp_http_client_get_status_code(http);
        if (code == 200) { opened = true; break; }
        ESP_LOGI(TAG, "HTTP status %d", code);
        if (code != 301 && code != 302 && code != 307 && code != 308) goto cleanup;
        if (esp_http_client_set_redirection(http) != ESP_OK) goto cleanup;
        esp_http_client_close(http);
    }
    if (!opened || !active(channel)) goto cleanup;

    // Stop if a server ignores the request and inserts ICY metadata into MP3 bytes.
    if (has_metadata) goto cleanup;

    input = malloc(RADIO_INPUT_BYTES);
    pcm = malloc(RADIO_OUTPUT_BYTES);
    esp_audio_simple_dec_cfg_t decode_config = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3};
    if (!input || !pcm ||
        esp_audio_simple_dec_open(&decode_config, &decoder) != ESP_AUDIO_ERR_OK) goto cleanup;
    if (bsp_audio_init() != ESP_OK || bsp_audio_wake() != ESP_OK) goto cleanup;
    audio_awake = true;
    atomic_store(&status, RADIO_BUFFERING);
    unsigned rate = 0, previous_volume = 101;
    TickType_t last_pcm = xTaskGetTickCount();
    while (active(channel)) {
        int buffered = 0;
        while (buffered < RADIO_INPUT_BYTES && active(channel)) {
            const int n = esp_http_client_read(http, (char *)input + buffered, RADIO_INPUT_BYTES - buffered);
            if (n <= 0) goto cleanup;
            buffered += n;
        }
        esp_audio_simple_dec_raw_t raw = {.buffer = input, .len = buffered};
        unsigned no_progress = 0;
        while (raw.len && active(channel)) {
            esp_audio_simple_dec_out_t output = {.buffer = (uint8_t *)pcm, .len = RADIO_OUTPUT_BYTES};
            const esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &output);
            if (result != ESP_AUDIO_ERR_OK && result != ESP_AUDIO_ERR_CONTINUE) {
                ESP_LOGW(TAG, "MP3 decoder returned %d", result);
                goto cleanup;
            }
            if (raw.consumed > raw.len || output.decoded_size > RADIO_OUTPUT_BYTES) goto cleanup;
            if (!raw.consumed && !output.decoded_size) goto cleanup;
            if (!raw.consumed && ++no_progress > 32) goto cleanup;
            if (raw.consumed) no_progress = 0;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (!output.decoded_size) {
                if (xTaskGetTickCount() - last_pcm > pdMS_TO_TICKS(10000)) goto cleanup;
                continue;
            }
            esp_audio_simple_dec_info_t info = {0};
            if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                info.bits_per_sample != 16 || info.sample_rate < 8000 || info.sample_rate > 48000) goto cleanup;
            const size_t mono_bytes = radio_pcm_mono(pcm, output.decoded_size, info.channel);
            if (!mono_bytes) goto cleanup;
            if (rate != info.sample_rate) {
                if (bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) goto cleanup;
                rate = info.sample_rate;
            }
            const unsigned level = atomic_load(&volume);
            if (level != previous_volume) { bsp_audio_set_volume(level); previous_volume = level; }
            if (!active(channel) || bsp_audio_write(pcm, mono_bytes) != ESP_OK) goto cleanup;
            publish_pcm(channel, &info, pcm, mono_bytes);
            last_pcm = xTaskGetTickCount();
            atomic_store(&status, RADIO_PLAYING);
            if (!played) {
                played = true;
                ESP_LOGI(TAG, "Playing channel %d at %u Hz", channel, rate);
                log_memory("playing");
            }
        }
    }
cleanup:
    taskENTER_CRITICAL(&telemetry_lock);
    if (last_channel == channel) memset(telemetry.levels, 0, sizeof(telemetry.levels));
    taskEXIT_CRITICAL(&telemetry_lock);
    if (audio_awake) (void)bsp_audio_sleep();
    if (decoder) esp_audio_simple_dec_close(decoder);
    free(pcm);
    free(input);
    esp_http_client_cleanup(http);
    log_memory("stopped");
    return played;
}

static void radio_task(void *unused)
{
    (void)unused;
    for (;;) {
        const int channel = atomic_load(&requested);
        if (channel < 0) {
            atomic_store(&status, RADIO_STOPPED);
        } else if (!(xEventGroupGetBits(wifi_events) & wifi_ready)) {
            atomic_store(&status, RADIO_WAIT_WIFI);
        } else if (time(NULL) < 1704067200) {
            // Wait for SNTP before validating the radio server's TLS certificate.
            atomic_store(&status, RADIO_WAIT_TIME);
        } else if (xSemaphoreTake(audio_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (active(channel)) {
                (void)play(channel);
                if (active(channel)) {
                    ESP_LOGW(TAG, "Stream interrupted, retrying");
                    atomic_store(&status, RADIO_RETRYING);
                }
            }
            xSemaphoreGive(audio_lock);
            // Cancellable backoff keeps a failed station from repeatedly opening TLS.
            for (unsigned i = 0; i < 25 && active(channel); ++i) vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t radio_player_init(EventGroupHandle_t wifi, EventBits_t ready, SemaphoreHandle_t audio_owner)
{
    wifi_events = wifi;
    wifi_ready = ready;
    audio_lock = audio_owner;
    if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    return xTaskCreate(radio_task, "radio_player", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
