// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "theme.h"
#include "util.h"
#include "external_osk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IME_KEY_TEXT = 0,
    IME_KEY_BACKSPACE,
    IME_KEY_SHIFT,
    IME_KEY_ENTER,
    IME_KEY_HIDE,
    IME_KEY_SPACE
};

typedef struct {
    App *app;
    int index;
} ImeKey;

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *label;
    int action;
    char text[8];
    bool accent;
    int row;
    int col;
} ImeKeyInfo;

static ImeKeyInfo g_keys[80];
static int g_key_count;
static int g_focus;
static int g_row_counts[5];
static int g_build_row;
static int g_build_col;

static lv_obj_t *g_letter_labels[32];
static char g_letter_chars[32][8];
static int g_letter_count;

static void ime_update_shift(App *app)
{
    for (int i = 0; i < g_letter_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.7s", g_letter_chars[i]);
        if (app->ime_shift) {
            for (char *p = buf; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            }
        }
        lv_label_set_text(g_letter_labels[i], buf);
    }
}

static void ime_update_focus(App *app)
{
    (void)app;
    for (int i = 0; i < g_key_count; i++) {
        ImeKeyInfo *k = &g_keys[i];
        bool focused = i == g_focus;
        lv_obj_set_style_bg_opa(k->btn, focused ? 95 : (k->accent ? 55 : 210), 0);
        lv_obj_set_style_border_width(k->btn, 2, 0);
        lv_obj_set_style_border_color(k->btn,
            lv_color_hex(focused ? PAL.accent2 : (k->accent ? PAL.accent : PAL.border)), 0);
        lv_obj_set_style_border_opa(k->btn,
            focused ? 255 : (k->accent ? 220 : 90), 0);
        lv_obj_set_style_text_color(k->label, lv_color_hex(PAL.text), 0);
    }
}

static void ime_activate(App *app)
{
    if (g_focus < 0 || g_focus >= g_key_count) return;
    ImeKeyInfo *k = &g_keys[g_focus];
    switch (k->action) {
        case IME_KEY_SHIFT:
            app->ime_shift = !app->ime_shift;
            ime_update_shift(app);
            ime_update_focus(app);
            break;
        case IME_KEY_BACKSPACE:
            app_input(app, IN_BACKSPACE, NULL);
            break;
        case IME_KEY_ENTER: {
            bool old = app->ime_handling;
            app->ime_handling = true;
            app_input(app, IN_CONFIRM, NULL);
            app->ime_handling = old;
            break;
        }
        case IME_KEY_SPACE:
            app_input(app, IN_TEXT, " ");
            break;
        case IME_KEY_HIDE:
            ui_ime_hide(app);
            break;
        default: {
            char text[8];
            snprintf(text, sizeof(text), "%s", k->text);
            if (app->ime_shift) {
                for (char *p = text; *p; p++) {
                    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
                }
            }
            app_input(app, IN_TEXT, text);
            break;
        }
    }
    audio_play(SND_NAV);
}

static void ime_key_cb(lv_event_t *e)
{
    ImeKey *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    g_focus = ctx->index;
    ime_update_focus(app);
    ime_activate(app);
}

static void ime_key_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void set_row(int row)
{
    g_build_row = row;
    g_build_col = 0;
}

static lv_obj_t *add_key(App *app, lv_obj_t *parent, int x, int y, int w, int h,
                         const char *label, const char *text, int action,
                         bool accent, bool letter)
{
    int index = g_key_count;
    if (index >= (int)(sizeof(g_keys) / sizeof(g_keys[0]))) return NULL;
    lv_obj_t *btn = ui_rect(parent, x, y, w, h, 8,
                            accent ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(btn, accent ? 55 : 210, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent ? PAL.accent : PAL.border), 0);
    lv_obj_set_style_border_opa(btn, accent ? 220 : 90, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 3);
    int lh = lv_font_get_line_height(FONT_XS);
    lv_obj_t *lbl = ui_label_center(btn, label, FONT_XS, PAL.text,
                                    0, (h - lh) / 2, w, lh + 2);
    if (letter && g_letter_count < 32) {
        g_letter_labels[g_letter_count] = lbl;
        snprintf(g_letter_chars[g_letter_count], sizeof(g_letter_chars[0]),
                 "%s", text ? text : "");
        g_letter_count++;
    }
    ImeKeyInfo *ki = &g_keys[g_key_count++];
    ki->btn = btn;
    ki->label = lbl;
    ki->action = action;
    snprintf(ki->text, sizeof(ki->text), "%s", text ? text : "");
    ki->accent = accent;
    ki->row = g_build_row;
    ki->col = g_build_col++;
    if (g_build_row >= 0 && g_build_row < 5) g_row_counts[g_build_row]++;

    ImeKey *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = index;
    lv_obj_add_event_cb(btn, ime_key_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(btn, ime_key_free, LV_EVENT_DELETE, ctx);
    return btn;
}

static void add_text_row(App *app, lv_obj_t *panel, int x, int y, int row_h,
                         int unit, int gap, const char *letters)
{
    int n = (int)strlen(letters);
    for (int i = 0; i < n; i++) {
        char s[2] = {letters[i], 0};
        add_key(app, panel, x + i * (unit + gap), y, unit, row_h, s, s,
                IME_KEY_TEXT, false, true);
    }
}

void ui_ime_show(App *app)
{
    if (!app) return;
    if (external_osk_available()) {
        external_osk_show(app);
        return;
    }
    if (!app->ui.modal) return;
    if (app->ui.keyboard) {
        lv_obj_remove_flag(app->ui.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(app->ui.keyboard);
        app->ime_visible = true;
        ime_update_focus(app);
        return;
    }

    UiLayout *L = &app->layout;
    int kw = ui_min(L->w - L->pad * 2, L->w * 82 / 100);
    int kh = L->h * 42 / 100;
    if (kh < 120) kh = 120;
    int kx = (L->w - kw) / 2;
    int ky = L->h - kh - L->pad;
    lv_obj_t *panel = ui_rect(app->ui.modal, kx, ky, kw, kh, 18, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 248, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(panel, 110, 0);
    lv_obj_move_foreground(panel);

    int margin = ui_clamp(kw / 70, 6, 12);
    int gap = ui_clamp(kw / 180, 3, 7);
    int row_h = (kh - margin * 2 - gap * 4) / 5;
    if (row_h < 14) row_h = 14;
    int unit = (kw - margin * 2 - gap * 9) / 10;
    if (unit < 16) unit = 16;
    int x = margin;
    int y = margin;

    app->ime_shift = false;
    g_key_count = 0;
    g_focus = 0;
    g_letter_count = 0;
    memset(g_row_counts, 0, sizeof(g_row_counts));

    set_row(0);
    add_text_row(app, panel, x, y, row_h, unit, gap, "1234567890");
    y += row_h + gap;
    set_row(1);
    add_text_row(app, panel, x, y, row_h, unit, gap, "qwertyuiop");
    y += row_h + gap;
    set_row(2);
    add_text_row(app, panel, x, y, row_h, unit, gap, "asdfghjkl");
    add_key(app, panel, x + 9 * (unit + gap), y, unit, row_h, LV_SYMBOL_BACKSPACE,
            "", IME_KEY_BACKSPACE, false, false);
    y += row_h + gap;

    set_row(3);
    add_key(app, panel, x, y, unit * 3 / 2, row_h, "Shift", "",
            IME_KEY_SHIFT, app->ime_shift, false);
    add_text_row(app, panel, x + unit * 3 / 2 + gap, y, row_h, unit, gap, "zxcvbnm");
    add_key(app, panel,
            x + unit * 3 / 2 + gap + 7 * (unit + gap), y,
            unit * 3 / 2, row_h, "Enter", "", IME_KEY_ENTER, true, false);
    y += row_h + gap;

    set_row(4);
    add_key(app, panel, x, y, unit, row_h, ",", ",", IME_KEY_TEXT, false, false);
    add_key(app, panel, x + unit + gap, y, unit, row_h, "/", "/", IME_KEY_TEXT, false, false);
    add_key(app, panel, x + unit * 2 + gap * 2, y, unit * 5 + gap * 4, row_h, "空格", " ",
            IME_KEY_SPACE, false, false);
    add_key(app, panel, x + unit * 7 + gap * 6, y, unit, row_h, ".", ".",
            IME_KEY_TEXT, false, false);
    add_key(app, panel,
            x + unit * 8 + gap * 7, y, unit * 2 + gap, row_h, "收起", "",
            IME_KEY_HIDE, false, false);

    app->ui.keyboard = panel;
    app->ime_visible = true;
    ime_update_shift(app);
    ime_update_focus(app);
}

void ui_ime_hide(App *app)
{
    if (!app) return;
    external_osk_hide(app);
    app->ime_visible = false;
    if (app->ui.keyboard) lv_obj_add_flag(app->ui.keyboard, LV_OBJ_FLAG_HIDDEN);
}

void ui_ime_raise(App *app)
{
    if (app && app->ui.keyboard && app->ime_visible)
        lv_obj_move_foreground(app->ui.keyboard);
}

void ui_ime_input(App *app, InputAction action)
{
    if (!app || g_key_count <= 0) return;
    if (action == IN_CANCEL || action == IN_B || action == IN_PAD_CANCEL) {
        ui_ime_hide(app);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A || action == IN_PAD_CONFIRM) {
        ime_activate(app);
        return;
    }
    int cur = g_focus;
    if (cur < 0 || cur >= g_key_count) cur = 0;
    ImeKeyInfo *c = &g_keys[cur];
    int best = -1;
    if (action == IN_LEFT || action == IN_RIGHT) {
        for (int i = 0; i < g_key_count; i++) {
            ImeKeyInfo *k = &g_keys[i];
            if (k->row != c->row) continue;
            if (action == IN_LEFT && k->col < c->col &&
                (best < 0 || k->col > g_keys[best].col)) best = i;
            if (action == IN_RIGHT && k->col > c->col &&
                (best < 0 || k->col < g_keys[best].col)) best = i;
        }
    } else if (action == IN_UP || action == IN_DOWN) {
        int target_row = c->row + (action == IN_UP ? -1 : 1);
        if (target_row >= 0 && target_row < 5 && g_row_counts[target_row] > 0) {
            int target_col = c->col;
            if (target_col >= g_row_counts[target_row])
                target_col = g_row_counts[target_row] - 1;
            for (int i = 0; i < g_key_count; i++) {
                ImeKeyInfo *k = &g_keys[i];
                if (k->row != target_row) continue;
                if (k->col == target_col) { best = i; break; }
            }
            if (best < 0) {
                for (int i = 0; i < g_key_count; i++) {
                    ImeKeyInfo *k = &g_keys[i];
                    if (k->row != target_row) continue;
                    if (best < 0 ||
                        abs(k->col - target_col) < abs(g_keys[best].col - target_col))
                        best = i;
                }
            }
        }
    }
    if (best >= 0) {
        g_focus = best;
        ime_update_focus(app);
        audio_play(SND_NAV);
    }
}
