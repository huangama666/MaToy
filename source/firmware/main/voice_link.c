#include "voice_link.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

enum { VOICE_PORT = 45903, VOICE_RATE = 16000, BLOCK_MS = 20, PCM_SIZE = 640, HEADER_SIZE = 5, TAG_SIZE = 16 };
static const char *TAG = "voice_link";
static char mac_host[16];
static uint8_t pairing_key[32];
static EventGroupHandle_t wifi_status;
static EventBits_t wifi_ready;
static SemaphoreHandle_t audio_lock;
static atomic_bool pointer_mode, push_to_talk, connected;
static atomic_bool speaker_enabled = true;
static atomic_bool enabled;
static atomic_uint target_port = VOICE_PORT;

void voice_link_monitor_target(bool monitoring)
{
    atomic_store(&target_port, monitoring ? 45905 : VOICE_PORT);
}

void voice_link_enable(bool active)
{
    atomic_store(&enabled, active);
    if (!active) voice_pointer_mode(false);
}

void voice_pointer_mode(bool enabled)
{
    atomic_store(&pointer_mode, enabled);
    if (!enabled) atomic_store(&push_to_talk, false);
}

void voice_push_to_talk(bool pressed)
{
    atomic_store(&push_to_talk, pressed && atomic_load(&pointer_mode) && atomic_load(&connected));
}

void voice_speaker_mode(bool enabled)
{
    atomic_store(&speaker_enabled, enabled);
}

bool voice_connected(void) { return atomic_load(&connected); }
bool voice_talking(void) { return atomic_load(&connected) && atomic_load(&push_to_talk); }

static bool transfer(int fd, uint8_t *buffer, size_t size, bool sending)
{
    while (size) {
        int count = sending ? send(fd, buffer, size, 0) : recv(fd, buffer, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        buffer += count;
        size -= count;
    }
    return true;
}

static bool hmac(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t output[32])
{
    const mbedtls_md_info_t *sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return sha && mbedtls_md_hmac(sha, key, 32, data, size, output) == 0;
}

static bool begin(int fd, mbedtls_gcm_context *to_mac, mbedtls_gcm_context *from_mac)
{
    uint8_t transcript[37] = {'A', 'P', 'A', 'U', '1'};
    uint8_t session[32], proof[32], expected[32], directional[32];
    esp_fill_random(transcript + 5, 16);
    bool ok = transfer(fd, transcript, 21, true) &&
        transfer(fd, transcript + 21, 16, false) &&
        hmac(pairing_key, transcript, sizeof(transcript), session) &&
        transfer(fd, proof, sizeof(proof), false) &&
        hmac(session, (const uint8_t *)"server", 6, expected) &&
        mbedtls_ct_memcmp(proof, expected, sizeof(proof)) == 0 &&
        hmac(session, (const uint8_t *)"client", 6, proof) &&
        hmac(session, (const uint8_t *)"board", 5, directional) &&
        mbedtls_gcm_setkey(to_mac, MBEDTLS_CIPHER_ID_AES, directional, 256) == 0 &&
        hmac(session, (const uint8_t *)"mac", 3, directional) &&
        mbedtls_gcm_setkey(from_mac, MBEDTLS_CIPHER_ID_AES, directional, 256) == 0 &&
        transfer(fd, proof, sizeof(proof), true);
    mbedtls_platform_zeroize(session, sizeof(session));
    mbedtls_platform_zeroize(directional, sizeof(directional));
    return ok;
}

static bool exchange(int fd, mbedtls_gcm_context *to_mac, mbedtls_gcm_context *from_mac,
                     uint32_t sequence, uint8_t pcm[PCM_SIZE], bool talking)
{
    uint8_t record[HEADER_SIZE + PCM_SIZE + TAG_SIZE];
    uint8_t nonce[12] = {0};
    nonce[8] = sequence >> 24;
    nonce[9] = sequence >> 16;
    nonce[10] = sequence >> 8;
    nonce[11] = sequence;
    memcpy(record, nonce + 8, 4);
    record[4] = talking ? 1 : 0;
    return mbedtls_gcm_crypt_and_tag(to_mac, MBEDTLS_GCM_ENCRYPT, PCM_SIZE, nonce, sizeof(nonce),
            record, HEADER_SIZE, pcm, record + HEADER_SIZE, TAG_SIZE, record + HEADER_SIZE + PCM_SIZE) == 0 &&
        transfer(fd, record, sizeof(record), true) && transfer(fd, record, sizeof(record), false) &&
        memcmp(record, nonce + 8, 4) == 0 && record[4] == 0 &&
        mbedtls_gcm_auth_decrypt(from_mac, PCM_SIZE, nonce, sizeof(nonce), record, HEADER_SIZE,
            record + HEADER_SIZE + PCM_SIZE, TAG_SIZE, record + HEADER_SIZE, pcm) == 0;
}

static void session(int fd, unsigned port)
{
    if (xSemaphoreTake(audio_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (!atomic_load(&enabled)) {
        xSemaphoreGive(audio_lock);
        return;
    }
    mbedtls_gcm_context to_mac, from_mac;
    mbedtls_gcm_init(&to_mac);
    mbedtls_gcm_init(&from_mac);
    if (begin(fd, &to_mac, &from_mac) && atomic_load(&enabled) && bsp_audio_init() == ESP_OK &&
        bsp_audio_wake() == ESP_OK && bsp_audio_set_format(VOICE_RATE, 16, 1) == ESP_OK) {
        bsp_audio_set_volume(52);
        uint8_t pcm[PCM_SIZE] __attribute__((aligned(4)));
        TickType_t next = xTaskGetTickCount();
        unsigned held_blocks = 0;
        for (uint32_t sequence = 1; sequence != UINT32_MAX &&
             atomic_load(&enabled) && atomic_load(&target_port) == port &&
             (xEventGroupGetBits(wifi_status) & wifi_ready); ++sequence) {
            bool talking = voice_talking();
            held_blocks = talking ? held_blocks + (held_blocks < 8) : 0;
            if (talking) {
                if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) break;
            }
            talking = talking && voice_talking();
            if (!talking || held_blocks < 8) memset(pcm, 0, sizeof(pcm));
            if (!exchange(fd, &to_mac, &from_mac, sequence, pcm, talking)) break;
            if (sequence == 1) {
                atomic_store(&connected, true);
                ESP_LOGI(TAG, "Authenticated audio connected");
            }
            if (talking || voice_talking() || !atomic_load(&speaker_enabled)) memset(pcm, 0, sizeof(pcm));
            if (bsp_audio_write(pcm, sizeof(pcm)) != ESP_OK) break;
            vTaskDelayUntil(&next, pdMS_TO_TICKS(BLOCK_MS));
            if ((TickType_t)(xTaskGetTickCount() - next) > pdMS_TO_TICKS(2 * BLOCK_MS)) {
                next = xTaskGetTickCount();
            }
        }
        atomic_store(&connected, false);
        atomic_store(&push_to_talk, false);
        (void)bsp_audio_sleep();
    }
    mbedtls_gcm_free(&to_mac);
    mbedtls_gcm_free(&from_mac);
    xSemaphoreGive(audio_lock);
}

static void voice_task(void *unused)
{
    (void)unused;
    struct sockaddr_in target = {.sin_family = AF_INET};
    inet_pton(AF_INET, mac_host, &target.sin_addr);
    for (;;) {
        if (!atomic_load(&enabled) || !(xEventGroupGetBits(wifi_status) & wifi_ready)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd >= 0) {
            const unsigned port = atomic_load(&target_port);
            target.sin_port = htons(port);
            const struct timeval timeout = {.tv_sec = 2};
            int enabled = 1;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
            if (connect(fd, (struct sockaddr *)&target, sizeof(target)) == 0) session(fd, port);
            close(fd);
        }
        vTaskDelay(pdMS_TO_TICKS(1100));
    }
}

void voice_link_start(const char *host, const uint8_t key[32], EventGroupHandle_t wifi,
                      EventBits_t ready, SemaphoreHandle_t audio_owner)
{
    memcpy(mac_host, host, sizeof(mac_host));
    memcpy(pairing_key, key, sizeof(pairing_key));
    wifi_status = wifi;
    wifi_ready = ready;
    audio_lock = audio_owner;
    if (xTaskCreate(voice_task, "voice_link", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Audio task could not start");
    }
}
