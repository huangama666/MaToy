#include <errno.h>
#include <string.h>
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_attr.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "vnc_client.h"
#include "vnc_geometry.h"

#define CONFIG_PREFIX "APVNC "
#define WIFI_UP BIT0
#define WIFI_FAILED BIT1
#define KEYBOARD_HEIGHT 44
#define DESKTOP_HEIGHT (BSP_LCD_H - KEYBOARD_HEIGHT)
static const char *TAG = "mac_vnc";

typedef struct { char ssid[33], wifi[65], host[16], vnc[9]; } connection_t;
typedef struct { bsp_btn_t key; bsp_btn_ev_t event; } input_t;
typedef struct { viewport_t view; unsigned source_w, source_h; bool painted; } render_t;

static EventGroupHandle_t wifi_status;
static SemaphoreHandle_t drawn;
static QueueHandle_t keys;
DMA_ATTR static uint8_t display_row[BSP_LCD_W * 2];
static uint8_t network_row[8192 * 2];
static uint8_t render_band[16][BSP_LCD_W * 2];

static bool load_connection(connection_t *cfg)
{
    nvs_handle_t nvs;
    if (nvs_open("mac_vnc", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t length = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(nvs, "connection", cfg, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(*cfg);
}

static bool save_connection(const connection_t *cfg)
{
    nvs_handle_t nvs;
    if (nvs_open("mac_vnc", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, "connection", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool json_field(cJSON *json, const char *key, char *value, size_t capacity, bool required)
{
    const char *field = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
    if (!field || (required && !*field) || strlen(field) >= capacity) return false;
    strcpy(value, field);
    return true;
}

static void usb_setup(void *unused)
{
    (void)unused;
    char line[768];
    unsigned length = 0;
    bool oversized = false;
    for (;;) {
        char byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(1000)) != 1) continue;
        if (byte != '\n') {
            if (length < sizeof(line) - 1) line[length++] = byte;
            else oversized = true;
            continue;
        }
        line[length] = '\0';
        const bool candidate = !oversized && strncmp(line, CONFIG_PREFIX, sizeof(CONFIG_PREFIX) - 1) == 0;
        length = 0;
        oversized = false;
        if (!candidate) continue;
        connection_t cfg = {0};
        struct in_addr address;
        cJSON *json = cJSON_Parse(line + sizeof(CONFIG_PREFIX) - 1);
        const bool ok = json && json_field(json, "ssid", cfg.ssid, sizeof(cfg.ssid), true) &&
            json_field(json, "wifi", cfg.wifi, sizeof(cfg.wifi), false) &&
            json_field(json, "host", cfg.host, sizeof(cfg.host), true) &&
            json_field(json, "vnc", cfg.vnc, sizeof(cfg.vnc), true) &&
            inet_pton(AF_INET, cfg.host, &address) == 1 && save_connection(&cfg);
        cJSON_Delete(json);
        memset(line, 0, sizeof(line));
        memset(&cfg, 0, sizeof(cfg));
        const char *reply = ok ? "APVNC CONFIG OK\n" : "APVNC CONFIG ERROR\n";
        usb_serial_jtag_write_bytes(reply, strlen(reply), pdMS_TO_TICKS(500));
        if (ok) {
            usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
}

static void wifi_event(void *unused, esp_event_base_t base, int32_t event, void *data)
{
    (void)unused;
    if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected and IPv4 assigned");
        xEventGroupClearBits(wifi_status, WIFI_FAILED);
        xEventGroupSetBits(wifi_status, WIFI_UP);
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *info = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %u", info->reason);
        xEventGroupClearBits(wifi_status, WIFI_UP);
        xEventGroupSetBits(wifi_status, WIFI_FAILED);
    }
}

static void wifi_start(const connection_t *cfg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t station = {.sta = {
        .scan_method = WIFI_ALL_CHANNEL_SCAN,
        .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        .failure_retry_cnt = 1,
    }};
    memcpy(station.sta.ssid, cfg->ssid, strlen(cfg->ssid));
    memcpy(station.sta.password, cfg->wifi, strlen(cfg->wifi));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(context, &woken);
    return woken == pdTRUE;
}

static void write_row(unsigned x, unsigned y, unsigned width)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(bsp_display_panel(), x, y, x + width, y + 1, display_row));
    xSemaphoreTake(drawn, portMAX_DELAY);
}

static void status_screen(uint16_t color)
{
    for (unsigned i = 0; i < BSP_LCD_W; ++i) {
        display_row[2 * i] = color >> 8;
        display_row[2 * i + 1] = color;
    }
    for (unsigned y = 0; y < BSP_LCD_H; ++y) write_row(0, y, BSP_LCD_W);
}

static bool render_row(void *context, unsigned x, unsigned y, unsigned width, const uint8_t *pixels)
{
    const render_t *r = context;
    const viewport_t v = r->view;
    const unsigned y0 = vnc_edge(y, v.h, r->source_h), y1 = vnc_edge(y + 1, v.h, r->source_h);
    if (y0 == y1) return true;
    const unsigned x0 = vnc_edge(x, v.w, r->source_w), x1 = vnc_edge(x + width, v.w, r->source_w);
    uint8_t *dest = render_band[y0 % 16];
    for (unsigned dx = x0; dx < x1; ++dx) {
        const unsigned src = dx * r->source_w / v.w - x;
        dest[2 * dx] = pixels[2 * src + 1];
        dest[2 * dx + 1] = pixels[2 * src];
    }
    return true;
}

static void flush_band(void *context, unsigned x, unsigned y, unsigned width, unsigned height)
{
    render_t *r = context;
    const viewport_t v = r->view;
    const unsigned x0 = vnc_edge(x, v.w, r->source_w), x1 = vnc_edge(x + width, v.w, r->source_w);
    if (x0 == x1) return;
    const unsigned y0 = vnc_edge(y, v.h, r->source_h), y1 = vnc_edge(y + height, v.h, r->source_h);
    for (unsigned dy = y0; dy < y1; ++dy) {
        memcpy(display_row, render_band[dy % 16] + x0 * 2, (x1 - x0) * 2);
        write_row(v.x + x0, v.y + dy, x1 - x0);
        r->painted = true;
    }
}

static const char *const key_groups[] = {
    "0123456789", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "!@#$%^&*()-_=+[]{}", "\\|;:'\",.<>/?`~", " \b\t\r",
};
static const char *const group_names[] = {"123", "abc", "ABC", "Symbols 1", "Symbols 2", "Special"};

static void panel_text(const char *text, unsigned x, int row, int selected)
{
    if (row < 0 || row >= 9) return;
    const lv_font_fmt_txt_dsc_t *font = lv_font_unscii_8.dsc;
    for (unsigned index = 0; text[index] && x + 8 <= BSP_LCD_W; ++index, x += 8) {
        const unsigned c = (unsigned char)text[index];
        if (c < 32 || c > 126) continue;
        const lv_font_fmt_txt_glyph_dsc_t *glyph = &font->glyph_dsc[c - 31];
        const int glyph_row = row - (9 - glyph->ofs_y - glyph->box_h);
        for (unsigned column = 0; column < 8; ++column) {
            const int gx = (int)column - glyph->ofs_x;
            bool ink = false;
            if (gx >= 0 && gx < glyph->box_w && glyph_row >= 0 && glyph_row < glyph->box_h) {
                const unsigned bit = glyph_row * glyph->box_w + gx;
                ink = (font->glyph_bitmap[glyph->bitmap_index + bit / 8] & (0x80 >> (bit % 8))) != 0;
            }
            const uint16_t color = (int)index == selected ? (ink ? 0x0000 : 0x97f2) : (ink ? 0xffff : 0x0865);
            display_row[2 * (x + column)] = color >> 8;
            display_row[2 * (x + column) + 1] = color;
        }
    }
}

static void input_panel(bool keyboard, bool vertical, unsigned group, unsigned selected)
{
    const unsigned groups = sizeof(key_groups) / sizeof(key_groups[0]);
    for (unsigned y = 0; y < KEYBOARD_HEIGHT; ++y) {
        for (unsigned x = 0; x < BSP_LCD_W; ++x) {
            display_row[2 * x] = 0x08;
            display_row[2 * x + 1] = 0x65;
        }
        if (keyboard) {
            panel_text(group_names[group], 8, (int)y - 4, -1);
            panel_text("HOLD OK: DEL", 136, (int)y - 4, -1);
            panel_text("HOLD UP/DOWN: GROUP", 8, (int)y - 32, -1);
            if (group == groups - 1) {
                static const char *const special[] = {"SPACE", "BACKSPACE", "TAB", "ENTER"};
                panel_text(special[selected], 8, (int)y - 18, -1);
            } else {
                panel_text(key_groups[group], 8, (int)y - 18, selected);
            }
        } else {
            panel_text(vertical ? "POINTER: UP/DOWN" : "POINTER: LEFT/RIGHT", 8, (int)y - 6, -1);
            panel_text("DOUBLE OK: KEYBOARD", 8, (int)y - 24, -1);
        }
        write_row(0, DESKTOP_HEIGHT + y, BSP_LCD_W);
    }
}

static void on_button(bsp_btn_t key, bsp_btn_ev_t event, void *queue)
{
    if (event == BSP_BTN_CLICK || event == BSP_BTN_LONG || event == BSP_BTN_DOUBLE ||
        (key != BSP_BTN_OK && event == BSP_BTN_PRESS)) {
        const input_t input = {key, event};
        xQueueSend(queue, &input, 0);
    }
}

static int move_axis(int axis, int limit, bsp_btn_t key)
{
    int step = limit / 40 + 1;
    axis += key == BSP_BTN_UP ? -step : step;
    if (axis < 0) return 0;
    return axis < limit ? axis : limit - 1;
}

static void vnc_session(const connection_t *cfg)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return;
    struct timeval limit = {.tv_sec = 12};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(5900)};
    inet_pton(AF_INET, cfg->host, &target.sin_addr);
    vnc_client_t client = {.fd = fd};
    if (connect(fd, (struct sockaddr *)&target, sizeof(target)) != 0 || !vnc_handshake(&client, cfg->vnc)) {
        ESP_LOGW(TAG, "VNC connection or authentication failed");
        close(fd);
        return;
    }
    const unsigned max_w = client.width < BSP_LCD_W ? client.width : BSP_LCD_W;
    const unsigned max_h = client.height < DESKTOP_HEIGHT ? client.height : DESKTOP_HEIGHT;
    render_t render = {
        .view = vnc_fit(client.width, client.height, max_w, max_h),
        .source_w = client.width,
        .source_h = client.height,
    };
    render.view.x = (BSP_LCD_W - render.view.w) / 2;
    render.view.y = (DESKTOP_HEIGHT - render.view.h) / 2;
    status_screen(0);
    xQueueReset(keys);
    bool vertical = false, first_frame = true, keyboard = false;
    unsigned group = 0, selected = 0;
    const unsigned groups = sizeof(key_groups) / sizeof(key_groups[0]);
    input_panel(keyboard, vertical, group, selected);
    uint16_t x = client.width / 2, y = client.height / 2;
    ESP_LOGI(TAG, "Connected to Mac desktop %ux%u", client.width, client.height);
    const int64_t first_request = esp_timer_get_time();
    bool running = vnc_request(&client, false);
    while (running && (xEventGroupGetBits(wifi_status) & WIFI_UP)) {
        input_t input;
        while (running && xQueueReceive(keys, &input, 0) == pdTRUE) {
            if (input.key == BSP_BTN_OK && input.event == BSP_BTN_DOUBLE) {
                keyboard = !keyboard;
                if (keyboard) {
                    group = 0;
                    selected = 0;
                }
                input_panel(keyboard, vertical, group, selected);
            } else if (keyboard) {
                if (input.key == BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
                    static const uint32_t special[] = {0x20, 0xff08, 0xff09, 0xff0d};
                    const uint32_t code = group == groups - 1 ? special[selected] : (uint8_t)key_groups[group][selected];
                    running = vnc_key(&client, code, true) && vnc_key(&client, code, false);
                } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) {
                    running = vnc_key(&client, 0xff08, true) && vnc_key(&client, 0xff08, false);
                } else if (input.key != BSP_BTN_OK && input.event == BSP_BTN_LONG) {
                    group = (group + (input.key == BSP_BTN_UP ? groups - 1 : 1)) % groups;
                    selected = 0;
                    input_panel(keyboard, vertical, group, selected);
                } else if (input.key != BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
                    const unsigned size = strlen(key_groups[group]);
                    selected = (selected + (input.key == BSP_BTN_UP ? size - 1 : 1)) % size;
                    input_panel(keyboard, vertical, group, selected);
                }
            } else if (input.key != BSP_BTN_OK && input.event == BSP_BTN_PRESS) {
                if (vertical) y = move_axis(y, client.height, input.key);
                else x = move_axis(x, client.width, input.key);
                running = vnc_pointer(&client, x, y, 0);
            } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) {
                vertical = !vertical;
                input_panel(keyboard, vertical, group, selected);
            } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
                running = vnc_pointer(&client, x, y, 1) && vnc_pointer(&client, x, y, 0);
            }
        }
        if (!running) break;
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(fd, &ready);
        struct timeval wait = {.tv_usec = 30000};
        int result = select(fd + 1, &ready, NULL, NULL, &wait);
        if (result < 0) break;
        if (result == 0) continue;
        uint32_t encoding = 0;
        int update = vnc_receive(&client, render_row, flush_band, &render, network_row, &encoding);
        if (update < 0) break;
        if (update > 0) {
            if (first_frame && render.painted) {
                ESP_LOGI(TAG, "First desktop frame drawn in %ld ms (%s)",
                         (long)((esp_timer_get_time() - first_request) / 1000), encoding == 5 ? "Hextile" : "Raw");
                first_frame = false;
            }
            running = vnc_request(&client, !first_frame);
        }
    }
    close(fd);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 256, .rx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    ESP_ERROR_CHECK(xTaskCreate(usb_setup, "vnc_config", 4608, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(bsp_display_init());
    drawn = xSemaphoreCreateBinary();
    keys = xQueueCreate(12, sizeof(input_t));
    wifi_status = xEventGroupCreate();
    ESP_ERROR_CHECK(drawn && keys && wifi_status ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = on_color_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &callbacks, drawn));
    bsp_display_backlight(70);
    ESP_ERROR_CHECK(bsp_button_init(on_button, keys));
    connection_t cfg = {0};
    if (!load_connection(&cfg)) {
        status_screen(0x0828);
        ESP_LOGI(TAG, "Mac VNC ready for USB configuration");
        return;
    }
    wifi_start(&cfg);
    for (;;) {
        status_screen(0x0828);
        if (!(xEventGroupGetBits(wifi_status) & WIFI_UP)) {
            xEventGroupClearBits(wifi_status, WIFI_FAILED);
            const esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi connect request: %s", esp_err_to_name(err));
        }
        const EventBits_t result = xEventGroupWaitBits(wifi_status, WIFI_UP | WIFI_FAILED,
                                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (result & WIFI_UP) {
            vnc_session(&cfg);
        } else if (!(result & WIFI_FAILED)) {
            esp_wifi_disconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
