#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MENU_NONE, MENU_DISPLAY, MENU_CONTROL, MENU_AUDIO, MENU_SYSTEM,
    MENU_FULLSCREEN, MENU_HORIZONTAL, MENU_VERTICAL, MENU_KEYBOARD,
    MENU_SPEAKER, MENU_TALK, MENU_STANDBY, MENU_HOME,
} screen_menu_item_t;

typedef struct {
    bool open;
    uint8_t category;
    uint8_t selected;
    bool submenu;
} screen_menu_t;

static const struct {
    screen_menu_item_t title;
    uint8_t count;
    screen_menu_item_t items[3];
} screen_menu_pages[] = {
    {MENU_DISPLAY, 1, {MENU_FULLSCREEN}},
    {MENU_CONTROL, 3, {MENU_HORIZONTAL, MENU_VERTICAL, MENU_KEYBOARD}},
    {MENU_AUDIO, 2, {MENU_SPEAKER, MENU_TALK}},
    {MENU_SYSTEM, 2, {MENU_HOME, MENU_STANDBY}},
};

static inline unsigned screen_menu_count(const screen_menu_t *menu)
{
    return menu->submenu ? screen_menu_pages[menu->category].count :
           sizeof(screen_menu_pages) / sizeof(screen_menu_pages[0]);
}

static inline screen_menu_item_t screen_menu_item(const screen_menu_t *menu, unsigned row)
{
    return menu->submenu ? screen_menu_pages[menu->category].items[row] : screen_menu_pages[row].title;
}

static inline void screen_menu_open(screen_menu_t *menu)
{
    *menu = (screen_menu_t){.open = true};
}

static inline void screen_menu_move(screen_menu_t *menu, int step)
{
    const int count = screen_menu_count(menu);
    menu->selected = (menu->selected + count + step) % count;
}

static inline void screen_menu_back(screen_menu_t *menu)
{
    if (menu->submenu) {
        menu->submenu = false;
        menu->selected = menu->category;
    } else {
        menu->open = false;
    }
}

static inline screen_menu_item_t screen_menu_choose(screen_menu_t *menu)
{
    const screen_menu_item_t item = screen_menu_item(menu, menu->selected);
    if (menu->submenu) {
        menu->open = false;
        return item;
    }
    menu->category = menu->selected;
    menu->selected = 0;
    menu->submenu = true;
    return MENU_NONE;
}
