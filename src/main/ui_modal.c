// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "smb_share.h"
#include "catalog.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    int index;
    char id[96];
} ModalCtx;

static void modal_ctx_free(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void ime_box_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    ui_ime_show(app);
}


static lv_obj_t *panel_title(App *app, lv_obj_t *panel, const char *title)
{
    lv_obj_t *t = ui_label(panel, title, FONT_XL, PAL.text, 0, 0, 100, 34);
    (void)app;
    return t;
}

static void modal_common_title(App *app, lv_obj_t *panel, const char *title, const char *sub)
{
    ui_label(panel, title, FONT_XL, PAL.text, 30, 24, 600, 34);
    if (sub && *sub) ui_label(panel, sub, FONT_SM, PAL.text2, 30, 62, 660, 24);
    lv_obj_t *line = ui_rect(panel, 30, 96, 660, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 160, 0);
    (void)app;
}

/* ── Search ───────────────────────────────────────────────────────────────── */

static void search_row_cb(lv_event_t *e)
{
    ModalCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    app_close_modal(ctx->app);
    app_open_game(ctx->app, ctx->id);
}

static void search_close_cb(lv_event_t *e)
{
    app_close_modal(lv_event_get_user_data(e));
}

void ui_build_modal_search(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 760, 560);
    modal_common_title(app, panel, "搜索游戏", "在所有平台中按名称查找");

    lv_obj_t *box = ui_rect(panel, 30, 112, 700, 52, 16, PAL.card);
    lv_obj_set_style_bg_opa(box, 190, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(box, 120, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, ime_box_clicked, LV_EVENT_CLICKED, app);
    char shown[300];
    if (app->search_buf[0]) snprintf(shown, sizeof(shown), "%s|", app->search_buf);
    else snprintf(shown, sizeof(shown), "输入游戏名称...");
    app->ui.modal_input = ui_label(box, shown, FONT_MD, app->search_buf[0] ? PAL.text : PAL.text3,
                                   18, 14, 664, 26);
    lv_obj_t *sclose = ui_rect(panel, 620, 26, 110, 38, 19, PAL.card);
    lv_obj_set_style_bg_opa(sclose, 170, 0);
    lv_obj_set_style_border_width(sclose, 1, 0);
    lv_obj_set_style_border_color(sclose, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(sclose, 150, 0);
    lv_obj_add_flag(sclose, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(sclose, "关闭", FONT_SM, PAL.text2, 0, 8, 110, 22);
    lv_obj_add_event_cb(sclose, search_close_cb, LV_EVENT_CLICKED, app);

    GameList list = library_query(&app->library, "all", SORT_NAME, FILTER_ALL, app->search_buf);
    if (app->search_focus >= list.count) app->search_focus = list.count > 0 ? list.count - 1 : 0;
    if (app->search_focus < 0) app->search_focus = 0;
    app->ui.modal_list_count = 0;
    if (app->search_buf[0] == '\0') {
        ui_label(panel, "输入关键字开始搜索。中文、英文名称都可以。", FONT_SM, PAL.text3, 34, 198, 660, 24);
    } else if (list.count == 0) {
        ui_label(panel, "没有找到匹配的游戏。", FONT_MD, PAL.text2, 34, 198, 660, 30);
    } else {
        char count[64];
        snprintf(count, sizeof(count), "%d 个结果", list.count);
        ui_label(panel, count, FONT_XS, PAL.accent2, 34, 180, 300, 20);
        for (int i = 0; i < 7 && i < list.count; i++) {
            Game *g = list.items[i];
            bool sel = i == app->search_focus;
            lv_obj_t *row = ui_rect(panel, 30, 208 + i * 42, 700, 36, 12,
                                    sel ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(row, sel ? 36 : 80, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            ui_label(row, g->name, FONT_SM, sel ? PAL.accent2 : PAL.text, 14, 8, 500, 22);
            const char *pn = catalog_zh_name(g->platform_id);
            lv_obj_t *pl = ui_label(row, pn, FONT_XS, PAL.text3, 500, 9, 160, 20);
            lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_RIGHT, 0);
            ModalCtx *ctx = xmalloc(sizeof(*ctx));
            ctx->app = app;
            ctx->index = i;
            snprintf(ctx->id, sizeof(ctx->id), "%s", g->id);
            lv_obj_add_event_cb(row, search_row_cb, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(row, modal_ctx_free, LV_EVENT_DELETE, ctx);
            app->ui.modal_list[app->ui.modal_list_count++] = row;
        }
    }
    char search_hint[96];
    ui_button_hint(app, search_hint, sizeof(search_hint),
                   "上下 选择 / {A} 打开 / {B} 关闭");
    ui_label(panel, search_hint, FONT_XS, PAL.text3, 30, 512, 700, 20);
    game_list_free(&list);
}

/* ── Detail ───────────────────────────────────────────────────────────────── */

static void detail_launch_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    Game *g = library_find_game(&app->library, app->selected_game_id);
    if (g) { app_close_modal(app); app_launch_game(app, g); }
}

static void detail_fav_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    Game *g = library_find_game(&app->library, app->selected_game_id);
    if (g) { app_toggle_favorite(app, g); ui_build_modal(app); }
}

static void detail_edit_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    Game *g = library_find_game(&app->library, app->selected_game_id);
    if (!g) return;
    app_close_modal(app);
    app_open_text(app, TEXT_GAME_COMMAND, 0, "游戏启动命令", g->command,
                  "留空则使用平台默认命令。%ROM% 表示游戏路径。");
}

void ui_build_modal_detail(App *app)
{
    Game *g = library_find_game(&app->library, app->selected_game_id);
    if (!g) return;
    lv_obj_t *panel = ui_modal_panel(app, 760, 540);
    lv_obj_t *cover = ui_rect(panel, 30, 34, 300, 400, 20, PAL.card_soft);
    lv_obj_set_style_clip_corner(cover, true, 0);
    if (g->cover_path && g->cover_path[0] && file_exists(g->cover_path)) {
        lv_obj_t *img = lv_image_create(cover);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style_all(img);
        lv_obj_set_size(img, ui_scale_x(300), ui_scale_y(400));
        lv_obj_center(img);
        char *src = ui_img_src(g->cover_path);
        lv_image_set_src(img, src);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        free(src);
    } else {
        char glyph[8] = {0};
        int bytes = 1;
        unsigned char c = (unsigned char)g->name[0];
        if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        memcpy(glyph, g->name, (size_t)bytes);
        ui_label_center(cover, glyph, FONT_HERO, 0x55FFFFFF, 0, 160, 300, 90);
    }

    int x = 360;
    ui_label(panel, catalog_zh_name(g->platform_id), FONT_XS, PAL.accent2, x, 38, 360, 20);
    ui_label(panel, g->name, FONT_XXL, PAL.text, x, 62, 360, 76);
    char meta[512];
    bool has_dev = g->developer && g->developer[0];
    bool has_date = g->release_date && g->release_date[0];
    snprintf(meta, sizeof(meta), "%s%s%s", has_dev ? g->developer : "",
             has_dev && has_date ? "  /  " : "",
             has_date ? g->release_date : "");
    ui_label(panel, meta, FONT_SM, PAL.text2, x, 148, 360, 24);
    ui_label(panel, g->favorite ? "已收藏" : "未收藏", FONT_SM,
             g->favorite ? 0xFFFF6B9A : PAL.text3, x, 178, 200, 22);
    lv_obj_t *desc = ui_label(panel,
        g->description && g->description[0] ? g->description : "暂无游戏简介。",
                              FONT_SM, PAL.text2, x, 212, 360, 150);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    char *played = format_playtime(g->play_seconds);
    char stats[256];
    snprintf(stats, sizeof(stats), "游玩 %d 次  /  %s", g->play_count, played);
    free(played);
    ui_label(panel, stats, FONT_XS, PAL.text3, x, 372, 360, 22);

    lv_obj_t *b1 = ui_rect(panel, x, 420, 140, 44, 22, PAL.accent);
    lv_obj_set_style_bg_opa(b1, 230, 0);
    lv_obj_add_flag(b1, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(b1, "开始游戏", FONT_SM, 0xFFFFFFFF, 0, 11, 140, 22);
    lv_obj_add_event_cb(b1, detail_launch_cb, LV_EVENT_CLICKED, app);
    lv_obj_t *b2 = ui_rect(panel, x + 154, 420, 100, 44, 22, PAL.card);
    lv_obj_add_flag(b2, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(b2, g->favorite ? "取消收藏" : "收藏", FONT_SM, PAL.text, 0, 11, 100, 22);
    lv_obj_add_event_cb(b2, detail_fav_cb, LV_EVENT_CLICKED, app);
    lv_obj_t *b3 = ui_rect(panel, x + 268, 420, 100, 44, 22, PAL.card);
    lv_obj_add_flag(b3, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(b3, "编辑命令", FONT_SM, PAL.text, 0, 11, 100, 22);
    lv_obj_add_event_cb(b3, detail_edit_cb, LV_EVENT_CLICKED, app);

    char detail_hint[160];
    ui_button_hint(app, detail_hint, sizeof(detail_hint),
                   "{A} 开始游戏 / {Y} 收藏 / {X} 编辑命令 / {B} 关闭");
    ui_label(panel, detail_hint, FONT_XS, PAL.text3, 30, 496, 700, 20);
}

/* ── Text ─────────────────────────────────────────────────────────────────── */

static void text_browse_cb(lv_event_t *e)
{
    app_files_pick_start(lv_event_get_user_data(e));
}

static void text_confirm_cb(lv_event_t *e)
{
    app_input(lv_event_get_user_data(e), IN_CONFIRM, NULL);
}

static void text_cancel_cb(lv_event_t *e)
{
    app_input(lv_event_get_user_data(e), IN_CANCEL, NULL);
}

void ui_build_modal_text(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 760, 360);
    modal_common_title(app, panel, app->modal_title, app->modal_hint);
    lv_obj_t *box = ui_rect(panel, 30, 122, 700, 60, 16, PAL.card);
    lv_obj_set_style_bg_opa(box, 190, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(box, 120, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, ime_box_clicked, LV_EVENT_CLICKED, app);
    char shown[2100];
    if (app->text_purpose == TEXT_SMB_PASS) {
        size_t n = strlen(app->input_buf);
        if (n >= sizeof(shown) - 1) n = sizeof(shown) - 2;
        memset(shown, '*', n);
        shown[n] = '\0';
        size_t len = strlen(shown);
        if (len + 1 < sizeof(shown)) { shown[len] = '|'; shown[len + 1] = '\0'; }
    } else {
        snprintf(shown, sizeof(shown), "%s|", app->input_buf[0] ? app->input_buf : "");
    }
    app->ui.modal_input = ui_label(box, shown, FONT_MD,
                                   app->input_buf[0] ? PAL.text : PAL.text3, 18, 16, 664, 28);
    bool can_browse = app_files_pick_supported(app);
    const char *hint = "直接输入字符 / {A} 确认 / {B} 取消 / 退格 删除";
    if (can_browse)
        hint = app->text_purpose == TEXT_ROOT ?
            "{Y} 浏览目录 / {A} 确认 / {B} 取消 / 退格 删除" :
            "{Y} 浏览文件 / {A} 确认 / {B} 取消 / 退格 删除";
    char text_hint[192];
    ui_button_hint(app, text_hint, sizeof(text_hint), hint);
    ui_label(panel, text_hint, FONT_XS, PAL.text3, 30, 300,
             can_browse ? 450 : 480, 20);
    lv_obj_t *cancel = ui_rect(panel, 520, 288, 90, 40, 20, PAL.card);
    lv_obj_set_style_bg_opa(cancel, 170, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(PAL.border), 0);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(cancel, "取消", FONT_SM, PAL.text2, 0, 8, 90, 22);
    lv_obj_add_event_cb(cancel, text_cancel_cb, LV_EVENT_CLICKED, app);
    lv_obj_t *ok = ui_rect(panel, 620, 288, 110, 40, 20, PAL.accent);
    lv_obj_set_style_bg_opa(ok, 230, 0);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(ok, "确定", FONT_SM, 0xFFFFFFFF, 0, 8, 110, 22);
    lv_obj_add_event_cb(ok, text_confirm_cb, LV_EVENT_CLICKED, app);
    if (can_browse) {
        lv_obj_t *btn = ui_rect(panel, 620, 26, 110, 38, 19, PAL.card);
        lv_obj_set_style_bg_opa(btn, 170, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(PAL.border_focus), 0);
        lv_obj_set_style_border_opa(btn, 150, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        ui_label_center(btn, app->text_purpose == TEXT_ROOT ? "浏览目录" : "浏览文件",
                        FONT_SM, PAL.accent2, 0, 8, 110, 22);
        lv_obj_add_event_cb(btn, text_browse_cb, LV_EVENT_CLICKED, app);
    }
}

/* ── Confirm ──────────────────────────────────────────────────────────────── */

static void confirm_yes_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app->confirm_focus = 0;
    /* Reuse the keyboard path. */
    app_input(app, IN_CONFIRM, NULL);
}

static void confirm_no_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_close_modal(app);
}

void ui_build_modal_confirm(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 600, 300);
    ui_label(panel, app->modal_title, FONT_XL, PAL.text, 30, 34, 540, 36);
    lv_obj_t *body = ui_label(panel, app->modal_body, FONT_SM, PAL.text2, 30, 82, 540, 90);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_t *yes = ui_rect(panel, 170, 206, 120, 44, 22,
                            app->confirm_focus == 0 ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(yes, app->confirm_focus == 0 ? 220 : 160, 0);
    lv_obj_add_flag(yes, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(yes, "确认", FONT_MD, PAL.text, 0, 10, 120, 24);
    lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, app);
    lv_obj_t *no = ui_rect(panel, 310, 206, 120, 44, 22,
                           app->confirm_focus == 1 ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(no, app->confirm_focus == 1 ? 220 : 160, 0);
    lv_obj_add_flag(no, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(no, "取消", FONT_MD, PAL.text, 0, 10, 120, 24);
    lv_obj_add_event_cb(no, confirm_no_cb, LV_EVENT_CLICKED, app);
    char confirm_hint[96];
    ui_button_hint(app, confirm_hint, sizeof(confirm_hint),
                   "左右 选择 / {A} 确认 / {B} 取消");
    ui_label_center(panel, confirm_hint, FONT_XS, PAL.text3, 0, 266, 600, 20);
}

/* ── Choice ───────────────────────────────────────────────────────────────── */

static void choice_scroll_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app) app->core_scrolled = true;
}

static void choice_row_pressed_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app) app->core_scrolled = false;
}

static void choice_click_cb(lv_event_t *e)
{
    ModalCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    /* A swipe inside the scrollable core list must not launch the core that
     * happens to be under the finger when it is released. */
    if (app->choice_purpose == CHOICE_CORE && app->core_scrolled) {
        app->core_scrolled = false;
        return;
    }
    int values[8] = {0, 4, 5, 6, 7, 8};
    int value = ctx->index;
    if (app->choice_purpose == CHOICE_THEME) value = ctx->index;
    else if (app->choice_purpose == CHOICE_COLUMNS) value = values[ctx->index];
    else if (app->choice_purpose == CHOICE_SORT) value = ctx->index;
    else if (app->choice_purpose == CHOICE_UI_SCALE) {
        int vals[16];
        int n = app_ui_scale_values(vals, 16, app_ui_scale_max(app));
        value = (ctx->index >= 0 && ctx->index < n) ? vals[ctx->index] : 100;
    }
    else value = ctx->index;
    app_do_choice(app, ctx->index, value);
}

static void choice_default_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    ModalCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    if (ctx->index < 0 || ctx->index >= app->files_open_with_count) return;
    int app_index = app->files_open_with_apps[ctx->index];
    app_close_modal(app);
    app_files_open_with(app, app_index, true);
}

void ui_build_modal_choice(App *app)
{
    const char *opts[64];
    int n = 0;
    app->core_scrolled = false;
    if (app->choice_purpose == CHOICE_CORE) {
        n = app->core_item_count;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) opts[i] = app->core_items[i].label;
    } else if (app->choice_purpose == CHOICE_THEME) {
        n = theme_preset_count() + 1;
        for (int i = 0; i < n - 1 && i < 7; i++) opts[i] = theme_preset_name(i);
        opts[n - 1] = "自动（深色/浅色）";
    } else if (app->choice_purpose == CHOICE_COLUMNS) {
        n = 6;
        const char *o[] = {"自动（按窗口宽度）", "4 列", "5 列", "6 列", "7 列", "8 列"};
        for (int i = 0; i < 6; i++) opts[i] = o[i];
    } else if (app->choice_purpose == CHOICE_SORT) {
        n = 4;
        const char *o[] = {"名称 A-Z", "最近游玩", "最多游玩", "收藏优先"};
        for (int i = 0; i < 4; i++) opts[i] = o[i];
    } else if (app->choice_purpose == CHOICE_RENDERER) {
        n = 3;
        const char *o[] = {"OpenGL", "OpenGL ES", "VULKAN"};
        for (int i = 0; i < 3; i++) opts[i] = o[i];
    } else if (app->choice_purpose == CHOICE_LANGUAGE) {
        n = 2;
        opts[0] = "中文";
        opts[1] = "English";
    } else if (app->choice_purpose == CHOICE_EXTRACT_TARGET) {
        n = 2;
        opts[0] = "解压到当前目录";
        opts[1] = "解压到指定目录…";
    } else if (app->choice_purpose == CHOICE_OPEN_WITH) {
        n = app->files_open_with_count + (app->files_open_with_builtin ? 1 : 0);
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) {
            if (app->files_open_with_builtin) {
                if (i == 0) { opts[i] = "内置文本编辑器"; continue; }
                opts[i] = app->apps.apps[app->files_open_with_apps[i - 1]].name;
            } else {
                opts[i] = app->apps.apps[app->files_open_with_apps[i]].name;
            }
        }
    } else if (app->choice_purpose == CHOICE_ARCHIVE_FORMAT) {
        n = 3;
        opts[0] = "7Z";
        opts[1] = "ZIP";
        opts[2] = "TAR";
    } else if (app->choice_purpose == CHOICE_TIMEZONE) {
        n = app_timezone_option_count(app);
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) opts[i] = app_timezone_option_label(app, i);
    } else if (app->choice_purpose == CHOICE_UI_SCALE) {
        static char labels[16][24];
        int vals[16];
        int max = app_ui_scale_max(app);
        n = app_ui_scale_values(vals, 16, max);
        for (int i = 0; i < n; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%.2fx  (%d%%)",
                     (double)vals[i] / 100.0, vals[i]);
            opts[i] = labels[i];
        }
    } else {
        n = 4;
        const char *o[] = {"全部游戏", "仅收藏", "未游玩", "多人游戏"};
        for (int i = 0; i < 4; i++) opts[i] = o[i];
    }
    /* 超过 8 项时才使用滚动容器；短列表保持原有紧凑布局。 */
    int max_visible = 8;
    int phys_h = app->backend.height > 0 ? app->backend.height : app->layout.h;
    if (phys_h <= 260) max_visible = 2;
    else if (phys_h <= 360) max_visible = 5;
    else if (phys_h <= 520) max_visible = 6;
    int visible = n < max_visible ? n : max_visible;
    if (visible < 1) visible = 1;
    int list_h = visible * 56;
    /* LVGL 会在可视区底部多画一行，预留一行高度避免压住按键提示。 */
    int h = 160 + list_h;
    lv_obj_t *panel = ui_modal_panel(app, 600, h);
    ui_label(panel, app->modal_title, FONT_XL, PAL.text, 30, 26, 540, 34);
    lv_obj_t *line = ui_rect(panel, 30, 72, 540, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);

    int start_index = 0;
    if (n > visible) {
        start_index = app->choice_focus - visible / 2;
        if (start_index < 0) start_index = 0;
        if (start_index + visible > n) start_index = n - visible;
    }

    /* Core lists can be long; use a real scroll container so touch users can
     * reach every core and the trailing "all installed"/"back" rows. */
    lv_obj_t *list_parent = panel;
    lv_obj_t *scroll = NULL;
    int list_x = 30, list_y = 88;
    if (app->choice_purpose == CHOICE_CORE) {
        scroll = lv_obj_create(panel);
        lv_obj_remove_style_all(scroll);
        lv_obj_set_pos(scroll, ui_scale_x(30), ui_scale_y(88));
        lv_obj_set_size(scroll, ui_scale_x(540), ui_scale_y(list_h));
        lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(scroll, 0, 0);
        lv_obj_set_style_pad_all(scroll, 0, 0);
        lv_obj_set_style_radius(scroll, 0, 0);
        lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
        ui_scrollbar_style(scroll);
        lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(scroll, choice_scroll_cb, LV_EVENT_SCROLL, app);
        list_parent = scroll;
        list_x = 0;
        list_y = 0;
    }
    int first = scroll ? 0 : start_index;
    int last = scroll ? n : start_index + visible;
    lv_obj_t *focus_row = NULL;
    for (int i = first; i < last && i < n; i++) {
        bool sel = i == app->choice_focus;
        int ry = list_y + (scroll ? i * 56 : (i - start_index) * 56);
        lv_obj_t *row = ui_rect(list_parent, list_x, ry, 540, 48, 14,
                                sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 40 : 80, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, choice_row_pressed_cb, LV_EVENT_PRESSED, app);
        int text_x = 18;
        int text_w = 480;
        if (app->choice_purpose == CHOICE_OPEN_WITH) {
            bool builtin = app->files_open_with_builtin;
            bool is_builtin = builtin && i == 0;
            int list_index = builtin ? i - 1 : i;
            text_x = 54;
            text_w = 380;
            if (is_builtin) {
                ui_label(row, LV_SYMBOL_EDIT, FONT_MD, PAL.accent2,
                         14, 12, 28, 26);
            } else {
                int app_index = (list_index >= 0 &&
                                 list_index < app->files_open_with_count) ?
                                app->files_open_with_apps[list_index] : -1;
                AppEntry *ae = (app_index >= 0) ? &app->apps.apps[app_index] : NULL;
                char *src = (ae && ae->icon[0] && file_exists(ae->icon)) ?
                            ui_img_src(ae->icon) : NULL;
                if (src) {
                    lv_obj_t *icon = lv_image_create(row);
                    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_remove_style_all(icon);
                    lv_obj_set_pos(icon, 14, 10);
                    lv_obj_set_size(icon, 28, 28);
                    lv_image_set_src(icon, src);
                    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
                    free(src);
                } else {
                    ui_label(row, LV_SYMBOL_FILE, FONT_MD, PAL.text2,
                             14, 12, 28, 26);
                }
            }
            int def_index = builtin ? app->files_open_with_default + 1
                                    : app->files_open_with_default;
            if (is_builtin) {
                /* The built-in editor has no "set as default" action. */
            } else if (def_index >= 0 && i == def_index) {
                ui_label(row, "当前默认", FONT_XS, PAL.accent2,
                         386, 16, 146, 18);
            } else {
                lv_obj_t *def = ui_rect(row, 452, 11, 76, 26, 13, PAL.card_soft);
                lv_obj_set_style_bg_opa(def, 180, 0);
                lv_obj_set_style_border_width(def, 1, 0);
                lv_obj_set_style_border_color(def, lv_color_hex(PAL.border_focus), 0);
                lv_obj_set_style_border_opa(def, 90, 0);
                lv_obj_add_flag(def, LV_OBJ_FLAG_CLICKABLE);
                ui_label_center(def, "设为默认", FONT_XS, PAL.text2,
                                0, 5, 76, 16);
                ModalCtx *dctx = xmalloc(sizeof(*dctx));
                dctx->app = app;
                dctx->index = list_index;
                dctx->id[0] = '\0';
                lv_obj_add_event_cb(def, choice_default_cb, LV_EVENT_CLICKED, dctx);
                lv_obj_add_event_cb(def, modal_ctx_free, LV_EVENT_DELETE, dctx);
            }
        }
        ui_label(row, opts[i], FONT_MD, sel ? PAL.accent2 : PAL.text,
                 text_x, 12, text_w, 24);
        if (sel && app->choice_purpose != CHOICE_OPEN_WITH) {
            lv_obj_t *dot = ui_rect(row, 500, 14, 20, 20, 10, PAL.accent);
            lv_obj_set_style_bg_opa(dot, 230, 0);
        }
        ModalCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        ctx->id[0] = '\0';
        lv_obj_add_event_cb(row, choice_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, modal_ctx_free, LV_EVENT_DELETE, ctx);
        if (scroll && i == app->choice_focus)
            focus_row = row;
    }
    if (scroll) {
        lv_obj_update_layout(scroll);
        if (focus_row) lv_obj_scroll_to_view(focus_row, LV_ANIM_OFF);
    }
    const char *hint;
    if (app->choice_purpose == CHOICE_CORE)
        hint = app->core_picker_all ?
            "上下 选择 / {A} 确认 / {B} 返回兼容核心" :
            "上下 选择 / {A} 确认 / {B} 取消";
    else if (app->choice_purpose == CHOICE_OPEN_WITH)
        hint = (app->files_open_with_builtin &&
                app->files_open_with_count == 0)
            ? "上下 选择 / {A} 打开 / {B} 返回"
            : "上下 选择 / {A} 仅本次打开 / {Y} 设为默认 / {B} 返回";
    else
        hint = "上下 选择 / {A} 确认 / {B} 取消";
    char hintbuf[160];
    ui_button_hint(app, hintbuf, sizeof(hintbuf), hint);
    ui_label_center(panel, hintbuf, FONT_XS, PAL.text3, 0, h - 36, 600, 20);
}

/* ── Menu ─────────────────────────────────────────────────────────────────── */

static void menu_click_cb(lv_event_t *e)
{
    ModalCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    const char *labels[20];
    MenuAction acts[20];
    int n = 0;
    app_menu_labels(app, labels, acts, &n);
    if (ctx->index >= 0 && ctx->index < n) app_run_menu_action(app, acts[ctx->index]);
}

static void menu_info_close_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_close_modal(app);
}

static void ui_build_modal_menu_info(App *app)
{
    static const char *keys[5] = {"名称", "类型", "大小", "修改时间", "位置"};
    const char *vals[5] = {
        app->file_info_name, app->file_info_type, app->file_info_size,
        app->file_info_time, app->file_info_path,
    };
    int n = 5;
    int h = 130 + n * 50;
    lv_obj_t *panel = ui_modal_panel(app, 560, h);
    ui_label(panel, "文件属性", FONT_XL, PAL.text, 30, 26, 500, 34);
    lv_obj_t *line = ui_rect(panel, 30, 72, 500, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = ui_rect(panel, 30, 84 + i * 50, 500, 42, 12, PAL.card);
        lv_obj_set_style_bg_opa(row, 55, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        ui_label(row, keys[i], FONT_XS, PAL.text3, 18, 12, 84, 20);
        lv_obj_t *v = ui_label(row, vals[i], i == 0 ? FONT_MD : FONT_SM,
                               i == 0 ? PAL.accent2 : PAL.text,
                               110, i == 0 ? 9 : 11, 372, 24);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(row, menu_info_close_cb, LV_EVENT_CLICKED, app);
    }
    char hint[64];
    ui_button_hint(app, hint, sizeof(hint), "{A} / {B} / Esc 关闭");
    ui_label_center(panel, hint, FONT_XS, PAL.text3, 0, h - 34, 560, 20);
}

static int g_menu_scroll_y;

static void menu_scroll_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target(e);
    if (list) g_menu_scroll_y = (int)lv_obj_get_scroll_y(list);
}

void ui_build_modal_menu(App *app)
{
    if (app->menu_info) {
        ui_build_modal_menu_info(app);
        return;
    }    const char *labels[20];
    MenuAction acts[20];
    int n = 0;
    app_menu_labels(app, labels, acts, &n);
    /* Long menus (the file manager has up to 15 entries) used to be scaled
     * down until the whole list fitted, which made the text unreadable.
     * Cap the panel to the available height and scroll the rows instead. */
    UiLayout *L = &app->layout;
    int max_h = L->h - L->pad * 2;
    if (max_h < 200) max_h = 200;
    int h = 130 + n * 50;
    if (h > max_h) h = max_h;
    lv_obj_t *panel = ui_modal_panel(app, 560, h);
    ui_label(panel, "工具菜单", FONT_XL, PAL.text, 30, 26, 500, 34);
    lv_obj_t *line = ui_rect(panel, 30, 72, 500, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);
    int list_top = 84;
    int list_h = h - list_top - 46;
    if (list_h < 50) list_h = 50;
    lv_obj_t *list = ui_rect(panel, 30, list_top, 500, list_h, 0, PAL.card);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    ui_scrollbar_style(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(list, menu_scroll_cb, LV_EVENT_SCROLL, NULL);
    app->ui.modal_scroll = list;
    lv_obj_t *focus_row = NULL;
    for (int i = 0; i < n; i++) {
        bool sel = i == app->menu_focus;
        lv_obj_t *row = ui_rect(list, 0, i * 50, 500, 42, 12,
                                sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 38 : 65, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        ui_label(row, labels[i], FONT_MD, sel ? PAL.accent2 : PAL.text, 18, 9, 450, 24);
        if (acts[i] == MENU_QUIT) lv_obj_set_style_text_color(
            lv_obj_get_child(row, 0), lv_color_hex(PAL.danger), 0);
        ModalCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        ctx->id[0] = '\0';
        lv_obj_add_event_cb(row, menu_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, modal_ctx_free, LV_EVENT_DELETE, ctx);
        if (sel) focus_row = row;
    }
    if (focus_row) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, g_menu_scroll_y, LV_ANIM_OFF);
        int row_y = ui_scale_y(app->menu_focus * 50);
        int row_h = ui_scale_y(42);
        int view_h = lv_obj_get_height(list);
        int sy = (int)lv_obj_get_scroll_y(list);
        if (row_y < sy)
            lv_obj_scroll_to_y(list, row_y, LV_ANIM_OFF);
        else if (row_y + row_h > sy + view_h)
            lv_obj_scroll_to_y(list, row_y + row_h - view_h, LV_ANIM_OFF);
    }
    char hint[128];
    ui_button_hint(app, hint, sizeof(hint),
                   "{A} 执行 / 上下 滚动 / {B} 关闭");
    ui_label_center(panel, hint, FONT_XS, PAL.text3, 0, h - 34, 560, 20);
}

/* ── Roots ────────────────────────────────────────────────────────────────── */

static void roots_row_cb(lv_event_t *e)
{
    ModalCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    app->roots_focus = ctx->index;
    if (app->config.root_count > 0) {
        app_open_confirm(app, CONFIRM_REMOVE_ROOT, ctx->index,
                         "移除这个目录？", app->config.roots[ctx->index]);
    }
}

void ui_build_modal_roots(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 820, 520);
    modal_common_title(app, panel, "游戏目录", "太极会扫描这些目录下的平台子文件夹");
    if (app->config.root_count == 0) {
        ui_label(panel, "还没有添加目录。", FONT_MD, PAL.text2, 34, 140, 700, 30);
    } else {
        if (app->roots_focus >= app->config.root_count) app->roots_focus = app->config.root_count - 1;
        if (app->roots_focus < 0) app->roots_focus = 0;
        for (int i = 0; i < app->config.root_count && i < 8; i++) {
            bool sel = i == app->roots_focus;
            lv_obj_t *row = ui_rect(panel, 30, 116 + i * 44, 760, 38, 12,
                                    sel ? PAL.accent : PAL.card);
            lv_obj_set_style_bg_opa(row, sel ? 36 : 75, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            ui_label(row, app->config.roots[i], FONT_SM, sel ? PAL.accent2 : PAL.text, 14, 8, 730, 22);
            ModalCtx *ctx = xmalloc(sizeof(*ctx));
            ctx->app = app;
            ctx->index = i;
            ctx->id[0] = '\0';
            lv_obj_add_event_cb(row, roots_row_cb, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(row, modal_ctx_free, LV_EVENT_DELETE, ctx);
        }
    }
    char roots_hint[96];
    ui_button_hint(app, roots_hint, sizeof(roots_hint),
                   "{X} 添加目录 / {A} 移除选中目录 / {B} 关闭");
    ui_label(panel, roots_hint, FONT_XS, PAL.text3, 30, 474, 760, 20);
}

/* ── Mapping ──────────────────────────────────────────────────────────────── */

void ui_build_modal_mapping(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 700, 440);
    ui_label(panel, "手动按键映射", FONT_XL, PAL.text, 30, 26, 640, 34);
    ui_label(panel, "依次按下手柄上要绑定的按键。Esc 取消。", FONT_SM, PAL.text2, 30, 66, 640, 24);
    char step_a[64], step_b[64], step_x[64], step_y[64];
    snprintf(step_a, sizeof(step_a), "确认键（%s）",
             backend_button_confirm_label(&app->backend));
    snprintf(step_b, sizeof(step_b), "取消键（%s）",
             backend_button_cancel_label(&app->backend));
    snprintf(step_x, sizeof(step_x), "主菜单键（%s）",
             backend_button_west_label(&app->backend));
    snprintf(step_y, sizeof(step_y), "次菜单键（%s）",
             backend_button_north_label(&app->backend));
    const char *steps[] = {step_a, step_b, step_x, step_y};
    int x = 30;
    for (int i = 0; i < 4; i++) {
        int y = 104 + i * 58;
        bool cur = i == app->mapping_step;
        bool done = i < app->mapping_step;
        lv_obj_t *row = ui_rect(panel, x, y, 640, 46, 14,
                                cur ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, cur ? 36 : 75, 0);
        ui_label(row, steps[i], FONT_MD, cur ? PAL.accent2 : PAL.text, 18, 11, 300, 24);
        char val[64];
        if (done) snprintf(val, sizeof(val), "按钮 #%d", app->mapping_values[i]);
        else if (cur) snprintf(val, sizeof(val), "请按键...");
        else snprintf(val, sizeof(val), "等待");
        lv_obj_t *v = ui_label(row, val, FONT_SM, cur ? PAL.warning : PAL.text3, 380, 12, 240, 24);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    }
    ui_label(panel, "捕获后会自动保存并启用自定义映射", FONT_XS, PAL.text3,
             30, 396, 500, 20);
    lv_obj_t *mcancel = ui_rect(panel, 560, 380, 110, 40, 20, PAL.card);
    lv_obj_set_style_bg_opa(mcancel, 170, 0);
    lv_obj_set_style_border_width(mcancel, 1, 0);
    lv_obj_set_style_border_color(mcancel, lv_color_hex(PAL.border), 0);
    lv_obj_add_flag(mcancel, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(mcancel, "取消", FONT_SM, PAL.text2, 0, 8, 110, 22);
    lv_obj_add_event_cb(mcancel, text_cancel_cb, LV_EVENT_CLICKED, app);
    (void)panel_title;
}

/* ── Help ─────────────────────────────────────────────────────────────────── */

void ui_build_modal_help(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 840, 620);
    ui_label(panel, "操作帮助", FONT_XL, PAL.text, 30, 26, 720, 34);
    lv_obj_t *line = ui_rect(panel, 30, 72, 720, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);
    const char *text = app->layout.w < 800 ?
        "方向键 移动 / {A} 确认 / {B} 返回\n"
        "{A} 开始 / {B} 关闭或退一层 / {X} 搜索或编辑 / {Y} 菜单或新增\n"
        "{L1}/{R1} 切换栏目 / {L2}-{R2} 切换应用分类\n"
        "文件页 {L2} 隐藏文件 / {R2} 刷新\n"
        "{START} 工具菜单 / {BACK} 帮助\n"
        "无侧栏页面在首行按上进入顶部导航；有侧栏从侧栏首项按上\n"
        "长按电源键 打开电源菜单\n\n"
        "应用前台时按 {GUIDE} 返回太极\n"
        "前台程序默认暂停，可在应用卡片恢复\n\n"
        "F11 全屏 / F5 扫描 / Ctrl+Q 退出\n\n"
        "模拟器命令在 设置 至 模拟器 中配置。\n"
        "%ROM% %NAME% %PLATFORM% %DIR% 为占位符。" :
        "游戏库 / 应用\n"
        "  方向键        移动焦点\n"
        "  {A} / Enter   进入平台 / 启动或恢复应用\n"
        "  {B} / Esc     关闭弹窗或退一层\n"
        "  {X}           搜索 / 编辑应用\n"
        "  {Y} / {START} 工具菜单 / 新增应用\n"
        "  {L1} / {R1}   切换顶部栏目\n"
        "                Xbox: LB/RB  PS: L1/R1  Switch: L/R\n"
        "  {L2} / {R2}   应用页切换分类\n"
        "                文件页 {L2} 隐藏文件 / {R2} 刷新\n"
        "                Xbox: LT/RT  PS: L2/R2  Switch: ZL/ZR\n"
        "  方向键        应用页移动到分类栏后左右选择，再按上进入顶部导航\n"
        "  {BACK}        显示本帮助\n"
        "  第一行/侧栏首项  按上进入顶部导航，再向上进入状态栏\n"
        "  长按电源键    打开电源菜单\n\n"
        "应用\n"
        "  {GUIDE}       从外部程序返回太极\n"
        "  返回行为      默认暂停，可随时恢复\n"
        "  后台音乐      在应用设置中选择继续运行\n\n"
        "窗口与模拟器\n"
        "  F11 全屏 / F5 扫描 / Ctrl+Q 退出\n"
        "  设置 至 模拟器 中可为每个平台填写启动命令。\n"
        "  命令支持 %ROM% / %NAME% / %PLATFORM% / %DIR% 占位符。";
    char help_text[2048];
    ui_button_hint(app, help_text, sizeof(help_text), text);
    lv_obj_t *scroll = lv_obj_create(panel);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_pos(scroll, ui_scale_x(36), ui_scale_y(90));
    lv_obj_set_size(scroll, ui_scale_x(768), ui_scale_y(460));
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    ui_scrollbar_style(scroll);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *body = ui_label(scroll, help_text, FONT_MD, PAL.text2, 0, 0, 750, 600);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_text_line_space(body, 2, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    app->ui.modal_scroll = scroll;
    char close_hint[96];
    ui_button_hint(app, close_hint, sizeof(close_hint),
                   "上下 滚动 / {A} / {B} / Esc 关闭");
    ui_label_center(panel, close_hint, FONT_XS, PAL.text3, 0, 578, 840, 20);
    (void)app;
}


/* ── Editor key hints ─────────────────────────────────────────────────────── */

void ui_build_modal_editor_help(App *app)
{
    UiLayout *L = &app->layout;
    int max_h = L->h - L->pad * 2;
    if (max_h < 260) max_h = 260;
    int h = max_h < 392 ? max_h : 392;
    lv_obj_t *panel = ui_modal_panel(app, 640, h);
    ui_label(panel, "文本编辑器", FONT_XL, PAL.text, 30, 22, 580, 34);
    ui_label(panel, "手柄按键 / 键盘按键", FONT_XS, PAL.text3, 30, 58, 580, 18);
    lv_obj_t *line = ui_rect(panel, 30, 84, 580, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);

    static const struct { const char *keys; const char *desc; } rows[] = {
        { "方向键 / 左摇杆", "移动光标（左右可跨行）" },
        { "{A}",   "呼出屏幕键盘，输入直接插入光标位置" },
        { "{Y}",   "退格（删除光标前的字符）" },
        { "{X}",   "在光标处换行" },
        { "{B}",   "返回文件管理器" },
        { "{START}", "保存文件" },
        { "{BACK}", "显示本按键提示" },
        { "{L1} / {R1}", "上下翻 10 行" },
        { "{L2} / {R2}", "跳到首行 / 末行" },
    };
    int n = (int)(sizeof(rows) / sizeof(rows[0]));
    int y = 92;
    for (int i = 0; i < n; i++) {
        char keys[80];
        ui_button_hint(app, keys, sizeof(keys), rows[i].keys);
        ui_label(panel, keys, FONT_MD, PAL.accent2, 34, y, 220, 24);
        lv_obj_t *d = ui_label(panel, rows[i].desc, FONT_SM, PAL.text2,
                               260, y + 2, 340, 24);
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        y += 30;
    }
    char close_hint[64];
    ui_button_hint(app, close_hint, sizeof(close_hint), "{A} / {B} 关闭");
    ui_label_center(panel, close_hint, FONT_XS, PAL.text3, 0, h - 22, 640, 18);
}

/* ── Local SMB share settings ────────────────────────────────────────────── */

typedef struct {
    App *app;
    int index;
} SmbShareCtx;

static void smb_share_row_cb(lv_event_t *e)
{
    SmbShareCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    ctx->app->smb_ui.edit_focus = ctx->index;
    app_input(ctx->app, IN_CONFIRM, NULL);
}

static void smb_share_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

void ui_build_modal_smb_share(App *app)
{
    SmbUi *ui = &app->smb_ui;
    bool is_new = ui->edit_index < 0;
    SmbLocalShare temp;
    const SmbLocalShare *sh;
    if (is_new) {
        memset(&temp, 0, sizeof(temp));
        const char *base = strrchr(ui->pending_path, '/');
        base = base ? base + 1 : ui->pending_path;
        snprintf(temp.name, sizeof(temp.name), "%s", *base ? base : "share");
        snprintf(temp.path, sizeof(temp.path), "%s", ui->pending_path);
        temp.writable = ui->pending_writable;
        sh = &temp;
    } else {
        if (ui->edit_index >= ui->local_count) return;
        sh = &ui->local[ui->edit_index];
    }

    lv_obj_t *panel = ui_modal_panel(app, 700, 340);
    ui_label(panel, is_new ? "共享文件夹" : "共享设置", FONT_XL, PAL.text,
             30, 22, 640, 34);
    ui_label(panel, sh->name, FONT_MD, PAL.accent2, 30, 60, 640, 24);
    lv_obj_t *path = ui_label(panel, sh->path, FONT_XS, PAL.text3, 30, 88, 640, 18);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_obj_t *line = ui_rect(panel, 30, 116, 640, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);

    for (int i = 0; i < 2; i++) {
        bool focus = ui->edit_focus == i;
        lv_obj_t *row = ui_rect(panel, 30, 130 + i * 64, 640, 54, 12,
                                focus ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, focus ? 55 : 45, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        if (focus) ui_set_focus_ring(row, true, PAL.accent);
        int lh = ui_text_height(FONT_MD);
        if (i == 0) {
            ui_label(row, "访问权限", FONT_MD, PAL.text, 18,
                     (54 - lh) / 2, 300, lh);
            lv_obj_t *v = ui_label(row, sh->writable ? "访客读写" : "访客只读",
                                   FONT_SM, sh->writable ? PAL.warning : PAL.success,
                                   380, (54 - lh) / 2, 240, lh);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        } else {
            ui_label(row, is_new ? "创建共享" : "删除共享", FONT_MD,
                     is_new ? PAL.accent2 : PAL.danger, 18,
                     (54 - lh) / 2, 320, lh);
        }
        SmbShareCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        lv_obj_add_event_cb(row, smb_share_row_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, smb_share_ctx_free, LV_EVENT_DELETE, ctx);
    }

    char hintbuf[96];
    ui_button_hint(app, hintbuf, sizeof(hintbuf), "{A} 确认 / {B} 关闭");
    ui_label_center(panel, hintbuf, FONT_XS, PAL.text3, 0, 292, 700, 18);
}

/* ── Power menu ───────────────────────────────────────────────────────────── */

static void power_clicked(lv_event_t *e)
{
    ModalCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    ctx->app->power_focus = ctx->index;
    ui_update_power_focus(ctx->app);
    app_power_action(ctx->app, ctx->index);
}

void ui_update_power_focus(App *app)
{
    for (int i = 0; i < app->ui.power_button_count; i++) {
        lv_obj_t *btn = app->ui.power_buttons[i];
        if (!btn) continue;
        bool focused = i == app->power_focus;
        lv_obj_set_style_bg_opa(btn, focused ? 72 : 100, 0);
        lv_obj_set_style_border_opa(btn, focused ? 230 : 0, 0);
        lv_obj_set_style_shadow_opa(btn, focused ? 35 : 0, 0);
    }
}

void ui_build_modal_power(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 460, 430);
    ui_label(panel, "电源菜单", FONT_XL, PAL.text, 30, 22, 400, 36);
    ui_label(panel, "长按电源键打开", FONT_XS, PAL.text2, 30, 60, 400, 20);
    lv_obj_t *line = ui_rect(panel, 30, 90, 400, 1, 0, PAL.border);
    lv_obj_set_style_bg_opa(line, 150, 0);

    static const char *labels[4] = {"休眠", "重启", "关机", "登出"};
    static const char *icons[4] = {
        LV_SYMBOL_PAUSE, LV_SYMBOL_REFRESH, LV_SYMBOL_POWER, LV_SYMBOL_LEFT
    };
    app->ui.power_button_count = 0;
    int bw = 380, bh = 54, gap = 12, x = 40, y = 108;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = ui_rect(panel, x, y, bw, bh, 16, PAL.card);
        lv_obj_set_style_bg_opa(btn, 100, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(PAL.border_focus), 0);
        lv_obj_set_style_border_opa(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 10, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(PAL.accent), 0);
        lv_obj_set_style_shadow_opa(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        ui_label_center(btn, icons[i], FONT_LG, PAL.accent2, 8, (bh - 22) / 2, 42, 24);
        ui_label(btn, labels[i], FONT_MD, PAL.text, 58, (bh - 24) / 2, 180, 24);
        ModalCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->index = i;
        lv_obj_add_event_cb(btn, power_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, modal_ctx_free, LV_EVENT_DELETE, ctx);
        app->ui.power_buttons[app->ui.power_button_count++] = btn;
        y += bh + gap;
    }
    char power_hint[64];
    ui_button_hint(app, power_hint, sizeof(power_hint),
                   "上下选择 / {A} 确认 / {B} 关闭");
    ui_label_center(panel, power_hint, FONT_XS, PAL.text3,
                    0, 392, 460, 20);
    ui_update_power_focus(app);
}

/* ── Scan ─────────────────────────────────────────────────────────────────── */

void ui_build_modal_scan(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 460, 190);
    lv_obj_t *spinner = lv_spinner_create(panel);
    int pw = lv_obj_get_width(panel);
    int ph = lv_obj_get_height(panel);
    int ss = ui_clamp(pw * 42 / 460, 26, 42);
    lv_obj_set_size(spinner, ss, ss);
    lv_obj_set_pos(spinner, pw * 40 / 460, ph * 74 / 190);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(PAL.accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
    ui_label(panel, "正在扫描游戏库...", FONT_LG, PAL.text,
             104, 64, 300, 30);
    ui_label(panel, "识别平台、封面与游戏资料", FONT_SM, PAL.text2,
             104, 102, 300, 24);
    ui_label(panel, "文件较多时可能需要几秒", FONT_XS, PAL.text3,
             40, 146, 380, 20);
    (void)app;
}

/* ── OOBE date & time ─────────────────────────────────────────────────────── */

typedef struct {
    App *app;
    int field;
} DateFieldCtx;

static void oobe_date_field_cb(lv_event_t *e)
{
    DateFieldCtx *ctx = lv_event_get_user_data(e);
    if (ctx && ctx->app) app_oobe_date_set_field(ctx->app, ctx->field);
}

static void oobe_date_apply_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app) app_oobe_date_apply(app);
}

static void oobe_date_cancel_cb(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app) app_close_modal(app);
}

void ui_build_modal_oobe_date(App *app)
{
    lv_obj_t *panel = ui_modal_panel(app, 640, 360);
    ui_label(panel, tr("设置日期与时间"), FONT_XL, PAL.text, 30, 26, 580, 36);
    lv_obj_t *sub = ui_label(panel, tr("左右选择要修改的部分，上下调整数值；应用后会关闭自动时间同步。"),
                             FONT_XS, PAL.text3, 30, 70, 580, 20);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);

    static const char *units[5] = {"年", "月", "日", "时", "分"};
    for (int i = 0; i < 5; i++) {
        app->ui.oobe_date_boxes[i] = NULL;
        app->ui.oobe_date_values[i] = NULL;
    }
    int bw = 96, gap = 12, bh = 88;
    int total = 5 * bw + 4 * gap;
    int x0 = (640 - total) / 2;
    int y0 = 108;
    for (int i = 0; i < 5; i++) {
        bool sel = app->oobe_date_field == i;
        lv_obj_t *box = ui_rect(panel, x0 + i * (bw + gap), y0, bw, bh, 14,
                                sel ? PAL.accent : PAL.card_soft);
        lv_obj_set_style_bg_opa(box, sel ? 55 : 140, 0);
        lv_obj_set_style_border_width(box, sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(sel ? PAL.border_focus
                                                           : PAL.border), 0);
        lv_obj_set_style_border_opa(box, sel ? 220 : 90, 0);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        DateFieldCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->field = i;
        lv_obj_add_event_cb(box, oobe_date_field_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(box, modal_ctx_free, LV_EVENT_DELETE, ctx);

        char value[16];
        if (i == 0) snprintf(value, sizeof(value), "%04d", app->oobe_date[0]);
        else if (i == 1 || i == 2 || i == 3 || i == 4)
            snprintf(value, sizeof(value), "%02d", app->oobe_date[i]);

        lv_obj_t *v = ui_label_center(box, value, FONT_LG,
                                      sel ? PAL.text : PAL.text2,
                                      0, 16, bw, 34);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
        ui_label_center(box, tr(units[i]), FONT_XS, PAL.text3, 0, 56, bw, 18);
        app->ui.oobe_date_boxes[i] = box;
        app->ui.oobe_date_values[i] = v;
    }

    char preview[64];
    snprintf(preview, sizeof(preview), "%04d-%02d-%02d  %02d:%02d",
             app->oobe_date[0], app->oobe_date[1], app->oobe_date[2],
             app->oobe_date[3], app->oobe_date[4]);
    ui_label_center(panel, preview, FONT_MD, PAL.accent2, 0, 212, 640, 26);

    lv_obj_t *apply = ui_rect(panel, 150, 258, 150, 44, 22, PAL.accent);
    lv_obj_set_style_bg_opa(apply, 220, 0);
    lv_obj_add_flag(apply, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(apply, tr("应用修改"), FONT_MD, 0xFFFFFFFF, 0, 10, 150, 24);
    lv_obj_add_event_cb(apply, oobe_date_apply_cb, LV_EVENT_CLICKED, app);

    lv_obj_t *cancel = ui_rect(panel, 340, 258, 150, 44, 22, PAL.card);
    lv_obj_set_style_bg_opa(cancel, 170, 0);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(cancel, tr("取消"), FONT_MD, PAL.text2, 0, 10, 150, 24);
    lv_obj_add_event_cb(cancel, oobe_date_cancel_cb, LV_EVENT_CLICKED, app);

    char hint[160];
    ui_button_hint(app, hint, sizeof(hint),
                   tr("左右 选择 / 上下 调整 / {A} 应用 / {B} 取消"));
    ui_label_center(panel, hint, FONT_XS, PAL.text3, 0, 320, 640, 20);
}

void ui_oobe_date_focus_update(App *app)
{
    for (int i = 0; i < 5; i++) {
        lv_obj_t *box = app->ui.oobe_date_boxes[i];
        if (!box) continue;
        bool sel = i == app->oobe_date_field;
        lv_obj_set_style_bg_color(box, lv_color_hex(sel ? PAL.accent : PAL.card_soft), 0);
        lv_obj_set_style_bg_opa(box, sel ? 55 : 140, 0);
        lv_obj_set_style_border_width(box, sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(
            box, lv_color_hex(sel ? PAL.border_focus : PAL.border), 0);
        lv_obj_set_style_border_opa(box, sel ? 220 : 90, 0);
        if (app->ui.oobe_date_values[i])
            lv_obj_set_style_text_color(
                app->ui.oobe_date_values[i],
                lv_color_hex(sel ? PAL.text : PAL.text2), 0);
    }
}
