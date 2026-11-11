// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

typedef struct { App *app; int index; } SetClickCtx;

static void free_ctx(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void scroll_settled(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_obj_invalidate(obj);
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_END) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent) lv_obj_invalidate(parent);
    }
}


static void rail_clicked(lv_event_t *e)
{
    SetClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    app->settings_cat = ctx->index;
    app->settings_row = 0;
    app->settings_page = 0;
    app->settings_on_rail = false;
    app_rebuild(app);
    audio_play(SND_NAV);
}

static void row_clicked(lv_event_t *e)
{
    SetClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    app->settings_on_rail = false;
    app->settings_row = ctx->index;
    ui_update_settings(app);
    app_settings_activate(app, app->settings_row);
}

static void back_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_goto_screen(app, SCREEN_HOME);
    audio_play(SND_BACK);
}

static bool fs_space(const char *path, char *used, size_t used_n,
                     char *total, size_t total_n, char *free_out, size_t free_n)
{
    struct statvfs st;
    if (statvfs(path, &st) != 0 || st.f_blocks == 0) return false;
    off_t total_b = (off_t)st.f_blocks * (off_t)st.f_frsize;
    off_t free_b = (off_t)st.f_bavail * (off_t)st.f_frsize;
    off_t used_b = total_b - free_b;
    if (used_b < 0) used_b = 0;
    fs_format_size(used_b, used, used_n);
    fs_format_size(total_b, total, total_n);
    fs_format_size(free_b, free_out, free_n);
    return true;
}

static const char *settings_license_text(void)
{
    return
        "本项目代码与自有素材以 GPL-3.0-or-later 分发。\n"
        "Copyright (C) 2026 the Taiji Desktop project authors.\n\n"
        "完整许可证文本见安装目录：\n"
        "/usr/share/licenses/taiji/LICENSE\n\n"
        "第三方组件\n"
        "--------------------------------\n"
        "LVGL 9.3：MIT\n"
        "librime：BSD-3-Clause（运行时动态加载）\n"
        "SDL2：zlib\n"
        "libpng：libpng/zlib\n"
        "GLib / GIO：LGPL-2.1-or-later（动态链接）\n"
        "Wayland 与协议代码：MIT\n"
        "ZCOOL QingKe HuangYou 字体：SIL OFL 1.1\n"
        "uMTP-Responder：GPLv3（独立可执行文件）\n\n"
        "素材说明\n"
        "--------------------------------\n"
        "平台图标、音效和启动图是项目自有素材，随本项目以 GPL-3.0-or-later 授权。\n"
        "平台名称与图标属于各自厂商的商标，仅用于识别模拟平台。\n"
        "本项目与任何主机厂商无关联，也未获得其背书或赞助。\n\n"
        "uMTP-Responder 的完整对应源码随本项目提供，位于：\n"
        "third_party/umtp-responder\n";
}

static void settings_sysinfo_text(App *app, char *out, size_t out_size)
{
    unsigned long mem_total = 0, mem_avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal:", 9) == 0)
                sscanf(line + 9, "%lu", &mem_total);
            else if (strncmp(line, "MemAvailable:", 13) == 0)
                sscanf(line + 13, "%lu", &mem_avail);
        }
        fclose(f);
    }
    char mem_used[32] = "-", mem_all[32] = "-";
    if (mem_total > 0) {
        unsigned long used = mem_total > mem_avail ? mem_total - mem_avail : 0;
        fs_format_size((off_t)used * 1024, mem_used, sizeof(mem_used));
        fs_format_size((off_t)mem_total * 1024, mem_all, sizeof(mem_all));
    }
    char root_used[32] = "-", root_total[32] = "-", root_free[32] = "-";
    fs_space("/", root_used, sizeof(root_used), root_total, sizeof(root_total),
             root_free, sizeof(root_free));

    char sd_line[640] = "未检测到";
    FILE *m = fopen("/proc/mounts", "r");
    if (m) {
        char dev[256], mp[512], type[64], opts[256];
        while (fscanf(m, "%255s %511s %63s %255s %*d %*d", dev, mp, type, opts) == 4) {
            if (strncmp(mp, "/run/media/", 11) != 0 &&
                strncmp(mp, "/media/", 7) != 0 &&
                strncmp(mp, "/mnt/", 5) != 0) continue;
            char su[32], st[32], sf[32];
            if (fs_space(mp, su, sizeof(su), st, sizeof(st), sf, sizeof(sf))) {
                snprintf(sd_line, sizeof(sd_line), "%s  已用 %s / %s，可用 %s",
                         mp, su, st, sf);
                break;
            }
        }
        fclose(m);
    }

    struct utsname u;
    memset(&u, 0, sizeof(u));
    uname(&u);
    char model[160] = "未知机型";
    const char *model_paths[] = {
        "/sys/firmware/devicetree/base/model",
        "/proc/device-tree/model"
    };
    for (size_t mi = 0; mi < sizeof(model_paths) / sizeof(model_paths[0]); mi++) {
        FILE *mf = fopen(model_paths[mi], "rb");
        if (!mf) continue;
        size_t got = fread(model, 1, sizeof(model) - 1, mf);
        fclose(mf);
        model[got] = '\0';
        model[strcspn(model, "\r\n")] = '\0';
        size_t ml = strlen(model);
        while (ml > 0 && (unsigned char)model[ml - 1] < 32) model[--ml] = '\0';
        if (model[0]) break;
        snprintf(model, sizeof(model), "未知机型");
    }
    char osname[256] = "未知系统";
    FILE *osf = fopen("/etc/os-release", "r");
    if (osf) {
        char line[256];
        while (fgets(line, sizeof(line), osf)) {
            if (strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
            char *v = line + 12;
            while (*v == '"' || *v == '\'') v++;
            v[strcspn(v, "\r\n\"")] = '\0';
            snprintf(osname, sizeof(osname), "%s", v);
            break;
        }
        fclose(osf);
    }
    const char *session = getenv("TAIJI_SESSION_MODE");
    if (!session || !*session) session = "桌面";

#ifndef TAIJI_VERSION
#define TAIJI_VERSION "1.0.0"
#endif
    snprintf(out, out_size,
             "太极 / Taiji " TAIJI_VERSION "\n"
             "许可证：GPL-3.0-or-later\n"
             "LVGL 9.3 + SDL2\n\n"
             "系统：%s\n"
             "机型：%s\n"
             "设备：%s\n"
             "分辨率：%dx%d  界面：%dx%d\n"
             "渲染器：%s\n"
             "会话：%s\n\n"
             "内存：已用 %s / 共 %s\n"
             "系统存储：已用 %s / 共 %s\n"
             "SD 卡：%s\n\n"
             "内核：%s %s\n",
             osname, model,
             u.nodename[0] ? u.nodename : "未知",
             app->backend.width, app->backend.height,
             app->layout.w, app->layout.h,
             app->renderer_name[0] ? app->renderer_name : "自动",
             session,
             mem_used, mem_all, root_used, root_total, sd_line,
             u.sysname[0] ? u.sysname : "Linux",
             u.release[0] ? u.release : "-");
}

void ui_build_settings(App *app)
{
    ui_screen_background(app, NULL, PAL.accent);

    UiLayout *L = &app->layout;
    /* Left category rail */
    lv_obj_t *rail = ui_rect(app->ui.root, L->rail_x, L->rail_y, L->rail_w, L->rail_h, 18, PAL.surface);
    lv_obj_set_style_bg_opa(rail, 180, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_pad_all(rail, 0, 0);
    lv_obj_set_scroll_dir(rail, LV_DIR_VER);
    ui_scrollbar_style(rail);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    app->ui.settings_rail_count = 0;
    int cats = app_settings_rail_count();
    int rail_rh = L->rail_row_h;
    int rail_gap = L->rail_gap;
    /* 分类较多时压缩行高，保证“关于”在 320x240 也不会被裁掉。 */
    int need = 16 + cats * rail_rh + (cats - 1) * rail_gap;
    int rail_top = 8;
    if (need > L->rail_h) {
        rail_gap = 1;
        rail_top = 4;
        rail_rh = (L->rail_h - rail_top * 2 - (cats - 1) * rail_gap) / cats;
        if (rail_rh < 14) rail_rh = 14;
    }
    for (int i = 0; i < cats && i < 16; i++) {
        int cat = app_settings_rail_cat(i);
        const char *sub = NULL;
        const char *name = app_settings_category_name(cat, &sub);
        bool sel = cat == app->settings_cat;
        int rh = rail_rh;
        int rw = L->rail_w - 12;
        lv_obj_t *row = ui_rect(rail, 6, rail_top + i * (rh + rail_gap), rw, rh, 12,
                                sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 48 : 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_font_t *rail_font = rh < 22 ? FONT_XS :
                               (L->w < 600 ? FONT_SM : FONT_MD);
        int rail_line = ui_text_height(rail_font);
        lv_obj_t *lbl = ui_label(row, name, rail_font,
                                 sel ? PAL.accent2 : PAL.text, 16,
                                 (rh - rail_line) / 2, rw - 28, rail_line);
        if (sel) lv_obj_set_style_text_letter_space(lbl, 1, 0);
        SetClickCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = cat;
        lv_obj_add_event_cb(row, rail_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, free_ctx, LV_EVENT_DELETE, ctx);
        app->ui.settings_rail_labels[app->ui.settings_rail_count] = lbl;
        app->ui.settings_rail[app->ui.settings_rail_count++] = row;
        if (sel) ui_scroll_into_view(row, true);
    }

    /* Right detail panel */
    lv_obj_t *panel = ui_rect(app->ui.root, L->settings_panel_x, L->settings_panel_y,
                              L->settings_panel_w, L->settings_panel_h, 20, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 210, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);

    const char *subtitle = NULL;
    const char *cat_name = app->settings_license ? "开源许可" :
                           (app->settings_about ? "关于本机" :
                            app_settings_category_name(app->settings_cat, &subtitle));
    if (app->settings_license) subtitle = "GPL-3.0-or-later 与第三方组件";
    else if (app->settings_about) subtitle = "版本与系统信息";
    int inner = ui_clamp(L->settings_panel_w * 3 / 100, 14, 28);
    lv_font_t *title_font = L->w < 600 ? FONT_LG : FONT_XL;
    int title_line = ui_unscale_y(lv_font_get_line_height(title_font));
    int sub_line = ui_unscale_y(lv_font_get_line_height(FONT_XS));
    int title_y = 8;
    int sub_y = title_y + title_line + 6;
    int header_used = sub_y + sub_line + 8;
    if (header_used < L->settings_header_h) header_used = L->settings_header_h;
    app->ui.settings_title = ui_label(panel, cat_name, title_font,
                                      PAL.text, inner, title_y,
                                      L->settings_panel_w - inner * 2,
                                      title_line + 4);
    app->ui.settings_subtitle = ui_label(panel, subtitle, FONT_XS, PAL.text2,
                                         inner, sub_y,
                                         L->settings_panel_w - inner * 2,
                                         sub_line + 2);
    lv_obj_t *divider = ui_rect(panel, inner, header_used - 6,
                                L->settings_panel_w - inner * 2, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(divider, 150, 0);

    int total = 0;
    app_settings_rows(app, &total);
    app->settings_page = 0;
    app->ui.settings_row_count = 0;
    app->ui.settings_license_body = NULL;
    if (app->settings_license) {
        int body_y = L->settings_header_h;
        int body_h = L->settings_panel_h - body_y - 12;
        if (body_h < 40) body_h = 40;
        lv_obj_t *body = lv_obj_create(panel);
        lv_obj_remove_style_all(body);
        lv_obj_set_pos(body, ui_scale_x(inner), ui_scale_y(body_y));
        lv_obj_set_size(body, ui_scale_x(L->settings_panel_w - inner * 2),
                        ui_scale_y(body_h));
        lv_obj_set_style_pad_all(body, 0, 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
        ui_scrollbar_style(body);
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(body, scroll_settled, LV_EVENT_SCROLL, NULL);
        lv_obj_add_event_cb(body, scroll_settled, LV_EVENT_SCROLL_END, NULL);
        lv_obj_t *text = ui_label(body, settings_license_text(),
                                  L->settings_panel_w < 480 ? FONT_XS : FONT_SM,
                                  PAL.text2, 0, 0,
                                  L->settings_panel_w - inner * 2, 1600);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(text,
                                         L->settings_panel_w < 480 ? 1 : 3, 0);
        app->ui.settings_license_body = body;
    } else if (app->settings_about) {
        char text[2048];
        settings_sysinfo_text(app, text, sizeof(text));
        lv_obj_t *body = ui_label(panel, text,
                                  L->settings_panel_w < 480 ? FONT_XS : FONT_SM,
                                  PAL.text2, inner, L->settings_header_h,
                                  L->settings_panel_w - inner * 2,
                                  L->settings_panel_h - L->settings_header_h - 12);
        lv_obj_set_style_text_line_space(body,
                                         L->settings_panel_w < 480 ? 1 : 3, 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    } else {
        int list_y = header_used;
        int list_h = L->settings_panel_h - list_y - 8;
        if (list_h < 40) list_h = 40;
        lv_obj_t *list = lv_obj_create(panel);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, ui_scale_x(inner), ui_scale_y(list_y));
        lv_obj_set_size(list, ui_scale_x(L->settings_panel_w - inner * 2), ui_scale_y(list_h));
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        ui_scrollbar_style(list);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(list, scroll_settled, LV_EVENT_SCROLL, NULL);
        lv_obj_add_event_cb(list, scroll_settled, LV_EVENT_SCROLL_END, NULL);

        int rh = L->settings_row_h;
        int rw = L->settings_panel_w - inner * 2;
        for (int row = 0; row < total; row++) {
            char title[256] = {0}, value[512] = {0};
            int kind = 0;
            app_settings_row_info(app, row, title, sizeof(title), value, sizeof(value), &kind);
            bool sel = !app->settings_on_rail && app->settings_row == row;
            int ry = row * rh;
            lv_obj_t *r = ui_rect(list, 0, ry, rw, rh - 4, 12,
                                  sel ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(r, sel ? 34 : 70, 0);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            int value_w = rw * 48 / 100;
            int title_w = rw - value_w - 24;
            lv_font_t *title_font = L->w < 600 ? FONT_SM : FONT_MD;
            int title_line = ui_unscale_y(lv_font_get_line_height(title_font));
            int title_h = title_line + 2;
            lv_obj_t *tl = ui_label(r, title, title_font, PAL.text, 12,
                                    (rh - title_h) / 2, title_w, title_h);
            lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
            const char *tail = kind == 1 ? ">" : "";
            char right[600];
            if (value[0]) snprintf(right, sizeof(right), "%s  %s", tr(value), tail);
            else snprintf(right, sizeof(right), "%s", tail);
            int value_line = ui_unscale_y(lv_font_get_line_height(FONT_XS));
            int value_h = value_line + 2;
            lv_obj_t *v = ui_label(r, right, FONT_XS,
                                   kind == 3 ? (PAL.is_light ? PAL.accent : PAL.accent2) : PAL.text2,
                                   rw - value_w - 4, (rh - value_h) / 2, value_w, value_h);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
            SetClickCtx *ctx = xmalloc(sizeof(*ctx));
            ctx->app = app;
            ctx->index = row;
            lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(r, free_ctx, LV_EVENT_DELETE, ctx);
            app->ui.settings_rows[app->ui.settings_row_count++] = r;
        }
        if (total == 0) {
            ui_label(list, "这个分类暂时没有可调整的项目。", FONT_MD, PAL.text2,
                     0, 0, rw, 30);
        }
        lv_obj_update_layout(list);
        if (total > 0 && app->settings_row >= 0 &&
            app->settings_row < app->ui.settings_row_count) {
            ui_scroll_into_view(app->ui.settings_rows[app->settings_row], true);
            lv_obj_invalidate(list);
            lv_obj_invalidate(lv_screen_active());
        }
    }

    int bh = ui_clamp(L->footer_h - 14, 28, 36);
    lv_obj_t *back = ui_rect(app->ui.root, L->pad, L->footer_y + (L->footer_h - bh) / 2,
                             ui_min(96, L->rail_w), bh, bh / 2, PAL.surface);
    lv_obj_set_style_bg_opa(back, 170, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    int back_h = ui_text_height(FONT_XS);
    ui_label_center(back, "返回", FONT_XS, PAL.text2, 0, (bh - back_h) / 2,
                    ui_min(96, L->rail_w), back_h);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, app);

    int hint_w = L->w < 600 ? 170 : 300;
    char hintbuf[160];
    ui_button_hint(app, hintbuf, sizeof(hintbuf),
                   L->w < 600 ? "上下 移动 / {A} 选择 / 左 返回"
                              : "上下 移动 / {A} 选择 / 左 返回 / {B} 返回");
    lv_obj_t *hint = ui_label(app->ui.root, hintbuf,
                              FONT_XS, PAL.text2, L->w - L->pad - hint_w,
                              L->footer_y + (L->footer_h - 20) / 2,
                              hint_w, 22);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
    app->ui.settings_hint_label = hint;

    ui_update_settings(app);
}

void ui_update_settings(App *app)
{
    if (app->ui.settings_hint_label) {
        char hintbuf[160];
        if (app->settings_on_rail)
            ui_button_hint(app, hintbuf, sizeof(hintbuf),
                           app->layout.w < 600 ? "上下 分类 / {A} 进入"
                                               : "上下 分类 / {A} 或右 进入 / {B} 返回");
        else
            ui_button_hint(app, hintbuf, sizeof(hintbuf),
                           app->layout.w < 600 ? "上下 移动 / {A} 选择 / 左 返回"
                                               : "上下 移动 / {A} 选择 / 左 返回 / {B} 返回");
        lv_label_set_text(app->ui.settings_hint_label, hintbuf);
    }
    for (int i = 0; i < app->ui.settings_rail_count; i++) {
        bool sel = app_settings_rail_cat(i) == app->settings_cat;
        lv_obj_t *row = app->ui.settings_rail[i];
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(row, sel ? (app->settings_on_rail ? 82 : 52) : 0, 0);
        ui_set_focus_ring(row, sel && app->settings_on_rail,
                          ui_focus_accent(i));
        lv_obj_t *lbl = app->ui.settings_rail_labels[i];
        if (lbl)
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(sel ? PAL.accent2 : PAL.text), 0);
    }
    for (int i = 0; i < app->ui.settings_row_count; i++) {
        bool sel = !app->settings_on_rail && app->settings_row == i;
        lv_obj_t *row = app->ui.settings_rows[i];
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(row, sel ? 58 : 70, 0);
        ui_set_focus_ring(row, sel, ui_focus_accent(app->settings_cat * 100 + i));
        if (sel) ui_scroll_into_view(row, true);
    }
    lv_obj_invalidate(lv_screen_active());
}
