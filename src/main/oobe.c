// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "oobe.h"
#include "system_locale.h"
#include "app.h"
#include "audio.h"
#include "external_osk.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void oobe_focus_default(App *app, int step)
{
    switch (step) {
        case OOBE_STEP_LANGUAGE:
            app->oobe_focus = app->config.language == 1 ? 1 : 0;
            break;
        case OOBE_STEP_TIMEZONE:
            app->oobe_focus = app->oobe_tz;
            break;
        case OOBE_STEP_THEME: {
            int idx = app->config.theme_auto ? 0 : app->config.theme + 1;
            int n = theme_preset_count() + 1;
            if (idx < 0 || idx >= n) idx = 0;
            app->oobe_focus = idx;
            break;
        }
        default:
            app->oobe_focus = 0;
            break;
    }
}

void oobe_start(App *app, bool prefill)
{
    app->oobe = true;
    app->oobe_step = OOBE_STEP_WELCOME;
    app->oobe_focus = 0;
    app->oobe_finishing = false;
    app->oobe_rom[0] = '\0';
    app->oobe_tz_scroll = 0;
    app->oobe_content_scroll = 0;
    app->oobe_last_step = -1;
    if (prefill) {
        if (app->config.root_count > 0 && app->config.roots[0])
            snprintf(app->oobe_rom, sizeof(app->oobe_rom), "%s",
                     app->config.roots[0]);
    }
    app->oobe_tz = app_timezone_current_option(app);
    app->modal = MODAL_NONE;
    app->modal_closing = false;
    app_rebuild(app);
}

static void oobe_apply_timezone(App *app, int index)
{
    const char *tz = app_timezone_option_id(app, index);
    char error[256] = {0};
    if (system_timezone_set(tz, error, sizeof(error))) {
        app->oobe_tz = index;
    } else {
        /* 没有权限或系统不支持时不要让初始设置卡在这里。 */
        ui_toast(app, error[0] ? error : "时区设置失败，已跳过");
    }
    app->oobe_step = OOBE_STEP_THEME;
    oobe_focus_default(app, app->oobe_step);
    app_rebuild(app);
}

void app_oobe_rom_done(App *app)
{
    char value[2048];
    snprintf(value, sizeof(value), "%s", app->input_buf);
    char *trimmed = str_trim(value);
    if (!*trimmed) {
        app->oobe_rom[0] = '\0';
        external_osk_hide(app);
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
        app->oobe_focus = 1;
        ui_toast(app, "已清除游戏目录");
        app_rebuild(app);
        return;
    }
    char *abs = path_absolute(trimmed);
    if (!abs || !fs_is_dir(abs)) {
        ui_toast(app, "目录不存在");
        free(abs);
        return;
    }
    snprintf(app->oobe_rom, sizeof(app->oobe_rom), "%s", abs);
    free(abs);
    external_osk_hide(app);
    app->modal = MODAL_NONE;
    app->modal_closing = false;
    ui_set_input_active(false);
    ui_close_modal_visual(app);
    app->oobe_focus = 1;
    app_rebuild(app);
}

void oobe_finish(App *app)
{
    app->oobe_finishing = true;
    char error[256] = {0};
    system_language_apply(app->config.language, error, sizeof(error));
    if (error[0]) ui_toast(app, error);

    if (app->oobe_rom[0]) {
        char *abs = path_absolute(app->oobe_rom);
        if (abs && fs_is_dir(abs)) {
            bool exists = false;
            for (int i = 0; i < app->config.root_count; i++) {
                if (app->config.roots[i] && strcmp(app->config.roots[i], abs) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) config_add_root(&app->config, abs);
        }
        free(abs);
    }

    app_save_settings(app);
    app->oobe_finishing = false;
    app->oobe = false;
    app->oobe_step = OOBE_STEP_WELCOME;
    app->oobe_focus = 0;
    app->screen = SCREEN_HOME;
    app->status_on_bar = false;
    app->nav_on_bar = false;
    if (app->config.root_count > 0) {
        app_rescan(app);
    } else {
        app_rebuild(app);
        ui_toast(app, "设置已完成");
    }
}

void oobe_input(App *app, InputAction action)
{
    int step = app->oobe_step;
    if (step < 0 || step >= OOBE_STEP_COUNT) step = OOBE_STEP_WELCOME;

    if (action == IN_CANCEL || action == IN_B) {
        if (step > OOBE_STEP_WELCOME) {
            app->oobe_step = step - 1;
            if (app->oobe_step == OOBE_STEP_TIMEZONE) app->oobe_tz_scroll = 0;
            oobe_focus_default(app, app->oobe_step);
            app_rebuild(app);
            audio_play(SND_BACK);
        }
        return;
    }

    if (action == IN_LEFT || action == IN_RIGHT) {
        int dir = action == IN_LEFT ? -1 : 1;
        if (step == OOBE_STEP_LANGUAGE) {
            app->oobe_focus = app->oobe_focus ? 0 : 1;
            app_rebuild(app);
            audio_play(SND_NAV);
        } else if (step == OOBE_STEP_THEME) {
            int n = theme_preset_count() + 1;
            app->oobe_focus = (app->oobe_focus + dir + n) % n;
            app_rebuild(app);
            audio_play(SND_NAV);
        } else if (step == OOBE_STEP_OTHER) {
            if (app->oobe_focus == 0) app_oobe_cycle_ui_scale(app, dir);
            else if (app->oobe_focus == 1) app_oobe_cycle_refresh(app, dir);
            else if (app->oobe_focus == 2) app_oobe_cycle_render(app, dir);
            else if (app->oobe_focus == 3) app_oobe_cycle_paused(app, dir);
            audio_play(SND_NAV);
        }
        return;
    }

    if (action == IN_UP || action == IN_DOWN) {
        int dir = action == IN_UP ? -1 : 1;
        if (step == OOBE_STEP_TIMEZONE) {
            int n = app_timezone_option_count(app);
            int f = app->oobe_focus + dir;
            if (f < 0) f = 0;
            if (f >= n) f = n - 1;
            app->oobe_focus = f;
            app_rebuild(app);
            audio_play(SND_NAV);
        } else if (step == OOBE_STEP_NETWORK || step == OOBE_STEP_GAMES) {
            app->oobe_focus = app->oobe_focus ? 0 : 1;
            app_rebuild(app);
            audio_play(SND_NAV);
        } else if (step == OOBE_STEP_OTHER) {
            int n = 5;
            app->oobe_focus = (app->oobe_focus + dir + n) % n;
            app_rebuild(app);
            audio_play(SND_NAV);
        }
        return;
    }

    if (action == IN_Y && step == OOBE_STEP_TIMEZONE) {
        app_oobe_date_open(app);
        return;
    }

    if (action != IN_CONFIRM && action != IN_A) return;

    switch (step) {
        case OOBE_STEP_WELCOME:
            app->oobe_step = OOBE_STEP_LANGUAGE;
            oobe_focus_default(app, app->oobe_step);
            app_rebuild(app);
            break;
        case OOBE_STEP_LANGUAGE:
            app_oobe_apply_language(app, app->oobe_focus == 1 ? 1 : 0);
            app->oobe_step = OOBE_STEP_TIMEZONE;
            app->oobe_tz_scroll = 0;
            oobe_focus_default(app, app->oobe_step);
            app_rebuild(app);
            break;
        case OOBE_STEP_TIMEZONE:
            oobe_apply_timezone(app, app->oobe_focus);
            break;
        case OOBE_STEP_THEME: {
            bool auto_mode = app->oobe_focus == 0;
            int preset = auto_mode ? 0 : app->oobe_focus - 1;
            app_oobe_apply_theme(app, preset, auto_mode);
            app->oobe_step = OOBE_STEP_NETWORK;
            oobe_focus_default(app, app->oobe_step);
            app_rebuild(app);
            break;
        }
        case OOBE_STEP_NETWORK:
            if (app->oobe_focus == 0) {
                app_open_control_center_view(app, STATUS_WIFI);
            } else {
                app->oobe_step = OOBE_STEP_GAMES;
                oobe_focus_default(app, app->oobe_step);
                app_rebuild(app);
            }
            break;
        case OOBE_STEP_GAMES:
            if (app->oobe_focus == 0) {
                app_open_text(app, TEXT_OOBE_ROM, 0, "游戏目录",
                              app->oobe_rom[0] ? app->oobe_rom : "",
                              "输入 ROM 根目录，留空则清除；例如 /home/armdeck/ROMs");
            } else {
                app->oobe_step = OOBE_STEP_OTHER;
                oobe_focus_default(app, app->oobe_step);
                app_rebuild(app);
            }
            break;
        case OOBE_STEP_OTHER:
            if (app->oobe_focus == 4) {
                oobe_finish(app);
            } else if (app->oobe_focus == 0) {
                app_oobe_cycle_ui_scale(app, 1);
            } else if (app->oobe_focus == 1) {
                app_oobe_cycle_refresh(app, 1);
            } else if (app->oobe_focus == 2) {
                app_oobe_cycle_render(app, 1);
            } else if (app->oobe_focus == 3) {
                app_oobe_cycle_paused(app, 1);
            }
            break;
        default:
            break;
    }
}
