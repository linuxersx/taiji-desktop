// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_THEME_H
#define TAIJI_THEME_H

#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

typedef struct {
    uint32_t bg;
    uint32_t bg_bottom;
    uint32_t surface;
    uint32_t card;
    uint32_t card_soft;
    uint32_t elevated;
    uint32_t text;
    uint32_t text2;
    uint32_t text3;
    uint32_t accent;
    uint32_t accent2;
    uint32_t success;
    uint32_t warning;
    uint32_t danger;
    uint32_t border;
    uint32_t border_focus;
    uint32_t overlay;
    bool is_light;
} Palette;

extern Palette PAL;

extern lv_font_t *FONT_XS;
extern lv_font_t *FONT_SM;
extern lv_font_t *FONT_MD;
extern lv_font_t *FONT_LG;
extern lv_font_t *FONT_XL;
extern lv_font_t *FONT_XXL;
extern lv_font_t *FONT_HERO;

void theme_init(void);
void theme_set_scale(float scale);
void theme_set_preset(int preset);
const char *theme_preset_name(int preset);
int theme_preset_count(void);
uint32_t platform_accent(const char *platform_id);

#endif
