#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lvgl.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "launcher_state.h"
#include "radio_player.h"
#include "radio_ui.h"
#include "screen_menu.h"
#include "screen_menu_glyphs.h"
#include "voice_link.h"

#define WIFI_UP BIT0
#define WIFI_FAILED BIT1
#define ROWS_PER_RECORD 8
#define MIRROR_PORT 45902
#define MONITOR_PORT 45904
#define INPUT_TOGGLE_FULLSCREEN 4
#define LANDSCAPE_WIDTH BSP_LCD_H
#define LANDSCAPE_HEIGHT BSP_LCD_W
#define POINTER_HEIGHT 14
#define KEYBOARD_HEIGHT 44
#define MENU_HEADER_HEIGHT 22
#define MENU_BADGE_WIDTH 70
#define MENU_WIDTH 132
#define MENU_ROW_HEIGHT 26
#define MENU_ROWS 4
#define MENU_HEIGHT (2 * MENU_HEADER_HEIGHT + MENU_ROWS * MENU_ROW_HEIGHT)
#define SOURCE_WIDTH_LIMIT LANDSCAPE_WIDTH
#define SOURCE_HEIGHT_LIMIT 276
static const char *TAG = "live_screen";

// Existing Wi-Fi configuration written by the previous firmware.
typedef struct { char ssid[33], wifi[65], host[16], vnc[9]; } connection_t;
typedef struct { char host[16]; uint8_t key[32]; } pairing_t;

static EventGroupHandle_t wifi_status;
static SemaphoreHandle_t drawn;
static QueueHandle_t keys;
typedef struct { bsp_btn_t key; bsp_btn_ev_t event; } input_t;
DMA_ATTR static uint8_t pixels[LANDSCAPE_WIDTH * ROWS_PER_RECORD * 2];
static uint8_t decoded[SOURCE_WIDTH_LIMIT * ROWS_PER_RECORD * 2];
static uint8_t ciphertext[sizeof(decoded)];
static bool speaker_enabled = true;
static launcher_t launcher = LAUNCHER_INITIAL_STATE;
static bool remote_streaming;
static bool portrait = true;
static bool monitor_wanted = true;
static unsigned monitor_audio_mode;
static bool (*image_wait_hook)(void *);
static void *image_wait_context;
extern const uint8_t standby_pixels[] asm("_binary_live_control_standby_rgb565_start");
extern const uint8_t standby_pixels_end[] asm("_binary_live_control_standby_rgb565_end");
extern const uint8_t portrait_pixels[] asm("_binary_live_control_standby_portrait_rgb565_start");
extern const uint8_t portrait_pixels_end[] asm("_binary_live_control_standby_portrait_rgb565_end");

static bool load_connection(connection_t *cfg)
{
    nvs_handle_t nvs;
    if (nvs_open("mac_vnc", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t length = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(nvs, "connection", cfg, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(*cfg);
}

static bool load_pairing(pairing_t *pairing)
{
    nvs_handle_t nvs;
    if (nvs_open("mac_screen", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t length = sizeof(*pairing);
    esp_err_t result = nvs_get_blob(nvs, "pairing", pairing, &length);
    nvs_close(nvs);
    return result == ESP_OK && length == sizeof(*pairing);
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool save_pairing(char *line)
{
    static const char prefix[] = "APSCREEN ";
    if (strncmp(line, prefix, sizeof(prefix) - 1)) return false;
    char *host = line + sizeof(prefix) - 1;
    char *key = strchr(host, ' ');
    if (!key) return false;
    *key++ = 0;
    pairing_t pairing = {0};
    struct in_addr ip;
    if (strlen(host) >= sizeof(pairing.host) || strlen(key) != sizeof(pairing.key) * 2 ||
        inet_pton(AF_INET, host, &ip) != 1) return false;
    strcpy(pairing.host, host);
    for (unsigned i = 0; i < sizeof(pairing.key); ++i) {
        int hi = hex_value(key[2 * i]), lo = hex_value(key[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        pairing.key[i] = (hi << 4) | lo;
    }
    nvs_handle_t nvs;
    if (nvs_open("mac_screen", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(nvs, "pairing", &pairing, sizeof(pairing));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

static void usb_setup(void *unused)
{
    (void)unused;
    char line[96];
    unsigned length = 0;
    bool overflow = false;
    for (;;) {
        char ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(1000)) != 1) continue;
        if (ch != '\n') {
            if (length < sizeof(line) - 1) line[length++] = ch;
            else overflow = true;
            continue;
        }
        line[length] = 0;
        bool command = length && !overflow && !strncmp(line, "APSCREEN ", 9);
        bool ok = command && save_pairing(line);
        memset(line, 0, sizeof(line));
        length = 0;
        overflow = false;
        if (!command) continue;
        const char *reply = ok ? "APSCREEN PAIRED\n" : "APSCREEN ERROR\n";
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
        (void)esp_netif_sntp_start();
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

static void maintain_wifi(void *unused)
{
    (void)unused;
    for (;;) {
        if (!(xEventGroupGetBits(wifi_status) & WIFI_UP)) {
            xEventGroupClearBits(wifi_status, WIFI_FAILED);
            esp_wifi_connect();
            xEventGroupWaitBits(wifi_status, WIFI_UP | WIFI_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(context, &woken);
    return woken == pdTRUE;
}

static void write_pixels(unsigned x, unsigned y, unsigned width, unsigned rows)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(bsp_display_panel(), x, y, x + width, y + rows, pixels));
    xSemaphoreTake(drawn, portMAX_DELAY);
}

static void status_screen(uint16_t color)
{
    for (unsigned i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = color >> 8;
        pixels[i + 1] = color;
    }
    const unsigned width = portrait ? BSP_LCD_W : LANDSCAPE_WIDTH;
    const unsigned height = portrait ? BSP_LCD_H : LANDSCAPE_HEIGHT;
    for (unsigned y = 0; y < height; y += ROWS_PER_RECORD) {
        unsigned rows = height - y < ROWS_PER_RECORD ? height - y : ROWS_PER_RECORD;
        write_pixels(0, y, width, rows);
    }
}

static void set_portrait(bool enabled)
{
    if (portrait == enabled) return;
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(bsp_display_panel(), !enabled));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(bsp_display_panel(), !enabled, false));
    portrait = enabled;
}

static void show_standby(void)
{
    const unsigned width = portrait ? BSP_LCD_W : LANDSCAPE_WIDTH;
    const unsigned height = portrait ? BSP_LCD_H : LANDSCAPE_HEIGHT;
    const uint8_t *image = portrait ? portrait_pixels : standby_pixels;
    for (unsigned y = 0; y < height; y += ROWS_PER_RECORD) {
        const unsigned rows = height - y < ROWS_PER_RECORD ? height - y : ROWS_PER_RECORD;
        const size_t stride = width * 2;
        memcpy(pixels, image + y * stride, rows * stride);
        write_pixels(0, y, width, rows);
    }
}

static const char *menu_label(screen_menu_item_t item, bool speaker)
{
    static const char *const labels[] = {
        [MENU_DISPLAY] = "画面",
        [MENU_CONTROL] = "控制",
        [MENU_AUDIO] = "语音",
        [MENU_SYSTEM] = "系统",
        [MENU_FULLSCREEN] = "切换全屏",
        [MENU_HORIZONTAL] = "横向移动",
        [MENU_VERTICAL] = "纵向移动",
        [MENU_KEYBOARD] = "屏幕键盘",
        [MENU_TALK] = "按住下键对讲",
        [MENU_STANDBY] = "待机",
        [MENU_HOME] = "返回首页",
    };
    if (item == MENU_NONE) return "菜单";
    if (item == MENU_SPEAKER) return speaker ? "关闭扬声器" : "开启扬声器";
    return labels[item];
}

// Every label is fixed CJK text. The matching 16x16 masks are stored in flash.
static void menu_text(uint8_t *line, unsigned width, const char *label, unsigned x,
                      unsigned row, uint16_t color)
{
    const uint8_t *ch = (const uint8_t *)label;
    for (; *ch && x + MENU_GLYPH_SIZE <= width; ch += 3, x += MENU_GLYPH_SIZE) {
        uint16_t codepoint = ((ch[0] & 0x0f) << 12) | ((ch[1] & 0x3f) << 6) | (ch[2] & 0x3f);
        for (unsigned i = 0; i < sizeof(screen_menu_glyphs) / sizeof(screen_menu_glyphs[0]); ++i) {
            if (screen_menu_glyphs[i].codepoint != codepoint) continue;
            const uint16_t bits = screen_menu_glyphs[i].rows[row];
            for (unsigned col = 0; col < MENU_GLYPH_SIZE; ++col) {
                if (!(bits & (0x8000u >> col))) continue;
                line[2 * (x + col)] = color >> 8;
                line[2 * (x + col) + 1] = color;
            }
            break;
        }
    }
}

static void paint_menu_line(uint8_t *line, const char *label, unsigned width, unsigned row,
                            unsigned height, uint16_t background, uint16_t foreground, bool icon)
{
    for (unsigned x = 0; x < width; ++x) {
        const uint16_t color = icon && x >= 10 && x < 20 &&
                               (row == 7 || row == 11 || row == 15) ? foreground : background;
        line[2 * x] = color >> 8;
        line[2 * x + 1] = color;
    }
    const int glyph_row = (int)row - (int)(height - MENU_GLYPH_SIZE) / 2;
    if (glyph_row >= 0 && glyph_row < MENU_GLYPH_SIZE) {
        menu_text(line, width, label, icon ? 27 : 9, glyph_row, foreground);
    }
}

static void menu_row(const char *label, unsigned width, unsigned y, unsigned height,
                     uint16_t background, uint16_t foreground, bool icon)
{
    for (unsigned row = 0; row < height; ++row) {
        paint_menu_line(pixels, label, width, row, height, background, foreground, icon);
        write_pixels(LANDSCAPE_WIDTH - width, y + row, width, 1);
    }
}

static void menu_badge(void)
{
    menu_row("菜单", MENU_BADGE_WIDTH, 0, MENU_HEADER_HEIGHT, 0x1352, 0xffff, true);
}

static void write_desktop_band(unsigned x, unsigned y, unsigned width, unsigned rows, bool menu_open)
{
    if (menu_open) {
        const unsigned menu_x = LANDSCAPE_WIDTH - MENU_WIDTH;
        if (y >= MENU_HEIGHT || x + width <= menu_x) {
            write_pixels(x, y, width, rows);
            return;
        }
        const unsigned above_footer = MENU_HEIGHT - y;
        const unsigned covered = rows < above_footer ? rows : above_footer;
        const unsigned left = x < menu_x ? menu_x - x : 0;
        if (left) {
            for (unsigned row = 1; row < covered; ++row) {
                memmove(pixels + 2 * row * left, pixels + 2 * row * width, 2 * left);
            }
            write_pixels(x, y, left, covered);
        }
        if (covered < rows) {
            memmove(pixels, pixels + 2 * covered * width, 2 * (rows - covered) * width);
            write_pixels(x, y + covered, width, rows - covered);
        }
        return;
    }

    // Keep the badge above the picture without reserving a strip or rescaling the desktop.
    const unsigned badge_x = LANDSCAPE_WIDTH - MENU_BADGE_WIDTH;
    const unsigned first = x > badge_x ? x : badge_x;
    if (y < MENU_HEADER_HEIGHT && x + width > first) {
        uint8_t line[MENU_BADGE_WIDTH * 2];
        for (unsigned row = 0; row < rows && y + row < MENU_HEADER_HEIGHT; ++row) {
            paint_menu_line(line, "菜单", MENU_BADGE_WIDTH, y + row, MENU_HEADER_HEIGHT, 0x1352, 0xffff, true);
            memcpy(pixels + 2 * (row * width + first - x), line + 2 * (first - badge_x),
                   2 * (x + width - first));
        }
    }
    write_pixels(x, y, width, rows);
}

static void draw_menu(const screen_menu_t *menu, bool speaker)
{
    const char *title = menu->submenu ?
        menu_label(screen_menu_pages[menu->category].title, speaker) : "菜单";
    menu_row(title, MENU_WIDTH, 0, MENU_HEADER_HEIGHT, 0x1352, 0xffff, true);
    for (unsigned row = 0; row < MENU_ROWS; ++row) {
        const bool available = row < screen_menu_count(menu);
        const bool selected = available && row == menu->selected;
        menu_row(available ? menu_label(screen_menu_item(menu, row), speaker) : "",
                 MENU_WIDTH, MENU_HEADER_HEIGHT + row * MENU_ROW_HEIGHT, MENU_ROW_HEIGHT,
                 selected ? 0xff32 : 0x0865, selected ? 0x0828 : 0xffff, false);
    }
    menu_row("长按确定返回", MENU_WIDTH, MENU_HEADER_HEIGHT + MENU_ROWS * MENU_ROW_HEIGHT,
             MENU_HEADER_HEIGHT, 0x1352, 0xffff, false);
}

static void ui_block(const char *label, unsigned x, unsigned y, unsigned width, unsigned height,
                     uint16_t background, uint16_t foreground)
{
    const unsigned text_width = strlen(label) / 3 * MENU_GLYPH_SIZE;
    const unsigned text_x = text_width < width ? (width - text_width) / 2 : 0;
    for (unsigned row = 0; row < height; ++row) {
        for (unsigned col = 0; col < width; ++col) {
            pixels[2 * col] = background >> 8;
            pixels[2 * col + 1] = background;
        }
        const int glyph_row = (int)row - (int)(height - MENU_GLYPH_SIZE) / 2;
        if (glyph_row >= 0 && glyph_row < MENU_GLYPH_SIZE) {
            menu_text(pixels, width, label, text_x, glyph_row, foreground);
        }
        write_pixels(x, y + row, width, 1);
    }
}

static void draw_home(void)
{
    status_screen(0x21af);
    ui_block("选择功能", 12, 24, 216, 36, 0x21af, 0xffff);
    ui_block("电台", 12, 72, 216, 54, launcher.home_selection == 0 ? 0xff32 : 0x0865,
             launcher.home_selection == 0 ? 0x0828 : 0xffff);
    ui_block("远程桌面", 12, 142, 216, 54, launcher.home_selection == 1 ? 0xff32 : 0x0865,
             launcher.home_selection == 1 ? 0x0828 : 0xffff);
    ui_block("远程监控", 12, 212, 216, 54, launcher.home_selection == 2 ? 0xff32 : 0x0865,
             launcher.home_selection == 2 ? 0x0828 : 0xffff);
    ui_block("上下选择确定进入", 0, 288, 240, 32, 0x21af, 0xffff);
}

static void draw_radio(unsigned flags)
{
    static const char *const statuses[] = {
        [RADIO_STOPPED] = "准备播放",
        [RADIO_WAIT_WIFI] = "等待无线网络",
        [RADIO_WAIT_TIME] = "正在校时",
        [RADIO_CONNECTING] = "正在连接",
        [RADIO_BUFFERING] = "正在缓冲",
        [RADIO_PLAYING] = "正在播放",
        [RADIO_RETRYING] = "正在重新连接",
    };
    static int battery = -1;
    static TickType_t battery_checked;
    const TickType_t now = xTaskGetTickCount();
    if (!battery_checked || now - battery_checked >= pdMS_TO_TICKS(5000)) {
        battery = bsp_battery_soc();
        battery_checked = now;
    }
    radio_telemetry_t audio;
    radio_player_telemetry(&audio);
    wifi_ap_record_t ap;
    const int signal = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ?
                       (ap.rssi > -55 ? 4 : ap.rssi > -67 ? 3 : ap.rssi > -78 ? 2 : 1) : 0;
    radio_ui_t ui = {.app = &launcher, .status = statuses[radio_player_status()],
                     .elapsed_seconds = audio.elapsed_seconds, .sample_rate = audio.sample_rate,
                     .bitrate = audio.bitrate, .channels = audio.channels, .battery = battery, .signal = signal};
    if (!launcher.paused && radio_player_status() == RADIO_PLAYING) memcpy(ui.levels, audio.levels, sizeof(ui.levels));
    radio_ui_draw(&ui, flags, pixels, write_pixels);
}

static void draw_remote_wait(bool paired, bool configured)
{
    show_standby();
    const char *label = !configured ? "请先配置无线网络" : !paired ? "请先用数据线配对" :
                        (xEventGroupGetBits(wifi_status) & WIFI_UP) ? "正在连接远程桌面" : "等待无线网络";
    ui_block(label, 0, 176, 320, 32, 0x1352, 0xffff);
    ui_block("长按确定返回首页", 0, 208, 320, 32, 0x0865, 0xffff);
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
    for (unsigned index = 0; text[index] && x + 8 <= LANDSCAPE_WIDTH; ++index, x += 8) {
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
            pixels[2 * (x + column)] = color >> 8;
            pixels[2 * (x + column) + 1] = color;
        }
    }
}

static void input_panel(bool keyboard, bool vertical, unsigned group, unsigned selected)
{
    const unsigned groups = sizeof(key_groups) / sizeof(key_groups[0]);
    const unsigned height = keyboard ? KEYBOARD_HEIGHT : POINTER_HEIGHT;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < LANDSCAPE_WIDTH; ++x) {
            pixels[2 * x] = 0x08;
            pixels[2 * x + 1] = 0x65;
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
            panel_text(vertical ? "POINTER: UP/DOWN" : "POINTER: LEFT/RIGHT", 8, (int)y - 2, -1);
            panel_text(voice_talking() ? "MIC ON" : voice_connected() ? "HOLD DOWN: TALK" :
                       "HOLD UP: MENU", 164, (int)y - 2, -1);
        }
        write_pixels(0, LANDSCAPE_HEIGHT - height + y, LANDSCAPE_WIDTH, 1);
    }
}

static void on_button(bsp_btn_t key, bsp_btn_ev_t event, void *queue)
{
    static bool direction_held[2];
    if (key == BSP_BTN_UP || key == BSP_BTN_DOWN) {
        if (event == BSP_BTN_PRESS) {
            direction_held[key] = false;
            return;
        }
        if (event == BSP_BTN_LONG) {
            direction_held[key] = true;
            if (key == BSP_BTN_DOWN) voice_push_to_talk(true);
        } else if (event == BSP_BTN_RELEASE) {
            if (key == BSP_BTN_DOWN) voice_push_to_talk(false);
            if (direction_held[key]) return;
            // A held direction opens a menu or starts talking without moving the pointer.
            event = BSP_BTN_PRESS;
        }
    }
    if (event == BSP_BTN_CLICK || event == BSP_BTN_LONG || event == BSP_BTN_DOUBLE || event == BSP_BTN_TRIPLE ||
        (key != BSP_BTN_OK && event == BSP_BTN_PRESS)) {
        const input_t input = {key, event};
        xQueueSend(queue, &input, 0);
    }
}

static bool transfer(int fd, uint8_t *bytes, size_t size, bool outgoing)
{
    const TickType_t started = xTaskGetTickCount();
    while (size) {
        int count = outgoing ? send(fd, bytes, size, 0) : recv(fd, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!outgoing && image_wait_hook && !image_wait_hook(image_wait_context)) return false;
            if (!remote_streaming && launcher.page == PAGE_REMOTE) {
                input_t input;
                while (xQueueReceive(keys, &input, 0) == pdTRUE) {
                    if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) launcher.page = PAGE_HOME;
                }
            }
            if ((launcher.page != PAGE_REMOTE && launcher.page != PAGE_MONITOR) ||
                xTaskGetTickCount() - started >= pdMS_TO_TICKS(10000)) return false;
            fd_set waiting;
            FD_ZERO(&waiting);
            FD_SET(fd, &waiting);
            struct timeval timeout = {.tv_usec = 100000};
            const int ready = select(fd + 1, outgoing ? NULL : &waiting,
                                     outgoing ? &waiting : NULL, NULL, &timeout);
            if (ready < 0 && errno != EINTR) return false;
            continue;
        }
        if (count <= 0) return false;
        bytes += count;
        size -= count;
    }
    return true;
}

static unsigned read_u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool begin_session(int fd, const pairing_t *pairing, mbedtls_gcm_context *gcm,
                          mbedtls_gcm_context *input_gcm, const char *protocol)
{
    uint8_t greeting[37] = {'A', 'P', 'S', 'C', '2'};
    uint8_t session_key[32], proof[32], input_key[32];
    memcpy(greeting, protocol, 5);
    static const uint8_t input_label[] = "input";
    static const uint8_t purpose[] = "client";
    esp_fill_random(greeting + 5, 16);
    if (!transfer(fd, greeting, 21, true) || !transfer(fd, greeting + 21, 16, false)) return false;
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = sha256 && mbedtls_md_hmac(sha256, pairing->key, sizeof(pairing->key), greeting,
                                         sizeof(greeting), session_key) == 0 &&
        mbedtls_md_hmac(sha256, session_key, sizeof(session_key), purpose, sizeof(purpose) - 1, proof) == 0 &&
        mbedtls_gcm_setkey(gcm, MBEDTLS_CIPHER_ID_AES, session_key, 256) == 0 &&
        mbedtls_md_hmac(sha256, session_key, sizeof(session_key), input_label, sizeof(input_label) - 1, input_key) == 0 &&
        mbedtls_gcm_setkey(input_gcm, MBEDTLS_CIPHER_ID_AES, input_key, 256) == 0 &&
        transfer(fd, proof, sizeof(proof), true);
    memset(session_key, 0, sizeof(session_key));
    memset(proof, 0, sizeof(proof));
    memset(input_key, 0, sizeof(input_key));
    return ok;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}

static bool send_input(int fd, mbedtls_gcm_context *gcm, uint32_t *sequence, uint8_t kind, uint32_t value)
{
    if (*sequence == UINT32_MAX) return false;
    uint8_t packet[25], nonce[12] = {0}, data[5] = {kind};
    put_u32(packet, (*sequence)++);
    memcpy(nonce + 8, packet, 4);
    put_u32(data + 1, value);
    return mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, sizeof(data), nonce, sizeof(nonce),
                                    packet, 4, data, packet + 4, 16, packet + 9) == 0 &&
        transfer(fd, packet, sizeof(packet), true);
}

typedef struct {
    unsigned x, y, group, selected;
    bool keyboard, vertical, standby, redraw;
    screen_menu_t menu;
} controls_t;

static unsigned move_axis(unsigned value, bsp_btn_t key)
{
    const int next = (int)value + (key == BSP_BTN_UP ? -1300 : 1300);
    if (next < 0) return 0;
    return next > UINT16_MAX ? UINT16_MAX : next;
}

static bool fullscreen(int fd, mbedtls_gcm_context *gcm, uint32_t *sequence)
{
    if (!send_input(fd, gcm, sequence, INPUT_TOGGLE_FULLSCREEN, 0)) return false;
    ESP_LOGI(TAG, "Fullscreen toggle sent");
    return true;
}

static bool choose_menu_action(int fd, mbedtls_gcm_context *gcm, uint32_t *sequence,
                               controls_t *c, screen_menu_item_t item)
{
    switch (item) {
    case MENU_NONE:
        return true;
    case MENU_FULLSCREEN:
        if (!fullscreen(fd, gcm, sequence)) return false;
        break;
    case MENU_HORIZONTAL:
    case MENU_VERTICAL:
        c->vertical = item == MENU_VERTICAL;
        break;
    case MENU_KEYBOARD:
        c->keyboard = true;
        c->group = c->selected = 0;
        break;
    case MENU_SPEAKER:
        speaker_enabled = !speaker_enabled;
        voice_speaker_mode(speaker_enabled);
        break;
    case MENU_TALK:
        c->keyboard = false;
        break;
    case MENU_STANDBY:
        c->standby = true;
        voice_speaker_mode(false);
        set_portrait(true);
        show_standby();
        bsp_display_backlight(45);
        break;
    case MENU_HOME:
        launcher.page = PAGE_HOME;
        voice_link_enable(false);
        break;
    default:
        break;
    }
    return true;
}

static bool handle_inputs(int fd, mbedtls_gcm_context *gcm, uint32_t *sequence, controls_t *c)
{
    const unsigned groups = sizeof(key_groups) / sizeof(key_groups[0]);
    input_t input;
    while (xQueueReceive(keys, &input, 0) == pdTRUE) {
        if (c->standby) {
            // Wake on a complete gesture and consume it so it cannot click the Mac.
            if (input.event == BSP_BTN_PRESS) continue;
            c->standby = false;
            c->redraw = true;
            set_portrait(false);
            bsp_display_backlight(75);
            voice_speaker_mode(speaker_enabled);
            voice_pointer_mode(!c->keyboard);
            break;
        }
        if (c->menu.open) {
            if (input.key != BSP_BTN_OK && input.event == BSP_BTN_PRESS) {
                screen_menu_move(&c->menu, input.key == BSP_BTN_UP ? -1 : 1);
            } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
                if (!choose_menu_action(fd, gcm, sequence, c, screen_menu_choose(&c->menu))) return false;
            } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) {
                screen_menu_back(&c->menu);
            } else continue;

            if (c->menu.open) draw_menu(&c->menu, speaker_enabled);
            else {
                c->redraw = true;
                voice_pointer_mode(!c->keyboard && !c->standby);
                break;
            }
            continue;
        }
        if (!c->keyboard && input.key == BSP_BTN_UP && input.event == BSP_BTN_LONG) {
            screen_menu_open(&c->menu);
            voice_pointer_mode(false);
            draw_menu(&c->menu, speaker_enabled);
            continue;
        }
        if (input.key == BSP_BTN_OK && input.event == BSP_BTN_TRIPLE) {
            if (!fullscreen(fd, gcm, sequence)) return false;
            continue;
        }
        if (input.key == BSP_BTN_OK && input.event == BSP_BTN_DOUBLE) {
            c->keyboard = !c->keyboard;
            voice_pointer_mode(!c->keyboard);
            if (c->keyboard) c->group = c->selected = 0;
        } else if (c->keyboard) {
            if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) {
                if (!send_input(fd, gcm, sequence, 3, 0xff08)) return false;
                continue;
            }
            if (input.key == BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
                static const uint32_t special[] = {0x20, 0xff08, 0xff09, 0xff0d};
                uint32_t code = c->group == groups - 1 ? special[c->selected] : (uint8_t)key_groups[c->group][c->selected];
                if (!send_input(fd, gcm, sequence, 3, code)) return false;
                continue;
            }
            if (input.key != BSP_BTN_OK && input.event == BSP_BTN_LONG) {
                c->group = (c->group + (input.key == BSP_BTN_UP ? groups - 1 : 1)) % groups;
                c->selected = 0;
            } else if (input.key != BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
                const unsigned count = strlen(key_groups[c->group]);
                c->selected = (c->selected + (input.key == BSP_BTN_UP ? count - 1 : 1)) % count;
            } else continue;
        } else if (input.key != BSP_BTN_OK && input.event == BSP_BTN_PRESS) {
            if (c->vertical) c->y = move_axis(c->y, input.key);
            else c->x = move_axis(c->x, input.key);
            if (!send_input(fd, gcm, sequence, 1, (c->x << 16) | c->y)) return false;
            continue;
        } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_CLICK) {
            if (!send_input(fd, gcm, sequence, 2, (c->x << 16) | c->y)) return false;
            continue;
        } else if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) {
            c->vertical = !c->vertical;
        } else continue;
        input_panel(c->keyboard, c->vertical, c->group, c->selected);
    }
    return true;
}

typedef struct {
    unsigned x, y, width, height;
    uint16_t source_column[LANDSCAPE_WIDTH];
} screen_view_t;

static void fit_landscape(screen_view_t *view, unsigned width, unsigned height, bool keyboard)
{
    const unsigned available_height = LANDSCAPE_HEIGHT - (keyboard ? KEYBOARD_HEIGHT : POINTER_HEIGHT);
    unsigned out_width = LANDSCAPE_WIDTH, out_height = available_height;
    if (width * available_height >= height * LANDSCAPE_WIDTH) {
        out_height = height * LANDSCAPE_WIDTH / width;
        if (!out_height) out_height = 1;
    } else {
        out_width = width * available_height / height;
        if (!out_width) out_width = 1;
    }
    view->width = out_width;
    view->height = out_height;
    view->x = (LANDSCAPE_WIDTH - out_width) / 2;
    view->y = (available_height - out_height) / 2;
    for (unsigned x = 0; x < out_width; ++x) view->source_column[x] = x * width / out_width;
}

static void draw_scaled_band(const screen_view_t *view, unsigned width, unsigned height,
                             unsigned source_y, unsigned source_rows, bool menu_open)
{
    if (view->width == width && view->height == height) {
        memcpy(pixels, decoded, width * source_rows * 2);
        write_desktop_band(view->x, view->y + source_y, width, source_rows, menu_open);
        return;
    }
    // These ceilings give adjacent source records non-overlapping output rows.
    unsigned y = (source_y * view->height + height - 1) / height;
    const unsigned end = ((source_y + source_rows) * view->height + height - 1) / height;
    while (y < end) {
        const unsigned rows = end - y < ROWS_PER_RECORD ? end - y : ROWS_PER_RECORD;
        for (unsigned row = 0; row < rows; ++row) {
            const unsigned input_y = (y + row) * height / view->height - source_y;
            const uint8_t *input = decoded + input_y * width * 2;
            uint8_t *output = pixels + row * view->width * 2;
            for (unsigned x = 0; x < view->width; ++x) {
                const unsigned src = view->source_column[x] * 2;
                output[2 * x] = input[src];
                output[2 * x + 1] = input[src + 1];
            }
        }
        write_desktop_band(view->x, view->y + y, view->width, rows, menu_open);
        y += rows;
    }
}

static int connect_image_peer(const pairing_t *pairing, unsigned port, launcher_page_t page)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); return -1; }
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, pairing->host, &target.sin_addr);
    bool connected = connect(fd, (struct sockaddr *)&target, sizeof(target)) == 0;
    if (!connected && errno == EINPROGRESS) {
        const TickType_t started = xTaskGetTickCount();
        while (launcher.page == page && xTaskGetTickCount() - started < pdMS_TO_TICKS(4000)) {
            input_t input;
            while (xQueueReceive(keys, &input, 0) == pdTRUE) {
                if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) launcher.page = PAGE_HOME;
            }
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            struct timeval timeout = {.tv_usec = 100000};
            const int ready = select(fd + 1, NULL, &writable, NULL, &timeout);
            if (ready < 0) break;
            if (!ready) continue;
            int error = 0;
            socklen_t length = sizeof(error);
            connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error;
            break;
        }
    }
    if (!connected || launcher.page != page) { close(fd); return -1; }
    return fd;
}

static void receive_screen(const pairing_t *pairing)
{
    remote_streaming = false;
    int fd = connect_image_peer(pairing, MIRROR_PORT, PAGE_REMOTE);
    if (fd < 0) return;
    mbedtls_gcm_context gcm, input_gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_init(&input_gcm);
    bool connected = begin_session(fd, pairing, &gcm, &input_gcm, "APSC2");
    uint32_t expected = 1, frames = 0, input_sequence = 1;
    controls_t controls = {.x = 32768, .y = 32768};
    xQueueReset(keys);
    unsigned last_width = 0, last_height = 0, expected_y = 0;
    bool last_keyboard = false;
    bool last_voice_connected = false, last_voice_talking = false;
    screen_view_t view = {0};
    if (connected) {
        voice_pointer_mode(true);
        voice_speaker_mode(speaker_enabled);
    }
    while (connected && launcher.page == PAGE_REMOTE && (xEventGroupGetBits(wifi_status) & WIFI_UP)) {
        if (frames && !expected_y && !handle_inputs(fd, &input_gcm, &input_sequence, &controls)) break;
        if (launcher.page != PAGE_REMOTE) break;
        uint8_t header[12], nonce[12] = {0}, tag[16];
        if (!transfer(fd, header, sizeof(header), false)) break;
        unsigned width = read_u16(header), height = read_u16(header + 2);
        unsigned y = read_u16(header + 4), rows = read_u16(header + 6);
        uint32_t sequence = read_u32(header + 8);
        if (!width || width > SOURCE_WIDTH_LIMIT || !height || height > SOURCE_HEIGHT_LIMIT || y != expected_y ||
            !rows || rows > ROWS_PER_RECORD || y + rows > height || sequence != expected ||
            (y && (width != last_width || height != last_height))) break;
        size_t size = width * rows * 2;
        memcpy(nonce + 8, header + 8, 4);
        if (!transfer(fd, ciphertext, size, false) || !transfer(fd, tag, sizeof(tag), false) ||
            mbedtls_gcm_auth_decrypt(&gcm, size, nonce, sizeof(nonce), header, sizeof(header),
                                      tag, sizeof(tag), ciphertext, decoded) != 0) break;
        bool linked = voice_connected(), talking = voice_talking();
        const bool desktop_visible = !controls.standby;
        if (!y) {
            if (width != last_width || height != last_height || last_keyboard != controls.keyboard) {
                fit_landscape(&view, width, height, controls.keyboard);
                controls.redraw = true;
            }
            if (desktop_visible && controls.redraw) {
                status_screen(0);
                if (controls.menu.open) draw_menu(&controls.menu, speaker_enabled);
                else menu_badge();
                input_panel(controls.keyboard, controls.vertical, controls.group, controls.selected);
                controls.redraw = false;
            } else if (desktop_visible && !controls.keyboard &&
                       (last_voice_connected != linked || last_voice_talking != talking)) {
                input_panel(false, controls.vertical, controls.group, controls.selected);
            }
            last_width = width;
            last_height = height;
            last_keyboard = controls.keyboard;
            last_voice_connected = linked;
            last_voice_talking = talking;
        }
        if (desktop_visible) draw_scaled_band(&view, width, height, y, rows, controls.menu.open);
        expected_y = (y + rows) % height;
        if (!expected_y) {
            ++frames;
            remote_streaming = true;
            if (frames == 1 || frames == 30) ESP_LOGI(TAG, "Frame %lu drawn (%ux%u source, %ux%u landscape)",
                                                         (unsigned long)frames, width, height, view.width, view.height);
        }
        if (++expected == 0) break;
    }
    voice_pointer_mode(false);
    voice_speaker_mode(speaker_enabled);
    set_portrait(false);
    bsp_display_backlight(75);
    mbedtls_gcm_free(&input_gcm);
    mbedtls_gcm_free(&gcm);
    close(fd);
}


typedef struct {
    int fd;
    mbedtls_gcm_context *input_gcm;
    uint32_t input_sequence;
    unsigned selected, state, audio_state;
    bool pending, audio_pending, talking;
} monitor_controls_t;

static void monitor_controls(const monitor_controls_t *controls)
{
    static const char *const status[] = {
        "摄像头已关闭", "正在开启摄像头", "摄像头已开启", "请在电脑允许摄像头",
        "未找到摄像头", "摄像头连接失败", "正在关闭摄像头", "正在连接电脑",
    };
    static const char *const audio_status[] = {
        "声音已关闭", "正在开启声音", "监听声音已开启", "对讲按住下键讲话",
        "请在电脑允许麦克风", "声音连接失败", "正在关闭声音",
    };
    ui_block("远程监控", 0, 0, 240, 28, 0x1352, 0xffff);
    ui_block(status[controls->state], 0, 28, 240, 20, 0x0865, 0xffff);
    ui_block(controls->talking ? "正在向电脑讲话" : audio_status[controls->audio_state],
             0, 48, 240, 20, 0x0865, 0xffff);
    static const char *const actions[] = {
        "开启镜头", "关闭镜头", "开启声音", "关闭声音", "语音对讲", "返回首页",
    };
    for (unsigned i = 0; i < 6; ++i) {
        ui_block(actions[i], 8 + (i % 2) * 116, 224 + (i / 2) * 30, 108, 24,
                 controls->selected == i ? 0xff32 : 0x1352, controls->selected == i ? 0x0828 : 0xffff);
    }
}

static void monitor_clear_picture(void)
{
    ui_block("", 0, 72, 240, 146, 0x0000, 0xffff);
}

static bool monitor_keys(void *context)
{
    monitor_controls_t *controls = context;
    const bool talking = voice_talking();
    if (controls->talking != talking) {
        controls->talking = talking;
        monitor_controls(controls);
    }
    input_t input;
    while (xQueueReceive(keys, &input, 0) == pdTRUE) {
        if (input.key == BSP_BTN_OK && input.event == BSP_BTN_LONG) {
            launcher.page = PAGE_HOME;
            monitor_wanted = false;
            monitor_audio_mode = 0;
            voice_link_enable(false);
            (void)send_input(controls->fd, controls->input_gcm, &controls->input_sequence, 5, 0);
            return false;
        }
        if (input.event != BSP_BTN_CLICK) continue;
        if (input.key == BSP_BTN_OK) {
            if (controls->selected == 5) {
                launcher.page = PAGE_HOME;
                monitor_audio_mode = 0;
                voice_link_enable(false);
                (void)send_input(controls->fd, controls->input_gcm, &controls->input_sequence, 5, 0);
                return false;
            } else if (controls->selected < 2) {
                const bool enabled = controls->selected == 0;
                if (!send_input(controls->fd, controls->input_gcm, &controls->input_sequence, 5, enabled)) return false;
                monitor_wanted = enabled;
                controls->pending = true;
                controls->state = enabled ? 1 : 6;
                controls->selected = enabled ? 1 : 0;
                if (!enabled) monitor_clear_picture();
            } else {
                const unsigned mode = controls->selected == 2 ? 1 : controls->selected == 4 ? 2 : 0;
                if (!send_input(controls->fd, controls->input_gcm, &controls->input_sequence, 6, mode)) return false;
                monitor_audio_mode = mode;
                controls->audio_pending = true;
                controls->audio_state = mode ? 1 : 6;
                controls->selected = mode ? 3 : 2;
                voice_link_enable(mode != 0);
                voice_pointer_mode(mode == 2);
                voice_speaker_mode(mode != 0);
            }
        } else {
            controls->selected = (controls->selected + (input.key == BSP_BTN_UP ? 5 : 1)) % 6;
        }
        monitor_controls(controls);
    }
    return launcher.page == PAGE_MONITOR;
}

static void fit_monitor(screen_view_t *view, unsigned width, unsigned height)
{
    unsigned out_width = 240, out_height = height * 240 / width;
    if (out_height > 142) { out_height = 142; out_width = width * 142 / height; }
    view->width = out_width ? out_width : 1;
    view->height = out_height ? out_height : 1;
    view->x = (240 - view->width) / 2;
    view->y = 72 + (146 - view->height) / 2;
    for (unsigned x = 0; x < view->width; ++x) view->source_column[x] = x * width / view->width;
}

static void monitor_wait_screen(const char *label)
{
    set_portrait(true);
    status_screen(0x0865);
    ui_block("远程监控", 0, 0, 240, 36, 0x1352, 0xffff);
    ui_block(label, 0, 112, 240, 48, 0x0865, 0xffff);
    ui_block("长按确定返回首页", 0, 288, 240, 32, 0x0865, 0xffff);
}

static void receive_monitor(const pairing_t *pairing)
{
    remote_streaming = true;
    int fd = connect_image_peer(pairing, MONITOR_PORT, PAGE_MONITOR);
    if (fd < 0) return;
    mbedtls_gcm_context gcm, input_gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_init(&input_gcm);
    bool connected = begin_session(fd, pairing, &gcm, &input_gcm, "APCM2");
    monitor_controls_t controls = {
        .fd = fd, .input_gcm = &input_gcm, .input_sequence = 1,
        .selected = monitor_wanted ? 1 : 0, .state = 7, .audio_pending = monitor_audio_mode != 0,
    };
    if (connected) connected = send_input(fd, &input_gcm, &controls.input_sequence, 5, monitor_wanted);
    if (connected) connected = send_input(fd, &input_gcm, &controls.input_sequence, 6, monitor_audio_mode);
    uint32_t expected = 1, frames = 0;
    unsigned expected_y = 0, last_width = 0, last_height = 0;
    screen_view_t view = {0};
    if (connected) {
        voice_link_monitor_target(true);
        voice_link_enable(monitor_audio_mode != 0);
        voice_pointer_mode(monitor_audio_mode == 2);
        voice_speaker_mode(monitor_audio_mode != 0);
        status_screen(0x0865);
        monitor_clear_picture();
        monitor_controls(&controls);
        image_wait_hook = monitor_keys;
        image_wait_context = &controls;
    }
    while (connected && launcher.page == PAGE_MONITOR && (xEventGroupGetBits(wifi_status) & WIFI_UP)) {
        if (!monitor_keys(&controls)) break;
        uint8_t header[12], nonce[12] = {0}, tag[16];
        if (!transfer(fd, header, sizeof(header), false)) break;
        const unsigned width = read_u16(header), height = read_u16(header + 2);
        const unsigned y = read_u16(header + 4), rows = read_u16(header + 6);
        if (read_u32(header + 8) != expected) break;
        size_t size = 0;
        if (!width) {
            if ((height > 6 && (height < 16 || height > 22)) || y || rows || expected_y) break;
        } else {
            if (width > 240 || !height || height > SOURCE_HEIGHT_LIMIT || y != expected_y ||
                !rows || rows > ROWS_PER_RECORD || y + rows > height ||
                (y && (width != last_width || height != last_height))) break;
            size = width * rows * 2;
        }
        memcpy(nonce + 8, header + 8, 4);
        if (!transfer(fd, ciphertext, size, false) || !transfer(fd, tag, sizeof(tag), false) ||
            mbedtls_gcm_auth_decrypt(&gcm, size, nonce, sizeof(nonce), header, sizeof(header),
                                    tag, sizeof(tag), ciphertext, decoded) != 0) break;
        if (!width) {
            if (height >= 16) {
                const unsigned audio = height - 16;
                if (!controls.audio_pending && audio == 0) {
                    monitor_audio_mode = 0;
                    voice_link_enable(false);
                }
                if (audio == 1 || audio == 2 || audio == 3 || audio == 4 || audio == 5 ||
                    (!monitor_audio_mode && audio == 0)) controls.audio_pending = false;
                if (audio == 4 || audio == 5) voice_link_enable(false);
                if (controls.audio_state != audio) {
                    controls.audio_state = audio;
                    monitor_controls(&controls);
                    ESP_LOGI(TAG, "Monitor audio state %u", audio);
                }
            } else {
                if (!controls.pending && height == 0) {
                    monitor_wanted = false;
                    controls.selected = 0;
                }
                if ((monitor_wanted && (height == 1 || height == 2)) ||
                    (!monitor_wanted && (height == 0 || height == 6)) || (height >= 3 && height <= 5)) controls.pending = false;
                if (controls.state != height) {
                    controls.state = height;
                    if (height != 2) monitor_clear_picture();
                    monitor_controls(&controls);
                    ESP_LOGI(TAG, "Camera state %u", height);
                }
            }
        } else {
            if (!y && (width != last_width || height != last_height)) {
                fit_monitor(&view, width, height);
                monitor_clear_picture();
                last_width = width;
                last_height = height;
            }
            if (monitor_wanted) draw_scaled_band(&view, width, height, y, rows, false);
            expected_y = (y + rows) % height;
            if (!expected_y && ++frames == 1) ESP_LOGI(TAG, "Camera frame received (%ux%u)", width, height);
        }
        if (++expected == 0) break;
    }
    image_wait_hook = NULL;
    image_wait_context = NULL;
    voice_link_enable(false);
    voice_pointer_mode(false);
    mbedtls_gcm_free(&input_gcm);
    mbedtls_gcm_free(&gcm);
    close(fd);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 256, .rx_buffer_size = 512};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    ESP_ERROR_CHECK(xTaskCreate(usb_setup, "screen_pair", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(bsp_display_init());
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(bsp_display_panel(), false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(bsp_display_panel(), false, false));
    drawn = xSemaphoreCreateBinary();
    keys = xQueueCreate(16, sizeof(input_t));
    wifi_status = xEventGroupCreate();
    ESP_ERROR_CHECK(drawn && keys && wifi_status ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = on_color_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &callbacks, drawn));
    bsp_display_backlight(75);
    ESP_ERROR_CHECK(bsp_button_init(on_button, keys));
    ESP_ERROR_CHECK((size_t)(standby_pixels_end - standby_pixels) ==
                    LANDSCAPE_WIDTH * LANDSCAPE_HEIGHT * 2 ? ESP_OK : ESP_ERR_INVALID_SIZE);
    ESP_ERROR_CHECK((size_t)(portrait_pixels_end - portrait_pixels) ==
                    BSP_LCD_W * BSP_LCD_H * 2 ? ESP_OK : ESP_ERR_INVALID_SIZE);
    (void)bsp_battery_init();
    show_standby();
    vTaskDelay(pdMS_TO_TICKS(1300));
    connection_t cfg = {0};
    pairing_t pairing = {0};
    const bool configured = load_connection(&cfg) && cfg.ssid[0];
    const bool paired = load_pairing(&pairing);
    SemaphoreHandle_t audio_owner = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(audio_owner ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(radio_player_init(wifi_status, WIFI_UP, audio_owner));
    if (configured) {
        wifi_start(&cfg);
        esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
        time_config.start = false;
        ESP_ERROR_CHECK(esp_netif_sntp_init(&time_config));
        ESP_ERROR_CHECK(xTaskCreate(maintain_wifi, "screen_wifi", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    if (configured && paired) voice_link_start(pairing.host, pairing.key, wifi_status, WIFI_UP, audio_owner);
    memset(&cfg, 0, sizeof(cfg));
    launcher_page_t previous_page = (launcher_page_t)-1;
    radio_status_t previous_radio_status = RADIO_STOPPED;
    TickType_t radio_refreshed = 0, radio_header_refreshed = 0;
    EventBits_t previous_wifi = 0;
    for (;;) {
        if (launcher.page != previous_page) {
            radio_player_select(-1);
            voice_link_enable(false);
            xQueueReset(keys);
            bsp_display_backlight(75);
            set_portrait(launcher.page != PAGE_REMOTE);
            if (launcher.page == PAGE_HOME) {
                draw_home();
            } else if (launcher.page == PAGE_RADIO) {
                status_screen(0x0865);
                radio_player_volume(launcher.volume);
                radio_player_select(launcher.paused ? -1 : launcher.channel);
                draw_radio(RADIO_DRAW_FULL);
                previous_radio_status = radio_player_status();
            } else if (launcher.page == PAGE_MONITOR) {
                monitor_wanted = true;
                monitor_audio_mode = 0;
                voice_link_monitor_target(true);
                monitor_wait_screen(paired ? "正在连接摄像头" : "请先配对电脑");
            } else {
                voice_link_monitor_target(false);
                if (paired && configured) voice_link_enable(true);
                draw_remote_wait(paired, configured);
                previous_wifi = xEventGroupGetBits(wifi_status) & WIFI_UP;
            }
            previous_page = launcher.page;
        }

        if (launcher.page == PAGE_MONITOR) {
            if (paired && (xEventGroupGetBits(wifi_status) & WIFI_UP)) receive_monitor(&pairing);
            if (launcher.page != PAGE_MONITOR) continue;
            monitor_wait_screen(!paired ? "请先配对电脑" :
                                !(xEventGroupGetBits(wifi_status) & WIFI_UP) ? "等待无线网络" : "请先启动监控发送端");
            input_t waiting;
            if (xQueueReceive(keys, &waiting, pdMS_TO_TICKS(500)) == pdTRUE &&
                waiting.key == BSP_BTN_OK && waiting.event == BSP_BTN_LONG) launcher.page = PAGE_HOME;
            continue;
        }

        if (launcher.page == PAGE_REMOTE && paired && (xEventGroupGetBits(wifi_status) & WIFI_UP)) {
            receive_screen(&pairing);
            if (launcher.page != PAGE_REMOTE) continue;
            draw_remote_wait(paired, configured);
            input_t waiting;
            // A bounded reconnect delay still lets the user return home immediately.
            if (xQueueReceive(keys, &waiting, pdMS_TO_TICKS(1000)) == pdTRUE &&
                waiting.key == BSP_BTN_OK && waiting.event == BSP_BTN_LONG) launcher.page = PAGE_HOME;
            continue;
        }

        if (launcher.page == PAGE_REMOTE) {
            const EventBits_t linked = xEventGroupGetBits(wifi_status) & WIFI_UP;
            if (linked != previous_wifi) {
                previous_wifi = linked;
                draw_remote_wait(paired, configured);
            }
        } else if (launcher.page == PAGE_RADIO) {
            const TickType_t now = xTaskGetTickCount();
            unsigned flags = 0;
            if (radio_player_status() != previous_radio_status) {
                previous_radio_status = radio_player_status();
                flags |= RADIO_DRAW_STATS;
            }
            if (now - radio_refreshed >= pdMS_TO_TICKS(125)) {
                flags |= RADIO_DRAW_METER;
                radio_refreshed = now;
            }
            if (now - radio_header_refreshed >= pdMS_TO_TICKS(1000)) {
                flags |= RADIO_DRAW_HEADER | RADIO_DRAW_STATS;
                radio_header_refreshed = now;
            }
            if (flags) draw_radio(flags);
        }

        input_t input;
        if (xQueueReceive(keys, &input, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        launcher_input_t action;
        if (launcher.page == PAGE_RADIO && input.key == BSP_BTN_OK && input.event == BSP_BTN_DOUBLE) {
            action = LAUNCHER_HOME;
        } else if (input.event == BSP_BTN_CLICK) {
            action = input.key == BSP_BTN_UP ? LAUNCHER_UP :
                     input.key == BSP_BTN_DOWN ? LAUNCHER_DOWN : LAUNCHER_OK;
        } else if (input.event == BSP_BTN_LONG) {
            action = input.key == BSP_BTN_UP ? LAUNCHER_LOUDER :
                     input.key == BSP_BTN_DOWN ? LAUNCHER_QUIETER : LAUNCHER_BACK;
        } else continue;
        launcher_input(&launcher, action);
        if (launcher.page != previous_page) continue;
        if (launcher.page == PAGE_HOME) {
            draw_home();
        } else if (launcher.page == PAGE_RADIO) {
            radio_player_volume(launcher.volume);
            radio_player_select(launcher.paused ? -1 : launcher.channel);
            draw_radio(RADIO_DRAW_FULL);
        }
    }
}
