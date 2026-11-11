// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include "util.h"
#include "catalog.h"
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

Palette PAL;
lv_font_t *FONT_XS, *FONT_SM, *FONT_MD, *FONT_LG, *FONT_XL, *FONT_XXL, *FONT_HERO;
static void *font_data;
static size_t font_data_size;

typedef struct {
    const char *name;
    Palette p;
} Preset;

static const Preset PRESETS[] = {
    {"深空蓝 / Deep Space", {
        .bg = 0xFF0A0C12, .bg_bottom = 0xFF11141E,
        .surface = 0xFF12151F, .card = 0xFF1A1E2A, .card_soft = 0xFF161A24,
        .elevated = 0xFF232838, .text = 0xFFE6EDF5, .text2 = 0xFF9AA6B8, .text3 = 0xFF64748B,
        .accent = 0xFF4D8BFF, .accent2 = 0xFF00E5FF, .success = 0xFF32E082,
        .warning = 0xFFFFB84C, .danger = 0xFFFF6B6B,
        .border = 0xFF2A3042, .border_focus = 0xFF6EA2FF, .overlay = 0xCC000000,
        .is_light = false,
    }},
    {"纸白 / Paper White", {
        .bg = 0xFFF2F4F7, .bg_bottom = 0xFFE4E9F1,
        .surface = 0xFFFFFFFF, .card = 0xFFFFFFFF, .card_soft = 0xFFF7F9FC,
        .elevated = 0xFFFFFFFF, .text = 0xFF1A1C22, .text2 = 0xFF5E6777, .text3 = 0xFF9AA3B2,
        .accent = 0xFF3A76F0, .accent2 = 0xFF00B4D8, .success = 0xFF27AE60,
        .warning = 0xFFF5A742, .danger = 0xFFE85D5D,
        .border = 0xFFD8DEE8, .border_focus = 0xFF3A76F0, .overlay = 0x660B1020,
        .is_light = true,
    }},
    {"落日霓虹 / Sunset", {
        .bg = 0xFF0E0812, .bg_bottom = 0xFF1A0D1E,
        .surface = 0xFF1A101F, .card = 0xFF241526, .card_soft = 0xFF1C1121,
        .elevated = 0xFF32203A, .text = 0xFFF4ECF7, .text2 = 0xFFB9A2C4, .text3 = 0xFF7D6689,
        .accent = 0xFFFF6B9A, .accent2 = 0xFFFFB84C, .success = 0xFF32E082,
        .warning = 0xFFFFB84C, .danger = 0xFFFF6B6B,
        .border = 0xFF3A2442, .border_focus = 0xFFFF8FB2, .overlay = 0xCC000000,
        .is_light = false,
    }},
    {"森林复古 / Forest", {
        .bg = 0xFF0B120E, .bg_bottom = 0xFF101A14,
        .surface = 0xFF121A15, .card = 0xFF18231C, .card_soft = 0xFF141E18,
        .elevated = 0xFF24352A, .text = 0xFFE8F1E9, .text2 = 0xFF9DB3A2, .text3 = 0xFF647A6B,
        .accent = 0xFF5DD39E, .accent2 = 0xFFE8C468, .success = 0xFF5DD39E,
        .warning = 0xFFE8C468, .danger = 0xFFFF7070,
        .border = 0xFF2A3B30, .border_focus = 0xFF7BE5B4, .overlay = 0xCC000000,
        .is_light = false,
    }},
};

void theme_set_preset(int preset)
{
    int n = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
    if (preset < 0 || preset >= n) preset = 0;
    PAL = PRESETS[preset].p;
}

void theme_preview_colors(int preset, uint32_t *bg, uint32_t *card,
                          uint32_t *accent)
{
    int n = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
    if (preset < 0 || preset >= n) preset = 0;
    if (bg) *bg = PRESETS[preset].p.bg;
    if (card) *card = PRESETS[preset].p.card;
    if (accent) *accent = PRESETS[preset].p.accent;
}

const char *theme_preset_name(int preset)
{
    int n = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
    if (preset < 0 || preset >= n) preset = 0;
    return PRESETS[preset].name;
}

int theme_preset_count(void)
{
    return (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
}

#define MAX_SYMBOL_FONTS 16
static lv_font_t *g_symbol_fonts[MAX_SYMBOL_FONTS];
static int g_symbol_font_count;
static void *symbol_font_data;
static size_t symbol_font_size;

static lv_font_t *montserrat_for_px(int px)
{
    if (px <= 12) return (lv_font_t *)&lv_font_montserrat_12;
    if (px <= 14) return (lv_font_t *)&lv_font_montserrat_14;
    if (px <= 16) return (lv_font_t *)&lv_font_montserrat_16;
    if (px <= 20) return (lv_font_t *)&lv_font_montserrat_20;
    if (px <= 24) return (lv_font_t *)&lv_font_montserrat_24;
    if (px <= 28) return (lv_font_t *)&lv_font_montserrat_28;
    if (px <= 32) return (lv_font_t *)&lv_font_montserrat_32;
    if (px <= 40) return (lv_font_t *)&lv_font_montserrat_40;
    return (lv_font_t *)&lv_font_montserrat_48;
}

/* TaijiCJK is a compact CJK subset without geometric symbols.  Use a system
 * symbol font as a middle fallback so PlayStation's triangle and friends render
 * instead of showing missing-glyph boxes. */
static lv_font_t *make_symbol_font(int px)
{
    static const char *paths[] = {
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        NULL,
    };
    if (!symbol_font_data) {
        char *asset = asset_path("fonts/TaijiButtons.ttf");
        if (asset) {
            symbol_font_data = read_file(asset, &symbol_font_size);
            free(asset);
            if (symbol_font_data)
                fprintf(stderr, "[taiji] using bundled button symbol font\n");
        }
        for (int i = 0; !symbol_font_data && paths[i]; i++) {
            symbol_font_data = read_file(paths[i], &symbol_font_size);
            if (symbol_font_data)
                fprintf(stderr, "[taiji] using system symbol font: %s\n", paths[i]);
        }
    }
    if (!symbol_font_data) return NULL;
    return lv_tiny_ttf_create_data_ex(symbol_font_data, symbol_font_size, px,
                                      LV_FONT_KERNING_NONE, 2048);
}

static void destroy_symbol_fonts(void)
{
    for (int i = 0; i < g_symbol_font_count; i++)
        lv_tiny_ttf_destroy(g_symbol_fonts[i]);
    g_symbol_font_count = 0;
}

static lv_font_t *make_font(int px)
{
    if (!font_data) return (lv_font_t *)&lv_font_montserrat_16;
    lv_font_t *f = lv_tiny_ttf_create_data_ex(font_data, font_data_size, px,
                                              LV_FONT_KERNING_NONE, 2048);
    if (!f) return (lv_font_t *)&lv_font_montserrat_16;
    lv_font_t *symbols = make_symbol_font(px);
    if (symbols) {
        symbols->fallback = montserrat_for_px(px);
        f->fallback = symbols;
        if (g_symbol_font_count < MAX_SYMBOL_FONTS)
            g_symbol_fonts[g_symbol_font_count++] = symbols;
    } else {
        /* Keep LVGL symbol glyphs in proportion to the CJK text size. */
        f->fallback = montserrat_for_px(px);
    }
    return f;
}

static float g_font_scale = 0.0f;

static void destroy_font(lv_font_t *f)
{
    if (f && font_data && f != &lv_font_montserrat_16) {
        lv_tiny_ttf_destroy(f);
    }
}

void theme_set_scale(float scale)
{
    if (scale < 0.70f) scale = 0.70f;
    if (scale > 4.50f) scale = 4.50f;
    if (fabsf(scale - g_font_scale) < 0.01f && FONT_XS) return;
    g_font_scale = scale;

    destroy_symbol_fonts();
    destroy_font(FONT_XS);
    destroy_font(FONT_SM);
    destroy_font(FONT_MD);
    destroy_font(FONT_LG);
    destroy_font(FONT_XL);
    destroy_font(FONT_XXL);
    destroy_font(FONT_HERO);

    if (!font_data) {
        
        
        FONT_MD = (lv_font_t *)&lv_font_montserrat_16;
        FONT_LG = (lv_font_t *)&lv_font_montserrat_20;
        FONT_XL = (lv_font_t *)&lv_font_montserrat_28;
        FONT_XXL = (lv_font_t *)&lv_font_montserrat_32;
        FONT_HERO = (lv_font_t *)&lv_font_montserrat_48;
        return;
    }

    int base[] = {12, 14, 17, 21, 28, 38, 52};
    lv_font_t **out[] = {&FONT_XS, &FONT_SM, &FONT_MD, &FONT_LG, &FONT_XL, &FONT_XXL, &FONT_HERO};
    for (int i = 0; i < 7; i++) {
        int px = (int)lroundf((float)base[i] * scale);
        if (px < 9) px = 9;
        if (px > 240) px = 240;
        *out[i] = make_font(px);
    }
}

void theme_init(void)
{
    theme_set_preset(0);
    char *font_path = asset_path("fonts/TaijiCJK.ttf");
    size_t len = 0;
    font_data = read_file(font_path, &len);
    if (!font_data) {
        fprintf(stderr, "[taiji] font missing: %s (Chinese text will not render)\n", font_path);
    } else {
        font_data_size = len;
    }
    free(font_path);
    g_font_scale = 0.0f;
    FONT_XS = FONT_SM = FONT_MD = FONT_LG = FONT_XL = FONT_XXL = FONT_HERO = NULL;
    theme_set_scale(1.0f);
}

uint32_t platform_accent(const char *id)
{
    static const uint32_t colors[] = {
        0xFF4D8BFF, 0xFF00E5FF, 0xFF32E082, 0xFFFFB84C,
        0xFFFF6B9A, 0xFFB388FF, 0xFF64FFDA, 0xFFFF8A65,
    };
    uint32_t h = fnv1a(id ? id : "");
    return colors[h % (sizeof(colors) / sizeof(colors[0]))];
}
