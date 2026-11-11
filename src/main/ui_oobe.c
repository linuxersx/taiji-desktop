// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "app.h"
#include "i18n.h"
#include "oobe.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    App *app;
    int index;
} OobeClickCtx;

static void oobe_click_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void oobe_clicked(lv_event_t *e)
{
    OobeClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    ctx->app->oobe_focus = ctx->index;
    oobe_input(ctx->app, IN_CONFIRM);
}

static void oobe_clickable(App *app, lv_obj_t *obj, int index)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    OobeClickCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = index;
    lv_obj_add_event_cb(obj, oobe_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(obj, oobe_click_free, LV_EVENT_DELETE, ctx);
}

static void oobe_zone_time(const char *tz, const char *fmt, char *out,
                           size_t out_size)
{
    char prev[160];
    bool changed = false;
    const char *old = getenv("TZ");
    if (tz && *tz) {
        if (old) snprintf(prev, sizeof(prev), "%s", old);
        setenv("TZ", tz, 1);
        tzset();
        changed = true;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, out_size, fmt, &tm);
    if (changed) {
        if (old) setenv("TZ", prev, 1);
        else unsetenv("TZ");
        tzset();
    }
}

static void oobe_set_label(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

static const char *const OOBE_WORLD_ZONES[] = {
    "Asia/Shanghai", "Asia/Tokyo", "Europe/London", "America/New_York"
};
static const char *const OOBE_WORLD_NAMES[] = { "北京", "东京", "伦敦", "纽约" };
#define OOBE_WORLD_COUNT 4

static void oobe_date_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app) app_oobe_date_open(app);
}

void ui_oobe_tick(App *app)
{
    if (!app || !app->oobe || app->files_pick != FILES_PICK_NONE) return;
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();
    if (last && now - last < 1000) return;
    last = now;

    if (app->ui.oobe_date_label) {
        char buf[64];
        oobe_zone_time(NULL, "%Y-%m-%d  %H:%M", buf, sizeof(buf));
        oobe_set_label(app->ui.oobe_date_label, buf);
    }
    if (app->ui.oobe_big_clock && app->ui.oobe_big_zone[0]) {
        char t[16], d[40];
        oobe_zone_time(app->ui.oobe_big_zone, "%H:%M", t, sizeof(t));
        oobe_zone_time(app->ui.oobe_big_zone, "%Y-%m-%d", d, sizeof(d));
        oobe_set_label(app->ui.oobe_big_clock, t);
        oobe_set_label(app->ui.oobe_big_date, d);
    }
    for (int i = 0; i < app->ui.oobe_clock_count; i++) {
        char t[16];
        oobe_zone_time(app->ui.oobe_clock_zones[i], "%H:%M", t, sizeof(t));
        oobe_set_label(app->ui.oobe_clock_labels[i], t);
    }
}

static const char *oobe_step_hint(App *app)
{
    switch (app->oobe_step) {
        case OOBE_STEP_WELCOME:  return "{A} 开始";
        case OOBE_STEP_LANGUAGE: return "左右 选择 / {A} 下一步 / {B} 上一步";
        case OOBE_STEP_TIMEZONE: return "上下 选择 / {A} 应用 / {Y} 日期 / {B} 上一步";
        case OOBE_STEP_THEME:    return "左右 选择 / {A} 下一步 / {B} 上一步";
        case OOBE_STEP_NETWORK:
        case OOBE_STEP_GAMES:    return "上下 选择 / {A} 确认 / {B} 上一步";
        case OOBE_STEP_OTHER:
            return "上下 选择 / 左右 修改 / {A} 确认 / {B} 上一步";
        default: return "";
    }
}

static void oobe_welcome(App *app, lv_obj_t *parent, int x, int w, int y, int h)
{
    (void)x;
    const char *pad = backend_controller_name(&app->backend);
    char line[200];
    if (pad && *pad)
        snprintf(line, sizeof(line), "%s%s", tr("已识别手柄："), pad);
    else
        snprintf(line, sizeof(line), "%s", tr("未检测到手柄，可使用触摸操作"));
    char start[64];
    ui_button_hint(app, start, sizeof(start), tr("按 {A} 开始"));

    int sm_h = ui_text_height(FONT_SM);
    int md_h = ui_text_height(FONT_MD);
    if (h < 200) {
        /* 小屏：收紧间距，保证开始提示在可视区内。 */
        int xl = ui_text_height(FONT_XL);
        int xs = ui_text_height(FONT_XS);
        int cy = y;
        ui_label_center(parent, "太极", FONT_XL, PAL.text, 0, cy, w, xl);
        ui_label_center(parent, "TAIJI / NOVARETRO DESKTOP", FONT_XS,
                        PAL.text3, 0, cy + xl + 2, w, xs);
        ui_label_center(parent, line, FONT_XS, PAL.text2, 0,
                        cy + xl + xs + 8, w, xs);
        ui_label_center(parent, start, FONT_MD, PAL.accent2, 0,
                        cy + xl + xs * 2 + 16, w, md_h);
        return;
    }

    int hero_h = ui_text_height(FONT_HERO);
    int cy = y + h / 2 - hero_h - 40;
    if (cy < y) cy = y;
    ui_label_center(parent, "太极", FONT_HERO, PAL.text, 0, cy, w, hero_h);
    ui_label_center(parent, "TAIJI / NOVARETRO DESKTOP", FONT_SM,
                    PAL.text3, 0, cy + hero_h + 6, w, sm_h);
    ui_label_center(parent, line, FONT_SM, PAL.text2, 0,
                    cy + hero_h + sm_h + 24, w, sm_h);
    ui_label_center(parent, start, FONT_MD, PAL.accent2, 0,
                    cy + hero_h + sm_h + 68, w, md_h);
}

static void oobe_language(App *app, lv_obj_t *parent, int x, int w, int y, int h)
{
    int gap = 24;
    int cw = (w - gap) / 2;
    int ch = ui_clamp(h * 30 / 100, 92, 170);
    if (ch > h) ch = h;
    int cy = y + (h - ch) / 2;
    if (cy < y) cy = y;
    static const char *names[2] = { "简体中文", "English" };
    static const char *subs[2] = { "界面与系统语言", "UI and system language" };
    for (int i = 0; i < 2; i++) {
        bool sel = app->oobe_focus == i;
        lv_obj_t *card = ui_rect(parent, x + i * (cw + gap), cy, cw, ch,
                                 16, sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(card, sel ? 55 : 60, 0);
        if (sel) {
            ui_set_focus_ring(card, true, PAL.accent);
            app->ui.oobe_focus_obj = card;
        }
        int lg = ui_text_height(FONT_LG);
        int xs = ui_text_height(FONT_XS);
        ui_label_center(card, names[i], FONT_LG, sel ? PAL.text : PAL.text2,
                        0, ch / 2 - lg - 2, cw, lg);
        ui_label_center(card, subs[i], FONT_XS, PAL.text3,
                        0, ch / 2 + 6, cw, xs);
        oobe_clickable(app, card, i);
    }
}

static void oobe_timezone(App *app, lv_obj_t *parent, int x, int w, int y,
                          int h)
{
    int gap = 14;
    int xs = ui_text_height(FONT_XS);
    bool compact = h < 200 || w < 420;
    int lw = compact ? w : (w - gap) * 56 / 100;
    int rw = compact ? 0 : w - lw - gap;
    int date_h = ui_clamp(h * 12 / 100, 44, 58);
    int list_h = h - date_h - 10;
    if (list_h < 44) list_h = 44;
    int n = app_timezone_option_count(app);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, ui_scale_x(x), ui_scale_y(y));
    lv_obj_set_size(list, ui_scale_x(lw), ui_scale_y(list_h));
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    ui_scrollbar_style(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    app->ui.oobe_tz_list = list;

    int rh = ui_clamp(app->layout.h * 8 / 100, 30, 48);
    lv_obj_t *focus_obj = NULL;
    for (int i = 0; i < n; i++) {
        bool sel = app->oobe_focus == i;
        bool current = app->oobe_tz == i;
        lv_obj_t *row = ui_rect(list, 0, i * (rh + 4), lw, rh, 8,
                                sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 55 : 40, 0);
        if (sel) ui_set_focus_ring(row, true, PAL.accent);
        int sh = ui_text_height(FONT_SM);
        lv_obj_t *name = ui_label(row, app_timezone_option_id(app, i), FONT_SM,
                                  sel ? PAL.text : PAL.text2, 14,
                                  (rh - sh) / 2, lw - 50, sh);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (current) {
            ui_label_center(row, LV_SYMBOL_OK, FONT_SM, PAL.success,
                            lw - 34, (rh - sh) / 2, 22, sh);
        }
        oobe_clickable(app, row, i);
        if (sel) focus_obj = row;
    }
    lv_obj_update_layout(list);
    if (app->oobe_tz_scroll > 0)
        lv_obj_scroll_to_y(list, app->oobe_tz_scroll, LV_ANIM_OFF);
    if (focus_obj) ui_scroll_into_view(focus_obj, false);

    /* 日期与时间 */
    lv_obj_t *dc = ui_rect(parent, x, y + list_h + 10, lw, date_h,
                           12, PAL.card);
    lv_obj_set_style_bg_opa(dc, 45, 0);
    lv_obj_add_flag(dc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dc, oobe_date_clicked, LV_EVENT_CLICKED, app);
    ui_label(dc, tr("当前日期与时间"), FONT_XS, PAL.text3, 14, 8,
             lw * 55 / 100, xs);
    char datebuf[64];
    oobe_zone_time(NULL, "%Y-%m-%d  %H:%M", datebuf, sizeof(datebuf));
    app->ui.oobe_date_label = ui_label(dc, datebuf, FONT_SM, PAL.text, 14,
                                       8 + xs + 2, lw * 62 / 100,
                                       ui_text_height(FONT_SM));
    char adjust[64];
    ui_button_hint(app, adjust, sizeof(adjust), tr("按 {Y} 调整"));
    ui_label_center(dc, adjust, FONT_XS, PAL.accent2, lw * 58 / 100,
                    (date_h - xs) / 2, lw * 40 / 100, xs);

    if (compact) return;

    /* 右侧世界时间 */
    lv_obj_t *card = ui_rect(parent, x + lw + gap, y, rw, h, 14,
                             PAL.surface);
    lv_obj_set_style_bg_opa(card, 220, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(card, 120, 0);

    const char *tz = app_timezone_option_id(app, app->oobe_focus);
    snprintf(app->ui.oobe_big_zone, sizeof(app->ui.oobe_big_zone), "%s", tz);
    int xl = ui_text_height(FONT_XL);
    ui_label(card, tr("所选时区"), FONT_XS, PAL.text3, 16, 12, rw - 32, xs);
    char big[16], big_date[40];
    oobe_zone_time(tz, "%H:%M", big, sizeof(big));
    oobe_zone_time(tz, "%Y-%m-%d", big_date, sizeof(big_date));
    app->ui.oobe_big_clock = ui_label(card, big, FONT_XL, PAL.text, 16,
                                      12 + xs + 2, rw - 32, xl);
    app->ui.oobe_big_date = ui_label(card, big_date, FONT_XS, PAL.text3, 16,
                                     12 + xs + xl + 4, rw - 32, xs);
    lv_obj_t *zl = ui_label(card, tz, FONT_XS, PAL.accent2, 16,
                            12 + xs + xl + xs + 6, rw - 32, xs);
    lv_label_set_long_mode(zl, LV_LABEL_LONG_DOT);

    int head_h = ui_clamp(h * 40 / 100, 96, 150);
    lv_obj_t *divider = ui_rect(card, 16, head_h - 8, rw - 32, 1, 0,
                                PAL.border);
    lv_obj_set_style_bg_opa(divider, 90, 0);

    int rows = OOBE_WORLD_COUNT;
    int avail = h - head_h - 8;
    if (avail / rows < 24 && avail / 24 < rows) rows = avail / 24;
    if (rows < 1) rows = 1;
    int wrh = ui_clamp(avail / rows, 20, 40);
    int sm = ui_text_height(FONT_SM);
    app->ui.oobe_clock_count = 0;
    for (int i = 0; i < rows; i++) {
        int ry = head_h + i * wrh;
        ui_label(card, tr(OOBE_WORLD_NAMES[i]), FONT_SM, PAL.text2, 16,
                 ry + (wrh - sm) / 2, rw / 2, sm);
        char t[16];
        oobe_zone_time(OOBE_WORLD_ZONES[i], "%H:%M", t, sizeof(t));
        lv_obj_t *lab = ui_label(card, t, FONT_SM, PAL.accent2,
                                 rw / 2, ry + (wrh - sm) / 2, rw / 2 - 16, sm);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_RIGHT, 0);
        int idx = app->ui.oobe_clock_count;
        if (idx < 8) {
            app->ui.oobe_clock_labels[idx] = lab;
            snprintf(app->ui.oobe_clock_zones[idx],
                     sizeof(app->ui.oobe_clock_zones[idx]), "%s",
                     OOBE_WORLD_ZONES[i]);
            app->ui.oobe_clock_count = idx + 1;
        }
    }
}

static void oobe_theme(App *app, lv_obj_t *parent, int x, int w, int y, int h)
{
    int n = theme_preset_count() + 1;
    int gap = 10;
    int cw = (w - gap * (n - 1)) / n;
    int ch = ui_clamp(h * 45 / 100, 92, 150);
    if (ch > h) ch = h;
    int cy = y + (h - ch) / 2;
    if (cy < y) cy = y;
    int title_h = ui_text_height(FONT_SM);
    int sub_h = ui_text_height(FONT_XS);
    for (int i = 0; i < n; i++) {
        bool sel = app->oobe_focus == i;
        lv_obj_t *card = ui_rect(parent, x + i * (cw + gap), cy, cw, ch,
                                 14, sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(card, sel ? 55 : 55, 0);
        if (sel) {
            ui_set_focus_ring(card, true, PAL.accent);
            app->ui.oobe_focus_obj = card;
        }
        int sw = cw - 20;
        int sh = ch - title_h - sub_h - 22;
        if (sh < 24) sh = 24;
        lv_obj_t *swatch = ui_rect(card, 10, 10, sw, sh, 10, PAL.card_soft);
        lv_obj_set_style_bg_opa(swatch, 255, 0);
        if (i == 0) {
            lv_obj_t *light = ui_rect(swatch, 0, 0, sw / 2, sh, 10, 0xFFFFFFFF);
            lv_obj_set_style_bg_opa(light, 255, 0);
            lv_obj_t *dark = ui_rect(swatch, sw / 2, 0, sw - sw / 2, sh, 10,
                                     0xFF1A1E2A);
            lv_obj_set_style_bg_opa(dark, 255, 0);
            lv_obj_t *dot = ui_rect(swatch, sw / 2 - 6, sh / 2 - 6, 12, 12, 6,
                                    PAL.accent);
            lv_obj_set_style_bg_opa(dot, 220, 0);
        } else {
            uint32_t bg = 0, cc = 0, ac = 0;
            theme_preview_colors(i - 1, &bg, &cc, &ac);
            lv_obj_set_style_bg_color(swatch, lv_color_hex(bg), 0);
            lv_obj_t *inner = ui_rect(swatch, sw / 6, sh / 4, sw * 2 / 3, sh / 2,
                                      8, cc);
            lv_obj_set_style_bg_opa(inner, 255, 0);
            lv_obj_t *dot = ui_rect(swatch, sw - 18, 8, 10, 10, 5, ac);
            lv_obj_set_style_bg_opa(dot, 255, 0);
        }
        char label[64];
        if (i == 0) snprintf(label, sizeof(label), "%s", "自动");
        else snprintf(label, sizeof(label), "%s", theme_preset_name(i - 1));
        ui_label_center(card, label, FONT_SM, sel ? PAL.text : PAL.text2,
                        0, ch - title_h - sub_h - 6, cw, title_h);
        if (i == 0)
            ui_label_center(card, "19:00–07:00", FONT_XS, PAL.text3,
                            0, ch - sub_h - 2, cw, sub_h);
        else
            ui_label_center(card, i == 2 ? "浅色" : "深色", FONT_XS, PAL.text3,
                            0, ch - sub_h - 2, cw, sub_h);
        oobe_clickable(app, card, i);
    }
}

static void oobe_choice_rows(App *app, lv_obj_t *parent, int x, int y, int w,
                             const char *t0, const char *s0,
                             const char *t1, const char *s1)
{
    int rh = ui_clamp(app->layout.h * 10 / 100, 40, 64);
    int sm = ui_text_height(FONT_SM);
    int xs = ui_text_height(FONT_XS);
    for (int i = 0; i < 2; i++) {
        bool sel = app->oobe_focus == i;
        lv_obj_t *row = ui_rect(parent, x, y + i * (rh + 8), w, rh, 12,
                                sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 55 : 45, 0);
        if (sel) {
            ui_set_focus_ring(row, true, PAL.accent);
            app->ui.oobe_focus_obj = row;
        }
        ui_label(row, i == 0 ? t0 : t1, FONT_SM, sel ? PAL.text : PAL.text2,
                 16, rh / 2 - sm - 1, w - 32, sm);
        ui_label(row, i == 0 ? s0 : s1, FONT_XS, PAL.text3,
                 16, rh / 2 + 4, w - 32, xs);
        oobe_clickable(app, row, i);
    }
}

static void oobe_status_card(App *app, lv_obj_t *parent, int x, int y, int w,
                             const char *title, const char *value,
                             const char *sub)
{
    int ch = ui_clamp(app->layout.h * 14 / 100, 62, 96);
    lv_obj_t *card = ui_rect(parent, x, y, w, ch, 14, PAL.surface);
    lv_obj_set_style_bg_opa(card, 210, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(card, 120, 0);
    int xs = ui_text_height(FONT_XS);
    int sm = ui_text_height(FONT_SM);
    ui_label(card, title, FONT_XS, PAL.text3, 16, 12, w - 32, xs);
    lv_obj_t *v = ui_label(card, value, FONT_SM, PAL.text, 16, 12 + xs + 6,
                           w - 32, sm);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    if (sub && *sub)
        ui_label(card, sub, FONT_XS, PAL.text3, 16, 12 + xs + sm + 10,
                 w - 32, xs);
}

static void oobe_network(App *app, lv_obj_t *parent, int x, int w, int y, int h)
{
    (void)h;
    char status[256];
    if (app->wifi.current_ssid[0])
        snprintf(status, sizeof(status), "%s", app->wifi.current_ssid);
    else if (!app->wifi.enabled)
        snprintf(status, sizeof(status), "%s", "Wi-Fi 已关闭");
    else
        snprintf(status, sizeof(status), "%s", "未连接");
    oobe_status_card(app, parent, x, y, w, "当前网络", status,
                     app->wifi.current_ssid[0] ? "已连接" : "连接后才能使用应用商店");
    int cy = y + ui_clamp(app->layout.h * 14 / 100, 62, 96) + 14;
    oobe_choice_rows(app, parent, x, cy, w,
                     "打开 Wi-Fi 设置", "扫描并连接无线网络",
                     "下一步", "稍后也可以在控制中心连接");
}

static void oobe_games(App *app, lv_obj_t *parent, int x, int w, int y, int h)
{
    (void)h;
    oobe_status_card(app, parent, x, y, w, "游戏目录",
                     app->oobe_rom[0] ? app->oobe_rom : "未选择",
                     app->oobe_rom[0] ? "完成后会扫描这个目录" :
                     "选择存放 ROM 的文件夹");
    int cy = y + ui_clamp(app->layout.h * 14 / 100, 62, 96) + 14;
    char sub0[128];
    ui_button_hint(app, sub0, sizeof(sub0), tr("输入路径，或按 {Y} 浏览选择"));
    oobe_choice_rows(app, parent, x, cy, w,
                     "选择目录", sub0,
                     "下一步", "稍后也可以在文件页添加");
}

static void oobe_other(App *app, lv_obj_t *parent, int x, int w, int y, int h)
{
    int gap = 14;
    int block_w = ui_min(w, 780);
    int bx = x + (w - block_w) / 2;
    int lw = (block_w - gap) * 54 / 100;
    int rw = block_w - lw - gap;

    char recap[600];
    const char *lang = app->config.language == 1 ? "English" : tr("中文");
    const char *tz = app_timezone_option_id(app, app->oobe_tz);
    const char *theme = app->config.theme_auto ? tr("自动") :
                        theme_preset_name(app->config.theme);
    const char *net = app->wifi.current_ssid[0] ? app->wifi.current_ssid :
                      tr("未连接");
    const char *games = app->oobe_rom[0] ? app->oobe_rom : tr("未选择");
    snprintf(recap, sizeof(recap), "%s / %s / %s / %s / %.120s",
             lang, tz, theme, net, games);

    int xs = ui_text_height(FONT_XS);
    int finish_h = ui_clamp(h * 9 / 100, 36, 54);
    int rh = ui_clamp((h - xs - 10 - finish_h - 18) / 4 - 6, 34, 50);
    int block_h = xs + 10 + 4 * (rh + 6) + 6 + finish_h;
    int block_y = y + (h > block_h ? (h - block_h) / 2 : 0);
    if (block_y < y) block_y = y;

    lv_obj_t *rl = ui_label(parent, recap, FONT_XS, PAL.text3,
                            bx, block_y, block_w, xs);
    lv_label_set_long_mode(rl, LV_LABEL_LONG_DOT);

    int row_y = block_y + xs + 10;

    char v0[64], v1[64], v2[64], v3[64];
    if (app->config.ui_scale_auto)
        snprintf(v0, sizeof(v0), "%s（%d%%）", tr("自动"),
                 app_effective_ui_scale(app));
    else
        snprintf(v0, sizeof(v0), "%d%%", app->config.ui_scale);
    if (app->config.refresh_hz == 0)
        snprintf(v1, sizeof(v1), "%s（%d Hz）", tr("自动"),
                 backend_display_refresh_hz(&app->backend));
    else
        snprintf(v1, sizeof(v1), "%d Hz", app->config.refresh_hz);
    snprintf(v2, sizeof(v2), "%d%%", app->config.render_scale);
    snprintf(v3, sizeof(v3), "%d %s", app->config.max_paused_apps, tr("个"));

    const char *titles[4] = { "界面缩放", "刷新率", "渲染分辨率", "最大暂停应用" };
    const char *values[4] = { v0, v1, v2, v3 };
    int sm = ui_text_height(FONT_SM);
    for (int i = 0; i < 4; i++) {
        bool sel = app->oobe_focus == i;
        lv_obj_t *row = ui_rect(parent, bx, row_y + i * (rh + 6), lw,
                                rh, 10, sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 55 : 45, 0);
        if (sel) {
            ui_set_focus_ring(row, true, PAL.accent);
            app->ui.oobe_focus_obj = row;
        }
        ui_label(row, titles[i], FONT_SM, sel ? PAL.text : PAL.text2,
                 14, (rh - sm) / 2, lw * 52 / 100, sm);
        lv_obj_t *v = ui_label(row, values[i], FONT_SM, PAL.accent2,
                               lw * 48 / 100, (rh - sm) / 2,
                               lw * 48 / 100 - 12, sm);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        oobe_clickable(app, row, i);
    }

    int by = row_y + 4 * (rh + 6) + 6;
    bool sel = app->oobe_focus == 4;
    lv_obj_t *btn = ui_rect(parent, bx, by, lw, finish_h,
                            finish_h / 2, PAL.accent);
    lv_obj_set_style_bg_opa(btn, sel ? 235 : 150, 0);
    if (sel) {
        ui_set_focus_ring(btn, true, PAL.accent);
        app->ui.oobe_focus_obj = btn;
    }
    int fh = ui_text_height(FONT_MD);
    ui_label_center(btn, tr("进入太极"), FONT_MD, 0xFFFFFFFF, 0,
                    (finish_h - fh) / 2, lw, fh);
    oobe_clickable(app, btn, 4);

    /* 右侧说明卡 */
    lv_obj_t *card = ui_rect(parent, bx + lw + gap, row_y, rw,
                             by + finish_h - row_y, 14, PAL.surface);
    lv_obj_set_style_bg_opa(card, 220, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(card, 120, 0);

    static const char *const desc_keys[4] = {
        "自动会按屏幕尺寸选择合适缩放；手动可固定 100% / 125% / 150% 等档位。",
        "限制帧率可以省电；自动会跟随屏幕刷新率。",
        "降低内部渲染分辨率能提升性能，画面会略微变软。",
        "切到其他应用时最多保留几个在后台，超出后最早的会被关闭。",
    };
    const char *ttl = app->oobe_focus == 4 ? tr("进入太极") : tr(titles[app->oobe_focus]);
    const char *desc = app->oobe_focus == 4 ?
        tr("完成后保存所有设置并进入太极。") : tr(desc_keys[app->oobe_focus]);
    const char *val = app->oobe_focus == 4 ? NULL : values[app->oobe_focus];

    ui_label(card, tr("说明"), FONT_XS, PAL.text3, 16, 14, rw - 32, xs);
    int lg = ui_text_height(FONT_LG);
    lv_obj_t *tl = ui_label(card, ttl, FONT_LG, PAL.text, 16, 14 + xs + 4,
                            rw - 32, lg);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_t *dl = ui_label(card, desc, FONT_SM, PAL.text2, 16,
                            14 + xs + lg + 14, rw - 32,
                            ui_text_height(FONT_SM) * 3);
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
    if (val) {
        ui_label(card, tr("当前值"), FONT_XS, PAL.text3, 16,
                 14 + xs + lg + 14 + ui_text_height(FONT_SM) * 3 + 14,
                 rw - 32, xs);
        lv_obj_t *vl = ui_label(card, val, FONT_MD, PAL.accent2, 16,
                                14 + xs + lg + 14 + ui_text_height(FONT_SM) * 3 + 14 + xs + 4,
                                rw - 32, ui_text_height(FONT_MD));
        lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
    }
}

void ui_build_oobe(App *app)
{
    UiLayout *L = &app->layout;
    ui_screen_background(app, NULL, PAL.accent);
    if (!app->oobe) return;

    app->ui.oobe_big_clock = NULL;
    app->ui.oobe_big_date = NULL;
    app->ui.oobe_big_zone[0] = '\0';
    app->ui.oobe_date_label = NULL;
    app->ui.oobe_clock_count = 0;
    app->ui.oobe_tz_list = NULL;
    app->ui.oobe_content = NULL;
    app->ui.oobe_focus_obj = NULL;

    if (app->oobe_last_step != app->oobe_step) {
        app->oobe_content_scroll = 0;
        app->oobe_tz_scroll = 0;
        app->oobe_last_step = app->oobe_step;
    }

    int pad = ui_clamp(L->w * 4 / 100, 14, 32);
    int panel_w = ui_min(L->w - pad * 2, 860);
    int panel_x = (L->w - panel_w) / 2;
    int top = ui_clamp(L->h * 5 / 100, 12, 40);
    int bottom = L->h - ui_clamp(L->h * 5 / 100, 12, 40);

    int steps = OOBE_STEP_COUNT;
    int dot = ui_clamp(L->h / 100, 5, 9);
    int step_gap = dot + 7;
    int total = (steps - 1) * step_gap + dot;
    int dx = (L->w - total) / 2;
    for (int i = 0; i < steps; i++) {
        lv_obj_t *d = ui_rect(app->ui.root, dx + i * step_gap, top, dot, dot,
                              dot / 2, PAL.border);
        lv_obj_set_style_bg_opa(d, 100, 0);
        if (i == app->oobe_step) {
            lv_obj_set_style_bg_color(d, lv_color_hex(PAL.accent), 0);
            lv_obj_set_style_bg_opa(d, 240, 0);
            lv_obj_set_style_shadow_width(d, 8, 0);
            lv_obj_set_style_shadow_color(d, lv_color_hex(PAL.accent), 0);
            lv_obj_set_style_shadow_opa(d, 90, 0);
        }
    }

    static const char *titles[OOBE_STEP_COUNT] = {
        "初始设置", "选择语言", "设置时区", "选择主题",
        "连接网络", "添加游戏目录", "其他设置",
    };
    static const char *subs[OOBE_STEP_COUNT] = {
        "第一次启动需要完成几项设置",
        "界面和系统语言",
        "让时间和所在地区一致",
        "选择喜欢的配色，稍后也可以修改",
        "连接 Wi-Fi 后才能使用应用商店",
        "选择存放游戏 ROM 的目录",
        "最后确认显示与应用设置",
    };
    int title_y = top + dot + 10;
    int title_h = ui_text_height(FONT_XL);
    int sub_h = ui_text_height(FONT_XS);
    ui_label_center(app->ui.root, titles[app->oobe_step], FONT_XL, PAL.text,
                    0, title_y, L->w, title_h);
    ui_label_center(app->ui.root, subs[app->oobe_step], FONT_XS, PAL.text3,
                    0, title_y + title_h + 2, L->w, sub_h);

    int content_y = title_y + title_h + sub_h + 16;
    int content_h = bottom - content_y - 26;
    if (content_h < 80) content_h = 80;

    /* 空间足够时直接画在 root 上，保持原有观感；小屏幕才套滚动容器。 */
    lv_obj_t *parent = app->ui.root;
    int cx = panel_x, cy = content_y, cw = panel_w, ch = content_h;
    if (content_h < 280) {
        lv_obj_t *content = lv_obj_create(app->ui.root);
        lv_obj_remove_style_all(content);
        lv_obj_set_pos(content, ui_scale_x(panel_x), ui_scale_y(content_y));
        lv_obj_set_size(content, ui_scale_x(panel_w), ui_scale_y(content_h));
        lv_obj_set_style_pad_all(content, 0, 0);
        lv_obj_set_scroll_dir(content, LV_DIR_VER);
        ui_scrollbar_style(content);
        lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        app->ui.oobe_content = content;
        parent = content;
        cx = 3;
        cy = 3;
        cw = panel_w - 6;
        ch = content_h - 6;
        if (cw < 120) cw = panel_w;
        if (ch < 40) ch = content_h;
    }
    switch (app->oobe_step) {
        case OOBE_STEP_WELCOME:
            oobe_welcome(app, parent, cx, cw, cy, ch);
            break;
        case OOBE_STEP_LANGUAGE:
            oobe_language(app, parent, cx, cw, cy, ch);
            break;
        case OOBE_STEP_TIMEZONE:
            oobe_timezone(app, parent, cx, cw, cy, ch);
            break;
        case OOBE_STEP_THEME:
            oobe_theme(app, parent, cx, cw, cy, ch);
            break;
        case OOBE_STEP_NETWORK:
            oobe_network(app, parent, cx, cw, cy, ch);
            break;
        case OOBE_STEP_GAMES:
            oobe_games(app, parent, cx, cw, cy, ch);
            break;
        case OOBE_STEP_OTHER:
            oobe_other(app, parent, cx, cw, cy, ch);
            break;
        default:
            break;
    }

    if (app->ui.oobe_content) {
        lv_obj_update_layout(app->ui.oobe_content);
        if (app->oobe_content_scroll > 0)
            lv_obj_scroll_to_y(app->ui.oobe_content,
                               app->oobe_content_scroll, LV_ANIM_OFF);
    }
    if (app->ui.oobe_focus_obj)
        ui_scroll_into_view(app->ui.oobe_focus_obj, false);

    char hint[256];
    ui_button_hint(app, hint, sizeof(hint), tr(oobe_step_hint(app)));
    ui_label_center(app->ui.root, hint, FONT_XS, PAL.text3,
                    0, bottom - 18, L->w, ui_text_height(FONT_XS));
}
