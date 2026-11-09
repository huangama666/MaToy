#include "radio_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "radio_font.h"

static uint32_t next_codepoint(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t code = *p++;
    if (code >= 0xe0) {
        code = ((code & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
        p += 2;
    } else if (code >= 0xc0) {
        code = ((code & 31) << 6) | (*p++ & 63);
    }
    *text = (const char *)p;
    return code;
}

static const radio_glyph_t *glyph_for(uint32_t code)
{
    unsigned low = 0, high = sizeof(radio_glyphs) / sizeof(radio_glyphs[0]);
    while (low < high) {
        const unsigned middle = low + (high - low) / 2;
        if (radio_glyphs[middle].codepoint < code) low = middle + 1;
        else high = middle;
    }
    return low < sizeof(radio_glyphs) / sizeof(radio_glyphs[0]) &&
           radio_glyphs[low].codepoint == code ? &radio_glyphs[low] : NULL;
}

static unsigned character_width(uint32_t code, unsigned size)
{
    return code < 0x2e80 ? size / 2 : size;
}

static void put_pixel(uint8_t *line, int x, uint16_t color)
{
    if (x < 0 || x >= RADIO_UI_WIDTH) return;
    line[2 * x] = color >> 8;
    line[2 * x + 1] = color;
}

static void text_line(uint8_t *line, const char *text, int x, int y, unsigned row,
                      unsigned size, uint16_t color, bool wrap)
{
    const int last_line = y + size + 4;
    while (*text && x < RADIO_UI_WIDTH) {
        const uint32_t code = next_codepoint(&text);
        const unsigned width = character_width(code, size);
        if (wrap && x + (int)width > RADIO_UI_WIDTH - 12) { x = 12; y += size + 4; }
        if (wrap && y > last_line) break;
        const int glyph_row = (int)row - y;
        const radio_glyph_t *glyph = glyph_for(code);
        if (glyph && glyph_row >= 0 && glyph_row < (int)size) {
            const uint32_t bits = size == 20 ? glyph->large[glyph_row] :
                                  glyph->small[size == 32 ? glyph_row / 2 : glyph_row];
            for (unsigned col = 0; col < width; ++col) {
                const unsigned bit = size == 32 ? col / 2 : col;
                if (bits & (0x80000000u >> bit)) put_pixel(line, x + col, color);
            }
        }
        x += width;
    }
}

static uint16_t background(unsigned y)
{
    return (uint16_t)((4u << 11) | ((10u + y / 40) << 5) | (15u + y / 50));
}

static void fill_line(uint8_t *line, uint16_t color)
{
    for (unsigned x = 0; x < RADIO_UI_WIDTH; ++x) put_pixel(line, x, color);
}

static void header(const radio_ui_t *ui, uint8_t *line, unsigned y)
{
    text_line(line, "RADIO", 12, 12, y, 16, 0xff30, false);
    char clock[12], battery[12];
    const unsigned seconds = ui->elapsed_seconds % 6000;
    snprintf(clock, sizeof(clock), "%02u:%02u", seconds / 60, seconds % 60);
    snprintf(battery, sizeof(battery), ui->battery < 0 ? "--%%" : "%d%%", ui->battery);
    text_line(line, clock, 77, 5, y, 32, 0xffff, false);
    text_line(line, battery, 196, 15, y, 16, 0xffff, false);
    for (unsigned bar = 0; bar < 4; ++bar) {
        if (y < 27 - bar * 3 || y >= 28) continue;
        for (unsigned x = 0; x < 3; ++x) put_pixel(line, 173 + bar * 5 + x, (int)bar < ui->signal ? 0x9fff : 0x39cf);
    }
    if (y == 44) for (unsigned x = 12; x < 228; ++x) put_pixel(line, x, 0x4a97);
}

static void meter(const radio_ui_t *ui, uint8_t *line, unsigned y)
{
    static const uint16_t colors[] = {0xcfff, 0x9fff, 0x5fff, 0x3e9e, 0xffff, 0xfdb7, 0xff30, 0xe7ff};
    for (unsigned x = 12; x < 228; ++x) put_pixel(line, x, (uint16_t)(0x3297 + ((y - 218) / 12 << 5)));
    for (unsigned i = 0; i < RADIO_UI_BARS; ++i) {
        const unsigned height = 4 + ui->levels[i] * 36 / 255;
        const unsigned top = 269 - height;
        if (y < top || y >= 269) continue;
        const unsigned edge = y - top < 3 ? 3 - (y - top) : 269 - y <= 3 ? 4 - (269 - y) : 0;
        for (unsigned x = edge; x < 9 - edge; ++x) put_pixel(line, 20 + i * 12 + x, colors[i % 8]);
    }
}

static const radio_region_t *browser_region(const launcher_t *app)
{
    return app->radio_region == 0 ? NULL : &radio_regions[app->radio_region - 1];
}

static const char *browser_label(const launcher_t *app, unsigned index)
{
    if (app->radio_browser == RADIO_BROWSE_REGIONS) {
        if (index == 0) return "返回首页";
        if (index == 1) return "音量调节";
        if (index == 2) return "全部电台";
        return radio_regions[index - 3].name;
    }
    const radio_region_t *region = browser_region(app);
    return radio_stations[(region ? region->first : 0) + index].name;
}

static void draw_browser(const radio_ui_t *ui, uint8_t *line, radio_ui_flush_t flush)
{
    const launcher_t *app = ui->app;
    const unsigned count = launcher_browser_count(app);
    const unsigned first = app->radio_selection / 6 * 6;
    for (unsigned y = 0; y < RADIO_UI_HEIGHT; ++y) {
        fill_line(line, background(y));
        text_line(line, app->radio_browser == RADIO_BROWSE_REGIONS ? "电台菜单" : "选择频道",
                  12, 12, y, 20, 0xff30, false);
        char page[20];
        snprintf(page, sizeof(page), "%u/%u", app->radio_selection + 1, count);
        text_line(line, page, 172, 16, y, 16, 0xffff, false);
        for (unsigned row = 0; row < 6 && first + row < count; ++row) {
            const unsigned top = 54 + row * 36;
            const bool selected = first + row == app->radio_selection;
            if (selected && y >= top && y < top + 30) {
                for (unsigned x = 8; x < 232; ++x) put_pixel(line, x, 0x3af6);
            }
            text_line(line, browser_label(app, first + row), 14, top + 6, y, 16,
                      selected ? 0xff30 : 0xffff, false);
        }
        text_line(line, "上下选择 短按OK进入", 12, 280, y, 16, 0xffff, false);
        text_line(line, "长按上下翻页 长按OK返回", 12, 302, y, 16, 0xc73f, false);
        flush(0, y, RADIO_UI_WIDTH, 1);
    }
}

static void draw_volume(const radio_ui_t *ui, uint8_t *line, radio_ui_flush_t flush)
{
    char value[16];
    snprintf(value, sizeof(value), "%u%%", ui->app->volume);
    for (unsigned y = 0; y < RADIO_UI_HEIGHT; ++y) {
        fill_line(line, background(y));
        text_line(line, "音量调节", 12, 18, y, 20, 0xff30, false);
        text_line(line, value, (RADIO_UI_WIDTH - strlen(value) * 16) / 2, 92, y, 32, 0xffff, false);
        if (y >= 146 && y < 160) {
            for (unsigned x = 16; x < 224; ++x) {
                put_pixel(line, x, x - 16 < ui->app->volume * 208 / 90 ? 0xff30 : 0x3af6);
            }
        }
        if ((y >= 182 && y < 218) || (y >= 230 && y < 266)) {
            for (unsigned x = 16; x < 224; ++x) put_pixel(line, x, 0x3af6);
        }
        text_line(line, "上键增大", 88, 192, y, 16, 0xffff, false);
        text_line(line, "下键减小", 88, 240, y, 16, 0xffff, false);
        text_line(line, "短按OK返回", 12, 284, y, 16, 0xffff, false);
        text_line(line, "长按OK也可返回", 12, 306, y, 16, 0xc73f, false);
        flush(0, y, RADIO_UI_WIDTH, 1);
    }
}

void radio_ui_draw(const radio_ui_t *ui, unsigned flags, uint8_t *line, radio_ui_flush_t flush)
{
    if (ui->app->radio_browser == RADIO_BROWSE_VOLUME) {
        if (flags & RADIO_DRAW_FULL) draw_volume(ui, line, flush);
        return;
    }
    if (ui->app->radio_browser != RADIO_BROWSE_CLOSED) {
        if (flags & RADIO_DRAW_FULL) draw_browser(ui, line, flush);
        return;
    }
    const radio_station_t *station = &radio_stations[ui->app->channel];
    char channel[32], title[160], level[16], audio[48], bitrate[20];
    snprintf(channel, sizeof(channel), "CH %03u / %03u", ui->app->channel + 1, RADIO_CHANNEL_COUNT);
    snprintf(title, sizeof(title), "%s%s%s", station->name, station->frequency[0] ? " " : "", station->frequency);
    snprintf(level, sizeof(level), "%u%%", ui->app->volume);
    if (ui->sample_rate) snprintf(audio, sizeof(audio), "%lu.%lu kHz · %uch",
                                 (unsigned long)(ui->sample_rate / 1000), (unsigned long)(ui->sample_rate % 1000 / 100), ui->channels);
    else snprintf(audio, sizeof(audio), "-- kHz · --ch");
    snprintf(bitrate, sizeof(bitrate), ui->bitrate ? "%lukbps" : "--kbps", (unsigned long)(ui->bitrate / 1000));

    for (unsigned y = 0; y < RADIO_UI_HEIGHT; ++y) {
        const bool full = flags & RADIO_DRAW_FULL;
        const bool draw_header = full || ((flags & RADIO_DRAW_HEADER) && y <= 44);
        const bool draw_stats = full || ((flags & RADIO_DRAW_STATS) && y >= 132 && y < 212);
        const bool draw_meter = full || ((flags & RADIO_DRAW_METER) && y >= 218 && y < 276);
        if (!draw_header && !draw_stats && !draw_meter) continue;
        fill_line(line, background(y));
        if (draw_header) header(ui, line, y);
        if (full) {
            text_line(line, channel, 12, 54, y, 16, 0xff30, false);
            text_line(line, title, 12, 82, y, 20, 0xffff, true);
            text_line(line, "上下切台 短按OK暂停", 12, 282, y, 16, 0xffff, false);
            text_line(line, "长按OK菜单 双按OK首页", 12, 304, y, 16, 0xc73f, false);
        }
        if (draw_stats) {
            text_line(line, ui->app->paused ? "已暂停" : ui->status, 12, 138, y, 16, 0xc73f, false);
            text_line(line, bitrate, 164, 138, y, 16, 0xc73f, false);
            text_line(line, "音量", 12, 166, y, 16, 0xffff, false);
            text_line(line, level, 78, 166, y, 20, 0xffff, false);
            text_line(line, "长按上下", 164, 166, y, 16, 0xff30, false);
            text_line(line, "采样", 12, 190, y, 16, 0xffff, false);
            text_line(line, audio, 70, 190, y, 16, 0xffff, false);
        }
        if (draw_meter && y >= 218 && y < 276) meter(ui, line, y);
        flush(0, y, RADIO_UI_WIDTH, 1);
    }
}
