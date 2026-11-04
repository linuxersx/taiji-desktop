// SPDX-License-Identifier: GPL-3.0-or-later
#include "store_ui.h"
#include "theme.h"
#include "theme_auto.h"
#include "i18n.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static StoreApp g_app;

static int store_scale_max(int w, int h)
{
    int by_w = w * 100 / 320;
    int by_h = h * 100 / 240;
    int max = by_w < by_h ? by_w : by_h;
    if (max > 300) max = 300;
    static const int steps[] = {100, 125, 150, 175, 200, 225, 250, 275, 300};
    int best = 100;
    for (int i = 0; i < 9; i++) if (max >= steps[i]) best = steps[i];
    return best;
}

static int store_effective_scale(StoreApp *app, int w, int h)
{
    int pct = app->config.ui_scale;
    if (app->config.ui_scale_auto) {
        int raw = h * 100 / 720;
        static const int steps[] = {100, 125, 150, 175, 200, 225, 250, 275, 300};
        int best = steps[0];
        int best_d = abs(raw - best);
        for (int i = 1; i < 9; i++) {
            int d = abs(raw - steps[i]);
            if (d < best_d) { best_d = d; best = steps[i]; }
        }
        pct = best;
    }
    if (pct < 100) pct = 100;
    if (pct > 300) pct = 300;
    int max = store_scale_max(w, h);
    if (pct > max) pct = max;
    (void)app;
    return pct;
}

static float store_font_base(int h)
{
    return (h <= 360) ? 0.85f : (h <= 520) ? 1.0f :
           (h <= 800) ? 1.2f : (h <= 1100) ? 1.5f :
           (h <= 1600) ? 2.0f : 2.6f;
}

static void on_input(void *userdata, InputAction action, const char *text)
{
    StoreApp *app = userdata;
    store_ui_input(app, action, text);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    StoreApp *app = &g_app;
    store_ui_init(app);

    int w = 1280, h = 720;
    const char *size = getenv("TAIJI_STORE_SIZE");
    if (size && *size) {
        int tw = 0, th = 0;
        if (sscanf(size, "%dx%d", &tw, &th) == 2 && tw >= 320 && th >= 240) {
            w = tw; h = th;
        }
    }
    if (!backend_init(&app->backend, w, h, on_input, app)) {
        fprintf(stderr, "taiji-store: failed to init SDL\n");
        return 2;
    }
    backend_set_title(&app->backend, "应用商店");
    bool test_size = size && *size;
    if (!test_size) {
        SDL_SetWindowFullscreen(app->backend.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_SetWindowBordered(app->backend.window, SDL_FALSE);
        app->backend.fullscreen = true;
        backend_refresh_size(&app->backend);
    }

    theme_init();
    app->active_theme = app->config.theme_auto
        ? (theme_auto_is_dark(app->config.auto_dark_min,
                              app->config.auto_light_min) ? 0 : 1)
        : app->config.theme;
    theme_set_preset(app->active_theme);
    app->ui_percent = store_effective_scale(app, app->backend.width,
                                            app->backend.height);
    theme_set_scale(store_font_base(app->backend.height) *
                    (float)app->ui_percent / 100.0f);

    lv_obj_t *scr = lv_screen_active();
    app->root = lv_obj_create(scr);
    lv_obj_remove_style_all(app->root);
    lv_obj_set_pos(app->root, 0, 0);
    lv_obj_set_size(app->root, app->backend.width, app->backend.height);
    lv_obj_set_style_bg_color(app->root, lv_color_hex(PAL.bg), 0);
    lv_obj_set_style_bg_opa(app->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(app->root, LV_OBJ_FLAG_SCROLLABLE);
    store_ui_set_view(app, app->backend.width, app->backend.height,
                      app->ui_percent, app->root);

    store_ui_build(app);
    store_ui_refresh(app);

    const char *test = getenv("TAIJI_STORE_TEST_ACTION");
    if (test && *test) {
        char *copy = xstrdup(test);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            if (strcmp(tok, "up") == 0) store_ui_input(app, IN_UP, NULL);
            else if (strcmp(tok, "down") == 0) store_ui_input(app, IN_DOWN, NULL);
            else if (strcmp(tok, "left") == 0) store_ui_input(app, IN_LEFT, NULL);
            else if (strcmp(tok, "right") == 0) store_ui_input(app, IN_RIGHT, NULL);
            else if (strcmp(tok, "confirm") == 0) store_ui_input(app, IN_CONFIRM, NULL);
            else if (strcmp(tok, "cancel") == 0) store_ui_input(app, IN_CANCEL, NULL);
            else if (strcmp(tok, "x") == 0) store_ui_input(app, IN_X, NULL);
            else if (strcmp(tok, "y") == 0) store_ui_input(app, IN_Y, NULL);
            else if (strcmp(tok, "start") == 0) store_ui_input(app, IN_START, NULL);
            else if (strncmp(tok, "text:", 5) == 0)
                store_ui_input(app, IN_TEXT, tok + 5);
        }
        free(copy);
    }

    const char *shot = getenv("TAIJI_STORE_SCREENSHOT");
    uint32_t delay = 1500;
    const char *sd = getenv("TAIJI_STORE_SCREENSHOT_DELAY");
    if (sd && *sd) delay = (uint32_t)atoi(sd);
    uint32_t t0 = SDL_GetTicks();

    while (app->running) {
        backend_poll(&app->backend);
        if (app->backend.resized) {
            app->backend.resized = false;
            lv_obj_set_size(app->root, app->backend.width, app->backend.height);
            app->ui_percent = store_effective_scale(app, app->backend.width,
                                                    app->backend.height);
            theme_set_scale(store_font_base(app->backend.height) *
                            (float)app->ui_percent / 100.0f);
            store_ui_set_view(app, app->backend.width, app->backend.height,
                              app->ui_percent, app->root);
            app->dirty = true;
        }
        store_ui_tick(app);
        lv_timer_handler();
        backend_present(&app->backend);
        if (shot && *shot && SDL_GetTicks() - t0 > delay) {
            backend_save_bmp(&app->backend, shot);
            app->running = false;
        }
        SDL_Delay(4);
    }
    store_ui_free(app);
    backend_shutdown(&app->backend);
    return 0;
}
