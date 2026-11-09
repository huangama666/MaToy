#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "radio_catalog.h"

typedef enum { PAGE_HOME, PAGE_RADIO, PAGE_REMOTE, PAGE_MONITOR } launcher_page_t;
typedef enum {
    LAUNCHER_UP, LAUNCHER_DOWN, LAUNCHER_OK, LAUNCHER_BACK,
    LAUNCHER_LOUDER, LAUNCHER_QUIETER, LAUNCHER_HOME,
} launcher_input_t;
typedef enum { RADIO_BROWSE_CLOSED, RADIO_BROWSE_REGIONS, RADIO_BROWSE_STATIONS, RADIO_BROWSE_VOLUME } radio_browser_t;

typedef struct {
    launcher_page_t page;
    uint8_t home_selection, volume;
    uint16_t channel, radio_selection, radio_region;
    radio_browser_t radio_browser;
    bool paused;
} launcher_t;

#define LAUNCHER_INITIAL_STATE {.page = PAGE_HOME, .volume = 55, .channel = RADIO_DEFAULT_STATION}

static inline unsigned launcher_browser_count(const launcher_t *app)
{
    if (app->radio_browser == RADIO_BROWSE_REGIONS) return RADIO_REGION_COUNT + 3;
    return app->radio_region ? radio_regions[app->radio_region - 1].count : RADIO_CHANNEL_COUNT;
}

static inline void launcher_radio_browser(launcher_t *app, launcher_input_t input)
{
    if (app->radio_browser == RADIO_BROWSE_VOLUME) {
        if (input == LAUNCHER_OK || input == LAUNCHER_BACK) {
            app->radio_browser = RADIO_BROWSE_REGIONS;
            app->radio_selection = 1;
        } else if (input == LAUNCHER_UP || input == LAUNCHER_LOUDER) {
            app->volume = app->volume >= 85 ? 90 : app->volume + 5;
        } else if (input == LAUNCHER_DOWN || input == LAUNCHER_QUIETER) {
            app->volume = app->volume <= 5 ? 0 : app->volume - 5;
        }
        return;
    }
    if (input == LAUNCHER_BACK) {
        app->radio_browser = app->radio_browser == RADIO_BROWSE_STATIONS ? RADIO_BROWSE_REGIONS : RADIO_BROWSE_CLOSED;
        app->radio_selection = 0;
        return;
    }
    if (input == LAUNCHER_OK) {
        if (app->radio_browser == RADIO_BROWSE_REGIONS) {
            if (app->radio_selection == 0) { app->page = PAGE_HOME; return; }
            if (app->radio_selection == 1) { app->radio_browser = RADIO_BROWSE_VOLUME; return; }
            app->radio_region = app->radio_selection - 2;
            app->radio_browser = RADIO_BROWSE_STATIONS;
            app->radio_selection = 0;
        } else {
            app->channel = (app->radio_region ? radio_regions[app->radio_region - 1].first : 0) + app->radio_selection;
            app->radio_browser = RADIO_BROWSE_CLOSED;
            app->paused = false;
        }
        return;
    }
    const int count = launcher_browser_count(app);
    int step = input == LAUNCHER_UP ? -1 : input == LAUNCHER_DOWN ? 1 :
               input == LAUNCHER_LOUDER ? -6 : 6;
    if (step == 6 || step == -6) {
        int next = (int)app->radio_selection + step;
        app->radio_selection = next < 0 ? 0 : next >= count ? count - 1 : next;
    } else {
        app->radio_selection = (app->radio_selection + count + step) % count;
    }
}

static inline void launcher_input(launcher_t *app, launcher_input_t input)
{
    if (input == LAUNCHER_HOME) {
        app->page = PAGE_HOME;
        app->radio_browser = RADIO_BROWSE_CLOSED;
        return;
    }
    if (app->page == PAGE_HOME) {
        if (input == LAUNCHER_UP || input == LAUNCHER_DOWN) {
            app->home_selection = (app->home_selection + (input == LAUNCHER_UP ? 2 : 1)) % 3;
        }
        if (input == LAUNCHER_OK) {
            app->page = app->home_selection == 0 ? PAGE_RADIO :
                        app->home_selection == 1 ? PAGE_REMOTE : PAGE_MONITOR;
            app->paused = false;
            app->radio_browser = RADIO_BROWSE_CLOSED;
        }
    } else if (app->page == PAGE_RADIO) {
        if (app->radio_browser != RADIO_BROWSE_CLOSED) { launcher_radio_browser(app, input); return; }
        switch (input) {
        case LAUNCHER_UP:
            app->channel = (app->channel + RADIO_CHANNEL_COUNT - 1) % RADIO_CHANNEL_COUNT;
            app->paused = false;
            break;
        case LAUNCHER_DOWN:
            app->channel = (app->channel + 1) % RADIO_CHANNEL_COUNT;
            app->paused = false;
            break;
        case LAUNCHER_OK:
            app->paused = !app->paused;
            break;
        case LAUNCHER_BACK:
            app->radio_browser = RADIO_BROWSE_REGIONS;
            app->radio_selection = 0;
            break;
        case LAUNCHER_LOUDER:
            app->volume = app->volume >= 85 ? 90 : app->volume + 5;
            break;
        case LAUNCHER_QUIETER:
            app->volume = app->volume <= 5 ? 0 : app->volume - 5;
            break;
        default:
            break;
        }
    } else if (input == LAUNCHER_BACK) {
        app->page = PAGE_HOME;
    }
}
