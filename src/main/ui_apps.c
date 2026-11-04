// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "apps.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    int index;
} AppClickCtx;

typedef struct {
    App *app;
    int cat;
} AppCatCtx;

static void ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void scroll_settled(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_obj_invalidate(obj);
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_END) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent) lv_obj_invalidate(parent);
    }
}


static void app_card_clicked(lv_event_t *e)
{
    AppClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    app->app_on_categories = false;
    for (int i = 0; i < app->app_filter_count; i++) {
        if (app->app_filter[i] == ctx->index) { app->app_focus = i; break; }
    }
    audio_play(SND_ENTER);
    app_launch_external(app);
}

static void category_clicked(lv_event_t *e)
{
    AppCatCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    app_set_app_category(ctx->app, apps_category_id(ctx->cat));
    audio_play(SND_NAV_STRONG);
}

static void add_app_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    int idx = apps_add(&app->apps, "新应用", "true");
    if (idx < 0) return;
    app_set_app_category(app, "all");
    app->app_focus = app->app_filter_count > 0 ? app->app_filter_count - 1 : 0;
    app_open_app_editor(app, idx);
}

static void show_all_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_set_app_category(app, "all");
}

static const char *app_status_text(App *app, const AppEntry *entry)
{
    static char buf[32];
    if (app_task_state(app, entry->id, buf, sizeof(buf)))
        return buf;
    if (app->launcher.active && app->launcher.external && app->launcher.game_id &&
        strcmp(app->launcher.game_id, entry->id) == 0) {
        if (strcmp(app->runtime.foreground, "taiji") == 0 &&
            strcmp(app->runtime.app_state, "background") == 0)
            return "后台";
        if (app->ui_suspended) {
            if (strcmp(app->runtime.foreground, "taiji") == 0)
                return "暂停";
            return "运行";
        }
        return "前台";
    }
    snprintf(buf, sizeof(buf), "%s", apps_runtime_state_label(&app->runtime, entry));
    return buf;
}

static uint32_t app_status_color(const char *status)
{
    if (strcmp(status, "前台") == 0) return PAL.success;
    if (strcmp(status, "暂停") == 0) return PAL.accent2;
    if (strcmp(status, "运行") == 0 || strcmp(status, "后台") == 0) return PAL.warning;
    if (strcmp(status, "失败") == 0) return PAL.danger;
    return PAL.text3;
}

static int category_count(App *app, const char *id)
{
    int n = 0;
    for (int i = 0; i < app->apps.count; i++) {
        if (apps_category_matches(&app->apps.apps[i], id)) n++;
    }
    return n;
}

static lv_obj_t *build_category_nav(App *app, int x, int w, const char *label, bool right)
{
    UiLayout *L = &app->layout;
    lv_obj_t *nav = ui_rect(app->ui.root, x, L->app_cat_y, w, L->app_cat_h,
                            L->app_cat_h / 2, PAL.accent);
    lv_obj_set_style_bg_opa(nav, 30, 0);
    lv_obj_set_style_border_width(nav, 2, 0);
    lv_obj_set_style_border_color(nav, lv_color_hex(PAL.accent2), 0);
    lv_obj_set_style_border_opa(nav, 190, 0);
    if (!app->config.low_power) {
        lv_obj_set_style_shadow_width(nav, 12, 0);
        lv_obj_set_style_shadow_color(nav, lv_color_hex(PAL.accent2), 0);
        lv_obj_set_style_shadow_opa(nav, 28, 0);
    }
    char hint[32];
    if (right) snprintf(hint, sizeof(hint), "%s " LV_SYMBOL_RIGHT, label);
    else snprintf(hint, sizeof(hint), LV_SYMBOL_LEFT " %s", label);
    ui_label_center(nav, hint, w >= 48 ? FONT_SM : FONT_XS, PAL.accent2,
                    0, (L->app_cat_h - (w >= 48 ? 20 : 16)) / 2, w, 20);
    return nav;
}

static void build_category_tabs(App *app, lv_obj_t *bar)
{
    UiLayout *L = &app->layout;
    int cats = apps_category_count();
    int tab_w = ui_clamp(L->w * 13 / 100, 46, 110);
    int gap = ui_clamp(L->w * 8 / 1000, 3, 8);
    int x = 0;
    lv_obj_t *focus_tab = NULL;
    lv_obj_t *selected_tab = NULL;
    app->ui.app_cat_count = 0;
    for (int i = 0; i < cats; i++) {
        const char *id = apps_category_id(i);
        bool selected = strcmp(app->app_cat, id) == 0;
        bool focused = app->app_on_categories && app->app_cat_focus == i;
        int count = category_count(app, id);
        lv_obj_t *tab = ui_rect(bar, x, 0, tab_w, L->app_cat_h,
                                L->app_cat_h / 2,
                                selected ? PAL.accent : PAL.surface);
        lv_obj_set_style_bg_opa(tab, selected ? 46 : 120, 0);
        lv_obj_set_style_border_width(tab, 1, 0);
        lv_obj_set_style_border_color(tab,
            lv_color_hex(focused ? PAL.border_focus : (selected ? PAL.accent : PAL.border)), 0);
        lv_obj_set_style_border_opa(tab, focused || selected ? 180 : 90, 0);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        if (focused) {
            ui_set_focus_ring(tab, true, PAL.accent);
            /* Flat highlight: rolling category bars must not leave a shadow
             * trail behind the selected chip. */
            lv_obj_set_style_shadow_width(tab, 0, 0);
        }
        char label[64];
        if (tab_w >= 56) snprintf(label, sizeof(label), "%s %d", apps_category_label(i), count);
        else snprintf(label, sizeof(label), "%s", apps_category_label(i));
        ui_label_center(tab, label, tab_w >= 56 ? FONT_XS : FONT_XS,
                        selected ? PAL.text : PAL.text2, 0, (L->app_cat_h - 16) / 2, tab_w, 18);
        AppCatCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->cat = i;
        lv_obj_add_event_cb(tab, category_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(tab, ctx_free, LV_EVENT_DELETE, ctx);
        app->ui.app_cat_tabs[app->ui.app_cat_count++] = tab;
        if (focused) focus_tab = tab;
        if (selected) selected_tab = tab;
        x += tab_w + gap;
    }
    lv_obj_update_layout(bar);
    if (focus_tab) lv_obj_scroll_to_view(focus_tab, LV_ANIM_OFF);
    else if (selected_tab) lv_obj_scroll_to_view(selected_tab, LV_ANIM_OFF);
    lv_obj_invalidate(bar);
    lv_obj_invalidate(lv_screen_active());
}

static bool app_badge_wanted(const char *status)
{
    return status && (strcmp(status, "暂停") == 0 ||
                      strcmp(status, "后台") == 0);
}

static void app_status_badge_create(App *app, int slot, lv_obj_t *card,
                                    const char *status, int w, int h)
{
    if (!app_badge_wanted(status)) return;
    int badge_w = ui_clamp(w * 32 / 100, 42, 92);
    int badge_text_h = ui_text_height(FONT_XS);
    int badge_h = ui_clamp(h * 9 / 100, 18, 28);
    if (badge_h < badge_text_h + 4) badge_h = badge_text_h + 4;
    lv_obj_t *badge = ui_rect(card, w - badge_w - 8, 8, badge_w, badge_h,
                              badge_h / 2, app_status_color(status));
    lv_obj_set_style_bg_opa(badge, 36, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(app_status_color(status)), 0);
    lv_obj_set_style_border_opa(badge, 130, 0);
    app->ui.app_status[slot] = ui_label_center(badge, status, FONT_XS,
                              app_status_color(status), 0,
                              (badge_h - badge_text_h) / 2,
                              badge_w, badge_text_h);
}

static void build_app_card(App *app, AppEntry *entry, int global_index, int slot,
                           int x, int y, int w, int h)
{
    int radius = ui_clamp(ui_min(w, h) * 9 / 100, 12, 28);
    lv_obj_t *card = ui_rect(app->ui.app_grid ? app->ui.app_grid : app->ui.root,
                             x, y, w, h, radius, PAL.card);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(card, PAL.is_light ? 120 : 80, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    bool tight = h < 110;
    lv_font_t *title_font = h > 190 ? FONT_LG : (h > 140 ? FONT_MD : FONT_SM);
    int title_line = ui_text_height(title_font);
    int sub_line = ui_text_height(FONT_XS);
    int bottom_pad = tight ? 4 : 8;
    bool show_sub = !tight;
    int block_h = title_line + (show_sub ? sub_line + 4 : 0);
    int block_top = h - bottom_pad - block_h;

    /* 小卡片优先保证图标和文字不互相挤压；空间不足时先去掉副标题。 */
    int icon_y = tight ? 4 : ui_clamp(h * 9 / 100, 8, 26);
    if (block_top < icon_y + 18 && show_sub) {
        show_sub = false;
        block_h = title_line;
        block_top = h - bottom_pad - block_h;
    }
    int icon_size = ui_min(w - 24, tight ? h * 42 / 100 : h * 56 / 100);
    int icon_x = (w - icon_size) / 2;
    int icon_room = block_top - 4 - icon_y;
    if (icon_room < 18) icon_room = 18;
    if (icon_size > icon_room) icon_size = icon_room;
    if (entry->icon[0] && file_exists(entry->icon)) {
        lv_obj_t *img = ui_image(card, entry->icon, icon_x, icon_y, icon_size, icon_size, 18, false);
        lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
    } else {
        char glyph[8] = {0};
        const char *p = entry->name[0] ? entry->name : "?";
        int bytes = 1;
        unsigned char c = (unsigned char)*p;
        if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        memcpy(glyph, p, (size_t)bytes);
        lv_obj_t *disc = ui_rect(card, icon_x, icon_y, icon_size, icon_size,
                                 icon_size / 2, PAL.elevated);
        lv_obj_set_style_bg_opa(disc, 150, 0);
        lv_font_t *glyph_font = icon_size > 120 ? FONT_HERO :
                                (icon_size > 72 ? FONT_XXL :
                                 (icon_size > 48 ? FONT_XL : FONT_LG));
        ui_label_center(disc, glyph, glyph_font,
                        0xFFFFFFFF, 0, 0, icon_size, icon_size);
    }

    const char *status = app_status_text(app, entry);
    app->ui.app_status[slot] = NULL;
    app_status_badge_create(app, slot, card, status, w, h);

    int title_y = block_top;
    lv_obj_t *title = ui_label_center(card, entry->name, title_font, PAL.text,
                                      4, title_y, w - 8, title_line);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    app->ui.app_titles[slot] = title;

    const char *mode = entry->mode == APP_MODE_HANDOFF ? "太极退出" :
                       entry->mode == APP_MODE_RESIDENT ? "太极留在后台" : "自动";
    const char *cat = apps_category_label(apps_category_index(entry->category));
    char sub[96];
    snprintf(sub, sizeof(sub), "%s / %s%s", cat, mode,
             entry->background ? " / 后台" : "");
    if (show_sub) {
        int sub_y = h - bottom_pad - sub_line;
        lv_obj_t *sl = ui_label_center(card, sub, FONT_XS, PAL.text3,
                                       6, sub_y, w - 12, sub_line);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
    }

    AppClickCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = global_index;
    lv_obj_add_event_cb(card, app_card_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, ctx_free, LV_EVENT_DELETE, ctx);
    app->ui.app_cards[slot] = card;
    app->ui.app_card_indices[slot] = global_index;
    app->ui.app_card_count = slot + 1;
}

void ui_build_apps(App *app)
{
    UiLayout *L = &app->layout;
    if (app->app_reset_focus) {
        app->app_reset_focus = false;
        app->app_focus = 0;
        app->app_page = 0;
        app->app_on_categories = false;
    }
    app_filter_apps(app);
    ui_screen_background(app, NULL, PAL.accent);
    app->ui.app_card_count = 0;
    app->ui.app_cat_count = 0;
    app->ui.app_page_label = NULL;

    int count = app->app_filter_count;
    if (app->app_focus < 0) app->app_focus = 0;
    if (app->app_focus >= count && count > 0) app->app_focus = count - 1;
    app->app_page = 0;
    app->ui.app_page_start = 0;
    app->ui.app_grid = NULL;

    char sub[160];
    int cat_count = category_count(app, app->app_cat);
    snprintf(sub, sizeof(sub), "%s / %d 个应用 / Guide 或 Home 返回",
             app->app_cat[0] ? apps_category_label(apps_category_index(app->app_cat)) : "全部",
             cat_count);
    app->ui.app_grid_count = ui_label(app->ui.root, sub, FONT_XS, PAL.text2,
                                      L->pad, L->content_y,
                                      L->w - L->pad * 2, 22);

    /* Scale the shoulder pills with the content padding.  Absolute caps made
     * them shrink to half size on HiDPI, where the logical canvas is 2x. */
    int nav_w = ui_clamp(L->pad * 5 / 3, 36, 120);
    int nav_gap = ui_clamp(L->pad / 4, 4, 18);
    int nav_inset = ui_clamp(L->pad / 2, 8, 48);
    int nav_lx = L->pad + nav_inset;
    int nav_rx = L->w - L->pad - nav_w - nav_inset;
    build_category_nav(app, nav_lx, nav_w, backend_trigger_left_label(&app->backend), false);
    build_category_nav(app, nav_rx, nav_w, backend_trigger_right_label(&app->backend), true);

    lv_obj_t *catbar = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(catbar);
    lv_obj_set_pos(catbar, ui_scale_x(nav_lx + nav_w + nav_gap), ui_scale_y(L->app_cat_y));
    lv_obj_set_size(catbar, ui_scale_x(nav_rx - (nav_lx + nav_w + nav_gap)),
                    ui_scale_y(L->app_cat_h));
    lv_obj_set_style_pad_all(catbar, 0, 0);
    lv_obj_set_style_bg_opa(catbar, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(catbar, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(catbar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(catbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(catbar, scroll_settled, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(catbar, scroll_settled, LV_EVENT_SCROLL_END, NULL);
    build_category_tabs(app, catbar);

    if (count == 0) {
        int pw = ui_min(L->w - L->pad * 2, 560);
        int ph = ui_min(L->app_grid_h, 300);
        int px = (L->w - pw) / 2;
        int py = L->app_grid_y + (L->app_grid_h - ph) / 2;
        lv_obj_t *panel = ui_rect(app->ui.root, px, py, pw, ph, 24, PAL.surface);
        lv_obj_set_style_bg_opa(panel, 220, 0);
        const char *title = app->apps.count == 0 ? "还没有应用" : "这个分类还没有应用";
        bool show_sub = ph >= 150;
        lv_font_t *title_font = ph < 150 ? FONT_MD : FONT_XL;
        int title_line = lv_font_get_line_height(title_font);
        int title_y = ph < 150 ? 8 : ui_clamp(ph * 12 / 100, 10, 30);
        int title_h = title_line + 4;
        ui_label_center(panel, title, title_font, PAL.text,
                        20, title_y, pw - 40, title_h);
        lv_font_t *sub_font = ph < 220 ? FONT_XS : FONT_SM;
        int sub_line = lv_font_get_line_height(sub_font);
        int sub_y = title_y + title_h + 6;
        int sub_h = show_sub ? sub_line * 2 + 4 : 0;
        if (show_sub) {
            lv_obj_t *sub = ui_label_center(panel,
                app->apps.count == 0
                    ? "添加常用的播放器、工具或模拟器，启动后会进入前台独占。"
                    : "切换到“全部”，或编辑应用修改分类。",
                sub_font, PAL.text2, 30, sub_y, pw - 60, sub_h);
            lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        }
        int bw = ui_min(pw - 80, 220);
        int bh = ui_clamp(ph * 16 / 100, 34, 52);
        int by = ph - bh - 10;
        if (by < sub_y + sub_h + 8) by = sub_y + sub_h + 8;
        if (by + bh > ph - 6) by = ph - bh - 6;
        lv_obj_t *btn = ui_rect(panel, (pw - bw) / 2, by, bw, bh, bh / 2, PAL.accent);
        lv_obj_set_style_bg_opa(btn, 230, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        ui_label_center(btn, app->apps.count == 0 ? "添加应用" : "显示全部",
                        FONT_MD, 0xFFFFFFFF, 0, (bh - 20) / 2, bw, 24);
        lv_obj_add_event_cb(btn, app->apps.count == 0 ? add_app_clicked : show_all_clicked,
                            LV_EVENT_CLICKED, app);
    } else {
        lv_obj_t *grid = lv_obj_create(app->ui.root);
        lv_obj_remove_style_all(grid);
        lv_obj_set_pos(grid, ui_scale_x(L->app_grid_x), ui_scale_y(L->app_grid_y));
        lv_obj_set_size(grid, ui_scale_x(L->app_grid_w), ui_scale_y(L->app_grid_h));
        lv_obj_set_style_pad_all(grid, 0, 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_scroll_dir(grid, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(grid, scroll_settled, LV_EVENT_SCROLL, NULL);
        lv_obj_add_event_cb(grid, scroll_settled, LV_EVENT_SCROLL_END, NULL);
        app->ui.app_grid = grid;

        int gap = ui_clamp(L->w * 12 / 1000, 6, 24);
        for (int i = 0; i < count; i++) {
            int c = i % L->app_cols;
            int r = i / L->app_cols;
            int gi = app->app_filter[i];
            build_app_card(app, &app->apps.apps[gi], gi, i,
                           c * (L->app_card_w + gap),
                           r * (L->app_card_h + gap),
                           L->app_card_w, L->app_card_h);
        }
        lv_obj_update_layout(grid);
        lv_obj_scroll_to_y(grid, 0, LV_ANIM_OFF);
        if (app->ui.app_card_count > 0) {
            int focus = app->app_focus;
            if (focus < 0) focus = 0;
            if (focus >= app->ui.app_card_count) focus = app->ui.app_card_count - 1;
            lv_obj_scroll_to_view(app->ui.app_cards[focus], LV_ANIM_OFF);
            lv_obj_invalidate(grid);
            lv_obj_invalidate(lv_screen_active());
        }
    }

    ui_footer_pill(app, "应用", count, "A 启动", "上下左右 移动", "X 编辑", "Y 新增");
    app->ui.app_page_label = NULL;
    ui_update_apps(app);
}

void ui_update_apps(App *app)
{
    for (int i = 0; i < app->ui.app_card_count; i++) {
        int gi = app->ui.app_card_indices[i];
        if (gi < 0 || gi >= app->apps.count) continue;
        bool focused = i == app->app_focus;
        AppEntry *entry = &app->apps.apps[gi];
        ui_set_card_focus(app->ui.app_cards[i], focused, ui_focus_accent(gi));
        if (app->ui.app_titles[i]) {
            lv_obj_set_style_text_color(app->ui.app_titles[i],
                                        lv_color_hex(focused ? PAL.text : PAL.text2), 0);
        }
        const char *status = app_status_text(app, entry);
        if (!app_badge_wanted(status)) {
            if (app->ui.app_status[i]) {
                lv_obj_t *badge = lv_obj_get_parent(app->ui.app_status[i]);
                app->ui.app_status[i] = NULL;
                if (badge) lv_obj_del(badge);
            }
        } else if (!app->ui.app_status[i]) {
            lv_obj_t *card = app->ui.app_cards[i];
            if (card) {
                int vw = ui_unscale_x(lv_obj_get_width(card));
                int vh = ui_unscale_y(lv_obj_get_height(card));
                app_status_badge_create(app, i, card, status, vw, vh);
            }
        } else {
            lv_label_set_text(app->ui.app_status[i], status);
            lv_obj_t *badge = lv_obj_get_parent(app->ui.app_status[i]);
            if (badge) {
                lv_obj_set_style_border_color(badge,
                    lv_color_hex(app_status_color(status)), 0);
                lv_obj_set_style_bg_color(badge,
                    lv_color_hex(app_status_color(status)), 0);
            }
        }
        if (focused) lv_obj_scroll_to_view(app->ui.app_cards[i], LV_ANIM_OFF);
    }
    /* Focus rings can leave a shadow trail when cards scroll; repaint the
     * whole screen once per focus move so nothing is left behind. */
    lv_obj_invalidate(lv_screen_active());
    if (app->ui.app_grid_count) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s / %d 个应用 / Guide 或 Home 返回",
                 apps_category_label(apps_category_index(app->app_cat)),
                 category_count(app, app->app_cat));
        lv_label_set_text(app->ui.app_grid_count, sub);
    }
    ui_footer_update(app, "应用", app->app_filter_count);
}

static void app_edit_fill_values(const AppEntry *a, char values[11][640])
{
    snprintf(values[0], 640, "%s", a->name);
    snprintf(values[1], 640, "%s", a->command);
    snprintf(values[2], 640, "%s", a->icon[0] ? a->icon : "自动文字图标");
    int cat = apps_category_index(a->category);
    if (cat == 0) cat = apps_category_count() - 1;
    snprintf(values[3], 640, "%s", apps_category_label(cat));
    snprintf(values[4], 640, "%s", a->mode == APP_MODE_HANDOFF ? "太极退出" :
             a->mode == APP_MODE_RESIDENT ? "太极留在后台" : "自动");
    snprintf(values[5], 640, "%s", a->on_return == APP_ON_RETURN_RUN ? "继续运行" :
             a->on_return == APP_ON_RETURN_CLOSE ? "关闭" : "暂停");
    snprintf(values[6], 640, "%s", a->backend == APP_BACKEND_WAYLAND ? "Wayland" :
             a->backend == APP_BACKEND_X11 ? "X11" : "自动");
    snprintf(values[7], 640, "%s", a->heavy ? "是" : "否");
    snprintf(values[8], 640, "%s", a->background ? "不抢前台" : "显示前台");
    snprintf(values[9], 640, "%s", a->terminal == APP_TERMINAL_YES ? "是" :
             a->terminal == APP_TERMINAL_NO ? "否" : "自动");
    snprintf(values[10], 640, "删除“%s”", a->name);
}

void ui_build_modal_app(App *app)
{
    if (app->app_edit_index < 0 || app->app_edit_index >= app->apps.count) return;
    AppEntry *a = &app->apps.apps[app->app_edit_index];
    lv_obj_t *panel = ui_modal_panel(app, 760, 560);
    int w = 760;
    int h = 560;
    int inner = ui_clamp(w * 4 / 100, 14, 32);
    int edit_title_h = ui_text_height(FONT_XL);
    int edit_sub_h = ui_text_height(FONT_XS);
    ui_label(panel, "应用设置", FONT_XL, PAL.text, inner, 12,
             w - inner * 2, edit_title_h);
    ui_label(panel, "上下 选择 / 左右 修改 / A 编辑 / B 保存", FONT_XS,
             PAL.text3, inner, 12 + edit_title_h, w - inner * 2, edit_sub_h);

    const char *labels[11] = {
        "名称", "启动命令", "图标路径", "分类", "启动方式", "返回太极时",
        "显示后端", "重型程序", "启动时", "终端模式", "删除应用"
    };
    char values[11][640];
    app_edit_fill_values(a, values);

    int top = 82;
    int list_h = h - top - 42;
    int rh = ui_clamp(list_h * 18 / 100, 42, 64);
    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, ui_scale_x(inner), ui_scale_y(top));
    lv_obj_set_size(list, ui_scale_x(w - inner * 2), ui_scale_y(list_h));
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    app->ui.app_edit_list = list;
    app->ui.app_edit_row_count = 11;

    int rw = w - inner * 2;
    for (int row = 0; row < 11; row++) {
        bool sel = row == app->app_edit_row;
        lv_obj_t *r = ui_rect(list, 0, row * (rh + 4), rw, rh, 12,
                              sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(r, sel ? 34 : 70, 0);
        lv_obj_set_style_border_width(r, sel ? 1 : 0, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(PAL.accent2), 0);
        lv_obj_set_style_border_opa(r, sel ? 130 : 0, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        int label_h = ui_text_height(FONT_SM);
        int value_h = ui_text_height(FONT_XS);
        int row_text_h = label_h > value_h ? label_h : value_h;
        ui_label(r, labels[row], FONT_SM,
                 row == 10 ? PAL.danger : PAL.text,
                 12, (rh - row_text_h) / 2, rw * 35 / 100, label_h);
        lv_obj_t *v = ui_label(r, values[row], FONT_XS,
                               row == 10 ? PAL.danger : PAL.text2,
                               rw * 36 / 100, (rh - row_text_h) / 2,
                               rw * 61 / 100 - 12, value_h);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        app->ui.app_edit_rows[row] = r;
        app->ui.app_edit_values[row] = v;
    }
    if (app->app_edit_row >= 0 && app->app_edit_row < 11 &&
        app->ui.app_edit_rows[app->app_edit_row])
        lv_obj_scroll_to_view(app->ui.app_edit_rows[app->app_edit_row], LV_ANIM_OFF);
}

void ui_update_modal_app(App *app)
{
    if (!app || app->app_edit_index < 0 || app->app_edit_index >= app->apps.count)
        return;
    AppEntry *a = &app->apps.apps[app->app_edit_index];
    char values[11][640];
    app_edit_fill_values(a, values);
    for (int row = 0; row < app->ui.app_edit_row_count && row < 11; row++) {
        lv_obj_t *r = app->ui.app_edit_rows[row];
        if (!r) continue;
        bool sel = row == app->app_edit_row;
        lv_obj_set_style_bg_color(r, lv_color_hex(sel ? PAL.accent : PAL.card), 0);
        lv_obj_set_style_bg_opa(r, sel ? 34 : 70, 0);
        lv_obj_set_style_border_width(r, sel ? 1 : 0, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(PAL.accent2), 0);
        lv_obj_set_style_border_opa(r, sel ? 130 : 0, 0);
        if (app->ui.app_edit_values[row])
            lv_label_set_text(app->ui.app_edit_values[row], values[row]);
    }
    if (app->app_edit_row >= 0 && app->app_edit_row < app->ui.app_edit_row_count &&
        app->ui.app_edit_rows[app->app_edit_row])
        lv_obj_scroll_to_view(app->ui.app_edit_rows[app->app_edit_row], LV_ANIM_ON);
}
