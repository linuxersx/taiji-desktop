// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    lv_obj_t *label;
    const char *name;
    int type; /* 0 button, 1 axis, 2 dpad */
    int code;
    bool down;
} PadKey;

#define PAD_MAX_KEYS 24
static PadKey g_keys[PAD_MAX_KEYS];
static int g_key_count;
static lv_obj_t *g_status;
static lv_obj_t *g_dpad_label;
static char g_last_pressed[32];

static lv_obj_t *pad_label(App *app, lv_obj_t *panel, int img_x, int img_y,
                           int img_w, int img_h, int rx, int ry,
                           const char *text)
{
    (void)app;
    int w = ui_clamp(img_w * 8 / 100, 36, 72);
    int h = ui_clamp(img_h * 6 / 100, 20, 32);
    int x = img_x + img_w * rx / 1000 - w / 2;
    int y = img_y + img_h * ry / 1000 - h / 2;
    lv_obj_t *l = ui_label_center(panel, text, FONT_XS, PAL.text2, x, y, w, h);
    lv_obj_set_style_bg_color(l, lv_color_hex(PAL.accent), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(l, 8, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    return l;
}

static void add_key(lv_obj_t *label, const char *name, int type, int code)
{
    if (g_key_count >= PAD_MAX_KEYS) return;
    PadKey *k = &g_keys[g_key_count++];
    k->label = label;
    k->name = name;
    k->type = type;
    k->code = code;
    k->down = false;
}

static bool key_down(App *app, const PadKey *k, const char **name)
{
    if (!app->backend.controller) return false;
    if (k->type == 1) {
        int v = backend_controller_axis(&app->backend, k->code);
        return v > 16000 || v < -16000;
    }
    if (k->type == 100) {
        bool up = backend_controller_button(&app->backend, SDL_CONTROLLER_BUTTON_DPAD_UP);
        bool down = backend_controller_button(&app->backend, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        bool left = backend_controller_button(&app->backend, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        bool right = backend_controller_button(&app->backend, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        if (up) { if (name) *name = "D-Pad ↑"; return true; }
        if (down) { if (name) *name = "D-Pad ↓"; return true; }
        if (left) { if (name) *name = "D-Pad ←"; return true; }
        if (right) { if (name) *name = "D-Pad →"; return true; }
        return false;
    }
    return backend_controller_button(&app->backend, k->code);
}

void ui_gamepad_poll(App *app)
{
    if (!app || app->modal != MODAL_GAMEPAD) return;
    if (!app->backend.controller) {
        for (int i = 0; i < g_key_count; i++) {
            PadKey *k = &g_keys[i];
            if (k->down) {
                k->down = false;
                lv_obj_set_style_bg_opa(k->label, LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_color(k->label, lv_color_hex(PAL.text2), 0);
            }
        }
        if (g_status) lv_label_set_text(g_status, tr("未连接手柄"));
        return;
    }
    bool any = false;
    for (int i = 0; i < g_key_count; i++) {
        PadKey *k = &g_keys[i];
        const char *name = k->name;
        bool down = key_down(app, k, &name);
        if (down != k->down) {
            k->down = down;
            lv_obj_set_style_bg_opa(k->label, down ? 220 : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(k->label,
                lv_color_hex(down ? 0xFFFFFFFF : PAL.text2), 0);
        }
        if (down) {
            any = true;
            snprintf(g_last_pressed, sizeof(g_last_pressed), "%s", name ? name : "");
        }
    }
    if (g_status) {
        if (any) lv_label_set_text(g_status, g_last_pressed);
        else lv_label_set_text(g_status, tr("按下手柄按键"));
    }
}

void ui_build_modal_gamepad(App *app)
{
    g_key_count = 0;
    g_status = NULL;
    g_dpad_label = NULL;
    g_last_pressed[0] = '\0';
    lv_obj_t *panel = ui_modal_panel(app, 900, 620);
    ui_label(panel, "手柄测试", FONT_XL, PAL.text, 30, 22, 600, 36);
    ui_label(panel, "按下手柄按键，图片上会标记；Xbox 布局", FONT_XS, PAL.text2,
             30, 62, 720, 20);
    lv_obj_t *line = ui_rect(panel, 30, 92, 840, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);

    int ix = 60, iy = 108, iw = 780, ih = 450;
    char *path = asset_path("images/gamepad_test.png");
    if (path && file_exists(path)) {
        lv_obj_t *img = ui_image(panel, path, ix, iy, iw, ih, 0, false);
        lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
    }
    free(path);

    g_dpad_label = pad_label(app, panel, ix, iy, iw, ih, 200, 495, "D-Pad");
    add_key(g_dpad_label, "D-Pad", 100, 0);

    lv_obj_t *l_lb = pad_label(app, panel, ix, iy, iw, ih, 322, 207, "LB");
    add_key(l_lb, "LB", 0, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    lv_obj_t *l_rb = pad_label(app, panel, ix, iy, iw, ih, 678, 207, "RB");
    add_key(l_rb, "RB", 0, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    lv_obj_t *l_lt = pad_label(app, panel, ix, iy, iw, ih, 322, 90, "LT");
    add_key(l_lt, "LT", 1, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    lv_obj_t *l_rt = pad_label(app, panel, ix, iy, iw, ih, 678, 90, "RT");
    add_key(l_rt, "RT", 1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    lv_obj_t *l_a = pad_label(app, panel, ix, iy, iw, ih, 750, 395, "A");
    add_key(l_a, "A", 0, SDL_CONTROLLER_BUTTON_A);
    lv_obj_t *l_b = pad_label(app, panel, ix, iy, iw, ih, 817, 510, "B");
    add_key(l_b, "B", 0, SDL_CONTROLLER_BUTTON_B);
    lv_obj_t *l_x = pad_label(app, panel, ix, iy, iw, ih, 683, 510, "X");
    add_key(l_x, "X", 0, SDL_CONTROLLER_BUTTON_X);
    lv_obj_t *l_y = pad_label(app, panel, ix, iy, iw, ih, 750, 625, "Y");
    add_key(l_y, "Y", 0, SDL_CONTROLLER_BUTTON_Y);

    bool compact = app->layout.w < 600;
    if (!compact) {
        lv_obj_t *l_l3 = pad_label(app, panel, ix, iy, iw, ih, 350, 587, "L3");
        add_key(l_l3, "L3", 0, SDL_CONTROLLER_BUTTON_LEFTSTICK);
        lv_obj_t *l_r3 = pad_label(app, panel, ix, iy, iw, ih, 670, 673, "R3");
        add_key(l_r3, "R3", 0, SDL_CONTROLLER_BUTTON_RIGHTSTICK);

        lv_obj_t *l_back = pad_label(app, panel, ix, iy, iw, ih, 405, 525, "Back");
        add_key(l_back, "Back", 0, SDL_CONTROLLER_BUTTON_BACK);
        lv_obj_t *l_start = pad_label(app, panel, ix, iy, iw, ih, 595, 525, "Start");
        add_key(l_start, "Start", 0, SDL_CONTROLLER_BUTTON_START);
        lv_obj_t *l_guide = pad_label(app, panel, ix, iy, iw, ih, 500, 375, "Guide");
        add_key(l_guide, "Guide", 0, SDL_CONTROLLER_BUTTON_GUIDE);
    }

    g_status = ui_label_center(panel, app->backend.controller ? "按下手柄按键" : "未连接手柄",
                               FONT_LG, PAL.accent2, 0, 568, 900, 30);
    ui_label_center(panel, "B / Esc 返回", FONT_XS, PAL.text3, 0, 598, 900, 18);
    ui_gamepad_poll(app);
}
