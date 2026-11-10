// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "i18n.h"
#include "catalog.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    int index;
    bool pressed;
    bool long_fired;
    uint32_t press_ms;
} GameClickCtx;

static const char *g_sort_names[] = {"名称", "最近游玩", "最多游玩", "收藏优先"};
static const char *g_filter_names[] = {"全部", "仅收藏", "未游玩", "多人"};

static void game_pressed(lv_event_t *e)
{
    GameClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    ctx->pressed = true;
    ctx->press_ms = lv_tick_get();
}

static void game_released(lv_event_t *e)
{
    GameClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->pressed) return;
    ctx->pressed = false;
    lv_indev_t *indev = lv_indev_active();
    if (indev && (lv_indev_get_scroll_obj(indev) ||
                  lv_indev_get_scroll_dir(indev) != LV_DIR_NONE ||
                  lv_indev_get_gesture_dir(indev) != LV_DIR_NONE))
        return;
    if (lv_tick_get() - ctx->press_ms < 450) return;
    App *app = ctx->app;
    ctx->long_fired = true;
    app->game_focus = ctx->index;
    Game *g = app_selected_game(app);
    if (g) snprintf(app->selected_game_id, sizeof(app->selected_game_id), "%s", g->id);
    ui_update_games(app);
    audio_play(SND_NAV_STRONG);
    app->modal = MODAL_DETAIL;
    ui_build_modal(app);
}

static void game_clicked(lv_event_t *e)
{
    GameClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->long_fired) {
        ctx->long_fired = false;
        return;
    }
    App *app = ctx->app;
    if (app->game_focus != ctx->index) {
        app->game_focus = ctx->index;
        Game *g = app_selected_game(app);
        if (g) snprintf(app->selected_game_id, sizeof(app->selected_game_id), "%s", g->id);
        ui_update_games(app);
        audio_play(SND_NAV);
        return;
    }
    Game *g = app_selected_game(app);
    if (g) app_launch_game(app, g);
}

static void game_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void back_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_goto_screen(app, SCREEN_HOME);
    audio_play(SND_BACK);
}

static void filter_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app->filter_mode = (app->filter_mode + 1) % 4;
    app->game_focus = 0;
    app_rebuild(app);
    audio_play(SND_NAV);
}

static void sort_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app->sort_mode = (app->sort_mode + 1) % 4;
    app->game_focus = 0;
    app_rebuild(app);
    audio_play(SND_NAV);
}

static void search_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app->modal = MODAL_SEARCH;
    app->search_buf[0] = '\0';
    app->search_focus = 0;
    ui_set_input_active(true);
    ui_build_modal(app);
    audio_play(SND_NAV_STRONG);
}

static void empty_add_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "",
                  "输入 ROM 根目录路径，例如 ~/ROMs");
}

static void apply_panel(App *app, Game *g)
{
    Ui *ui = &app->ui;
    if (!ui->game_cover || !ui->game_detail_title) return;
    if (!g) {
        lv_label_set_text(ui->game_detail_title, tr("没有游戏"));
        lv_label_set_text(ui->game_detail_meta, "");
        lv_label_set_text(ui->game_detail_desc, "");
        lv_label_set_text(ui->game_detail_stats, "");
        return;
    }
    lv_label_set_text(ui->game_detail_title, g->name);
    char meta[512];
    const char *platform = catalog_zh_name(g->platform_id);
    if (g->release_date && g->release_date[0]) {
        snprintf(meta, sizeof(meta), "%s  /  %s  /  %d 人", platform, g->release_date, g->players > 0 ? g->players : 1);
    } else {
        snprintf(meta, sizeof(meta), "%s  /  %d 人", platform, g->players > 0 ? g->players : 1);
    }
    lv_label_set_text(ui->game_detail_meta, meta);
    lv_label_set_text(ui->game_detail_desc,
                      g->description && g->description[0] ? g->description : "暂无游戏简介。");
    char stats[256];
    char *played = format_playtime(g->play_seconds);
    snprintf(stats, sizeof(stats), "游玩 %d 次  /  %s", g->play_count, played);
    free(played);
    lv_label_set_text(ui->game_detail_stats, stats);

    /* The cover widget is a wrapper containing an image. Replace its child. */
    lv_obj_t *wrap = ui->game_cover;
    int cw = lv_obj_get_width(wrap);
    int ch = lv_obj_get_height(wrap);
    if (lv_obj_get_child_count(wrap) > 0) lv_obj_clean(wrap);
    if (g->cover_path && g->cover_path[0] && file_exists(g->cover_path)) {
        char *src = ui_img_src(g->cover_path);
        lv_obj_t *img = lv_image_create(wrap);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style_all(img);
        lv_obj_set_size(img, cw, ch);
        lv_obj_center(img);
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
        ui_label_center(wrap, glyph, FONT_HERO, 0x55FFFFFF, 0, ch / 2 - 30, cw, 60);
    }
    if (ui->game_fav_badge) {
        if (g->favorite) {
            lv_obj_remove_flag(ui->game_fav_badge, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(ui->game_fav_badge, 220, 0);
            lv_obj_set_style_border_opa(ui->game_fav_badge, 255, 0);
        } else {
            lv_obj_add_flag(ui->game_fav_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void build_card(App *app, GameList *list, int idx, int x, int y, int cell_w, int img)
{
    Game *g = list->items[idx];
    int title_h = app->layout.game_title_h;
    lv_font_t *title_font = app->layout.w < 600 ? FONT_XS : (app->layout.w < 900 ? FONT_XS : FONT_SM);
    lv_obj_t *card = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, ui_scale_x(x), ui_scale_y(y));
    lv_obj_set_size(card, ui_scale_x(cell_w), ui_scale_y(img + title_h + 4));
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    int ix = (cell_w - img) / 2;
    lv_obj_t *wrap = ui_rect(card, ix, 0, img, img, 14, PAL.card);
    lv_obj_set_style_clip_corner(wrap, true, 0);
    if (g->cover_path && g->cover_path[0] && file_exists(g->cover_path)) {
        lv_obj_t *im = ui_image(wrap, g->cover_path, 0, 0, img, img, 14, true);
        lv_obj_set_style_bg_opa(im, LV_OPA_TRANSP, 0);
    } else {
        char glyph[8] = {0};
        int bytes = 1;
        unsigned char c = (unsigned char)g->name[0];
        if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        memcpy(glyph, g->name, (size_t)bytes);
        ui_label_center(wrap, glyph, FONT_XL, 0x66FFFFFF, 0, img / 2 - 20, img, 40);
    }
    if (g->favorite) {
        int fs = ui_clamp(img * 18 / 100, 18, 40);
        lv_obj_t *fav = ui_rect(wrap, img - fs - 8, 8, fs, fs, fs / 2, PAL.accent);
        lv_obj_set_style_bg_opa(fav, 230, 0);
        ui_label_center(fav, "藏", FONT_XS, 0xFFFFFFFF, 0, 2, fs, fs - 4);
    }
    /* Keep one clean, ellipsized line.  Letting long titles wrap made the
     * row heights uneven and let text bleed into the neighbouring card. */
    int title_line = ui_unscale_y(lv_font_get_line_height(title_font));
    int title_use = ui_min(title_h, title_line + 2);
    lv_obj_t *title = ui_label_center(card, g->name, title_font, PAL.text,
                                      0, img + 4, cell_w - 2, title_use);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(title, -1, 0);
    app->ui.game_cards[app->ui.game_card_count] = wrap;
    app->ui.game_titles[app->ui.game_card_count] = title;
    app->ui.game_card_count++;

    GameClickCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = idx;
    ctx->pressed = false;
    ctx->long_fired = false;
    ctx->press_ms = 0;
    lv_obj_add_event_cb(card, game_pressed, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(card, game_released, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(card, game_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, game_ctx_free, LV_EVENT_DELETE, ctx);
}

static void build_header(App *app, const char *collection_id, int count)
{
    UiLayout *L = &app->layout;
    int bh = ui_clamp(L->header_h * 55 / 100, 30, 40);
    lv_obj_t *back = ui_rect(app->ui.root, L->pad, L->content_y, bh, bh, bh / 2, PAL.surface);
    lv_obj_set_style_bg_opa(back, 170, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(PAL.border), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    ui_label_center(back, LV_SYMBOL_LEFT, FONT_MD, PAL.text, 0, (bh - 18) / 2, bh, 20);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, app);

    bool very_compact = L->w < 420;
    Platform *p = library_find_platform(&app->library, collection_id);
    char title[160];
    if (strcmp(collection_id, "favorites") == 0) snprintf(title, sizeof(title), "我的收藏");
    else if (strcmp(collection_id, "recent") == 0) snprintf(title, sizeof(title), "最近游玩");
    else if (strcmp(collection_id, "all") == 0) snprintf(title, sizeof(title), "全部游戏");
    else if (p) snprintf(title, sizeof(title), "%s", catalog_zh_name(p->id));
    else snprintf(title, sizeof(title), "%s", app->collection_name[0] ? app->collection_name : "游戏");

    int logo_w = ui_clamp(L->header_h * 22 / 10, 80, 260);
    int logo_h = ui_clamp(L->header_h * 6 / 10, 30, 90);
    int logo_x = L->pad + bh + 10;
    int title_x = logo_x;
    if (!very_compact && p && p->logo && file_exists(p->logo)) {
        app->ui.game_header_logo = ui_image(app->ui.root, p->logo, logo_x,
                                            L->content_y + (L->header_h - logo_h) / 2 - 2,
                                            logo_w, logo_h, 0, false);
        lv_obj_set_style_bg_opa(app->ui.game_header_logo, LV_OPA_TRANSP, 0);
        title_x = logo_x + logo_w + 12;
    } else {
        app->ui.game_header_logo = NULL;
        title_x = L->pad + bh + 12;
    }

    /* Right side actions, computed first so the title never runs under them.
     * On very narrow screens keep only search; filter/sort remain in the menu. */
    int cw = ui_clamp(L->w * 75 / 1000, 44, 100);
    int cgap = ui_clamp(L->w * 8 / 1000, 4, 8);
    int chips_total = very_compact ? cw : cw * 3 + cgap * 2;
    int chips_x = L->w - L->pad - chips_total;
    int chips_y = L->content_y + 4;
    int chips_h = ui_clamp(L->header_h * 5 / 10, 26, 34);
    int title_w = chips_x - title_x - 8;
    if (title_w < 80) title_w = 80;
    lv_font_t *title_font = L->w < 600 ? FONT_LG : FONT_XL;
    int title_line = ui_unscale_y(lv_font_get_line_height(title_font));
    app->ui.game_header_title = ui_label(app->ui.root, title, title_font, PAL.text,
                                         title_x, L->content_y - 2, title_w, title_line);
    lv_label_set_long_mode(app->ui.game_header_title, LV_LABEL_LONG_DOT);
    char sub[256];
    snprintf(sub, sizeof(sub), "%d 个游戏  /  %s  /  %s", count,
             g_sort_names[app->sort_mode % 4], g_filter_names[app->filter_mode % 4]);
    int sub_line = ui_unscale_y(lv_font_get_line_height(FONT_XS));
    int sub_gap = ui_clamp(L->header_h * 8 / 100, 6, 10);
    app->ui.game_header_count = ui_label(app->ui.root, sub, FONT_XS, PAL.text2,
                                         title_x, L->content_y + title_line + sub_gap,
                                         title_w, sub_line + 2);
    lv_label_set_long_mode(app->ui.game_header_count, LV_LABEL_LONG_DOT);

    const char *chips[3] = {"筛选", "排序", "搜索"};
    void (*cbs[3])(lv_event_t *) = {filter_clicked, sort_clicked, search_clicked};
    for (int i = 0; i < 3; i++) {
        if (very_compact && i != 2) continue;
        int chip_x = very_compact ? chips_x : chips_x + i * (cw + cgap);
        lv_obj_t *chip = ui_rect(app->ui.root, chip_x, chips_y,
                                 cw, chips_h, chips_h / 2, PAL.surface);
        lv_obj_set_style_bg_opa(chip, 150, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(PAL.border), 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        char label[64];
        if (i == 0) snprintf(label, sizeof(label), "%s", g_filter_names[app->filter_mode % 4]);
        else if (i == 1) snprintf(label, sizeof(label), "%s", g_sort_names[app->sort_mode % 4]);
        else snprintf(label, sizeof(label), "%s", chips[i]);
        int chip_text_h = ui_text_height(FONT_XS);
        ui_label_center(chip, label, FONT_XS,
                        i == 0 ? PAL.accent2 : PAL.text2,
                        0, (chips_h - chip_text_h) / 2, cw, chip_text_h);
        lv_obj_add_event_cb(chip, cbs[i], LV_EVENT_CLICKED, app);
    }
}

static void game_fav_badge_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    Game *g = app_selected_game(app);
    if (g) app_toggle_favorite(app, g);
}

static void build_cover_panel(App *app)
{
    UiLayout *L = &app->layout;
    if (L->panel_w <= 0 || !app->config.show_cover_panel) return;
    lv_obj_t *panel = ui_rect(app->ui.root, L->panel_x, L->panel_y,
                              L->panel_w, L->panel_h, 20, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 210, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);
    int inner = ui_clamp(L->panel_w * 5 / 100, 10, 18);
    int cw = L->panel_w - inner * 2;
    int ch = ui_clamp(L->panel_h * 52 / 100, 100, L->panel_h - 150);
    app->ui.game_cover = ui_rect(panel, inner, inner, cw, ch, 14, PAL.card_soft);
    lv_obj_set_style_clip_corner(app->ui.game_cover, true, 0);
    int ty = inner + ch + 8;
    int title_h = ui_text_height(FONT_LG) + 4;
    app->ui.game_detail_title = ui_label(panel, "", FONT_LG, PAL.text,
                                         inner, ty, cw, title_h);
    lv_label_set_long_mode(app->ui.game_detail_title, LV_LABEL_LONG_WRAP);
    int my = ty + title_h;
    int meta_h = ui_text_height(FONT_XS);
    app->ui.game_detail_meta = ui_label(panel, "", FONT_XS,
                                        PAL.is_light ? PAL.accent : PAL.accent2,
                                        inner, my, cw, meta_h);
    app->ui.game_detail_desc = ui_label(panel, "", FONT_XS, PAL.text2,
                                        inner, my + meta_h + 4, cw,
                                        L->panel_h - (my - L->panel_y) - meta_h - 56);
    lv_label_set_long_mode(app->ui.game_detail_desc, LV_LABEL_LONG_WRAP);
    int stats_h = ui_text_height(FONT_XS);
    app->ui.game_detail_stats = ui_label(panel, "", FONT_XS, PAL.text3,
                                         inner, L->panel_h - stats_h - 24,
                                         cw, stats_h);
    int bw = ui_clamp(L->panel_w * 12 / 100, 36, 48);
    app->ui.game_fav_badge = ui_rect(panel, L->panel_w - bw - inner, inner, bw, 28,
                                     bw / 2, PAL.accent);
    lv_obj_add_flag(app->ui.game_fav_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(app->ui.game_fav_badge, game_fav_badge_clicked,
                        LV_EVENT_CLICKED, app);
    lv_obj_set_style_bg_opa(app->ui.game_fav_badge, 0, 0);
    lv_obj_set_style_border_width(app->ui.game_fav_badge, 1, 0);
    lv_obj_set_style_border_color(app->ui.game_fav_badge, lv_color_hex(PAL.accent), 0);
    lv_obj_set_style_border_opa(app->ui.game_fav_badge, 0, 0);
    int fav_h = ui_text_height(FONT_XS);
    app->ui.game_fav_label = ui_label_center(app->ui.game_fav_badge, "藏",
                                             FONT_XS, 0xFFFFFFFF, 0,
                                             (28 - fav_h) / 2, bw, fav_h);
}

void ui_build_games(App *app, const char *collection_id)
{
    /* Callers pass app->collection_id here, so copy through a local buffer
     * first; snprintf() with the same source and destination is undefined and
     * used to blank the collection id after entering a platform. */
    char cid[sizeof(app->collection_id)];
    snprintf(cid, sizeof(cid), "%s", collection_id && *collection_id ? collection_id : "all");
    snprintf(app->collection_id, sizeof(app->collection_id), "%s", cid);
    Platform *p = library_find_platform(&app->library, cid);
    Collection collections[128];
    int nc = app_collections(app, collections, 128);
    const char *title = NULL;
    const char *cover = NULL;
    uint32_t accent = PAL.accent;
    for (int i = 0; i < nc; i++) {
        if (strcmp(collections[i].id, cid) == 0) {
            title = collections[i].name;
            cover = collections[i].cover;
            accent = collections[i].accent;
            break;
        }
    }
    GameList list = library_query(&app->library, cid,
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    if (title) snprintf(app->collection_name, sizeof(app->collection_name), "%s", title);
    else if (p) snprintf(app->collection_name, sizeof(app->collection_name), "%s", catalog_zh_name(p->id));

    if (app->game_focus >= list.count) app->game_focus = list.count > 0 ? list.count - 1 : 0;
    if (app->game_focus < 0) app->game_focus = 0;

    Game *sel = list.count ? list.items[app->game_focus] : NULL;
    ui_screen_background(app,
        sel && sel->cover_path && sel->cover_path[0] ? sel->cover_path : cover,
        accent);

    build_header(app, cid, list.count);
    if (list.count > 0) build_cover_panel(app);

    UiLayout *L = &app->layout;
    int cols = L->game_cols;
    int rows = L->game_rows;
    int per_page = L->game_page;
    int gap = ui_clamp(L->w * 10 / 1000, 4, 12);
    int cell_w = (L->game_grid_w - gap * (cols - 1)) / cols;
    int img = ui_min(cell_w - 6,
                     (L->game_grid_h - rows * L->game_title_h - (rows - 1) * gap) / rows);
    if (img < 28) img = 28;
    int row_h = img + L->game_title_h + gap;
    int page = app->game_focus / per_page;
    int start = page * per_page;
    app->game_page = page;

    app->ui.game_card_count = 0;
    app->ui.game_page_start = start;
    for (int i = 0; i < per_page && start + i < list.count; i++) {
        int c = i % cols;
        int r = i / cols;
        int cx = L->game_grid_x + c * (cell_w + gap);
        int cy = L->game_grid_y + r * row_h;
        build_card(app, &list, start + i, cx, cy, cell_w, img);
    }

    if (list.count == 0) {
        /* 空状态占满整个内容区，右侧详情面板不参与布局。 */
        int pw = L->w - L->pad * 2;
        int px = L->pad;
        int py = L->game_grid_y;
        int ph = L->footer_y - py - 4;
        if (ph < 80) ph = 80;
        lv_obj_t *panel_obj = ui_rect(app->ui.root, px, py, pw, ph, 24, PAL.surface);
        lv_obj_set_style_bg_opa(panel_obj, 220, 0);
        int empty_title_h = ui_text_height(FONT_XL);
        int empty_sub_h = ui_text_height(FONT_SM) + 4;
        ui_label_center(panel_obj, "这个分类还没有游戏", FONT_XL, PAL.text,
                        30, ph * 15 / 100, pw - 60, empty_title_h);
        ui_label_center(panel_obj, "添加 ROM 目录或立即扫描，太极会自动整理平台与封面。",
                        FONT_SM, PAL.text2, 30, ph * 38 / 100, pw - 60,
                        empty_sub_h);
        int bw = ui_min(pw - 80, 220);
        int bh = ui_clamp(ph * 18 / 100, 40, 48);
        int by = ph * 68 / 100;
        lv_obj_t *btn = ui_rect(panel_obj, (pw - bw) / 2, by, bw, bh, bh / 2, PAL.accent);
        lv_obj_set_style_bg_opa(btn, 240, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        int ks = ui_clamp(bh - 14, 16, 22);
        lv_obj_t *key = ui_rect(btn, 12, (bh - ks) / 2, ks, ks, ks / 2, 0xFFF5F9FF);
        int key_h = ui_text_height(FONT_XS);
        int btn_text_h = ui_text_height(FONT_MD);
        ui_label_center(key, backend_button_confirm_label(&app->backend),
                        FONT_XS, PAL.accent, 0, (ks - key_h) / 2, ks, key_h);
        ui_label_center(btn, "添加游戏目录", FONT_MD, 0xFFFFFFFF,
                        ks + 18, (bh - btn_text_h) / 2, bw - ks - 30,
                        btn_text_h);
        ui_set_focus_ring(btn, true, PAL.accent2);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_shadow_width(btn, 20, 0);
        lv_obj_set_style_shadow_opa(btn, 70, 0);
        lv_obj_set_style_shadow_spread(btn, 1, 0);
        lv_obj_add_event_cb(btn, empty_add_clicked, LV_EVENT_CLICKED, app);
        if (by + bh + 20 <= ph) {
            char hint[64];
            ui_button_hint(app, hint, sizeof(hint), "按 {A} 添加游戏目录");
            ui_label_center(panel_obj, hint, FONT_XS,
                            PAL.accent2, 0, by + bh + 4, pw,
                            ui_text_height(FONT_XS));
        }
    }

    ui_footer_pill(app, app->collection_name, list.count,
                    list.count == 0 ? "A 添加目录" : "A 开始",
                    "上下左右 移动", "X 详情", "Y 收藏");
    if (list.count > 0) {
        char pagebuf[32];
        snprintf(pagebuf, sizeof(pagebuf), "%d / %d", page + 1,
                 (list.count + per_page - 1) / per_page > 0 ?
                 (list.count + per_page - 1) / per_page : 1);
        app->ui.game_page_label = ui_label(app->ui.root, pagebuf, FONT_XS,
                                           PAL.text2, L->w - L->pad - 80,
                                           L->footer_y - 20, 80,
                                           ui_text_height(FONT_XS));
        lv_obj_set_style_text_align(app->ui.game_page_label,
                                    LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        app->ui.game_page_label = NULL;
    }

    apply_panel(app, sel);
    game_list_free(&list);
    ui_update_games(app);
}

void ui_update_games(App *app)
{
    GameList list = library_query(&app->library, app->collection_id,
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    if (list.count == 0) { game_list_free(&list); return; }
    if (app->game_focus >= list.count) app->game_focus = list.count - 1;
    if (app->game_focus < 0) app->game_focus = 0;
    UiLayout *L = &app->layout;
    int per_page = L->game_page;
    int page = app->game_focus / per_page;
    int start = page * per_page;
    if (start != app->ui.game_page_start) {
        game_list_free(&list);
        app_rebuild(app);
        return;
    }
    for (int i = 0; i < app->ui.game_card_count; i++) {
        int idx = start + i;
        bool focused = idx == app->game_focus;
        ui_set_card_focus(app->ui.game_cards[i], focused,
                          ui_focus_accent(idx));
        if (app->ui.game_titles[i]) {
            lv_obj_set_style_text_color(app->ui.game_titles[i],
                                        lv_color_hex(focused ? PAL.text : PAL.text2), 0);
        }
    }
    Game *sel = list.items[app->game_focus];
    if (sel) {
        snprintf(app->selected_game_id, sizeof(app->selected_game_id), "%s", sel->id);
        apply_panel(app, sel);
        if (!app->config.low_power && app->ui.bg_img &&
            sel->cover_path && sel->cover_path[0]) {
            char *src = ui_img_src(sel->cover_path);
            lv_image_set_src(app->ui.bg_img, src);
            free(src);
        }
    }
    ui_footer_update(app, app->collection_name, list.count);
    if (app->ui.game_page_label) {
        char pagebuf[32];
        int pages = (list.count + per_page - 1) / per_page;
        snprintf(pagebuf, sizeof(pagebuf), "%d / %d", page + 1, pages > 0 ? pages : 1);
        lv_label_set_text(app->ui.game_page_label, pagebuf);
    }
    game_list_free(&list);
}
