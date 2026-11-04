// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "catalog.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    int index;
} HomeClickCtx;

static void home_card_clicked(lv_event_t *e)
{
    HomeClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    if (app->home_focus != ctx->index) {
        app->home_focus = ctx->index;
        ui_update_home(app);
        audio_play(SND_NAV);
        return;
    }
    Collection cols[128];
    int n = app_collections(app, cols, 128);
    if (ctx->index < 0 || ctx->index >= n) return;
    app_enter_collection(app, cols[ctx->index].id);
    audio_play(SND_ENTER);
}

static void home_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void add_root_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    app_open_text(app, TEXT_ROOT, 0, "添加游戏目录",
                  "", "输入 ROM 根目录路径，例如 ~/ROMs 或 /Volumes/Games");
}

static void build_empty(App *app)
{
    UiLayout *L = &app->layout;
    int pw = L->w - L->pad * 2;
    int ph = L->content_h;
    int px = L->pad;
    int py = L->content_y;
    lv_obj_t *panel = ui_rect(app->ui.root, px, py, pw, ph, 24, PAL.surface);
    lv_obj_set_style_bg_opa(panel, 220, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);

    bool show_sub = ph >= 150;
    bool show_hint = ph >= 210;
    lv_font_t *title_font = ph < 190 ? FONT_LG : (ph < 250 ? FONT_XL : FONT_XXL);
    int title_line = lv_font_get_line_height(title_font);
    int title_y = ph < 190 ? 10 : ui_clamp(ph * 12 / 100, 10, 40);
    int title_h = title_line + 4;
    ui_label_center(panel, "还没有游戏", title_font, PAL.text,
                    20, title_y, pw - 40, title_h);

    lv_font_t *sub_font = ph < 250 ? FONT_XS : FONT_SM;
    int sub_line = lv_font_get_line_height(sub_font);
    int sub_y = title_y + title_h + 6;
    int sub_h = show_sub ? sub_line * 2 + 4 : 0;
    if (show_sub) {
        lv_obj_t *sub = ui_label_center(panel,
            "添加 ROM 根目录，太极会自动识别平台、封面与游戏资料。",
            sub_font, PAL.text2, 24, sub_y, pw - 48, sub_h);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    }

    int bw = ui_min(pw - 80, 220);
    int bh = ui_clamp(ph * 15 / 100, 34, 52);
    int by = show_hint ? ph - bh - 34 : ph - bh - 12;
    if (by < sub_y + sub_h + 8) by = sub_y + sub_h + 8;
    if (by + bh > ph - 6) by = ph - bh - 6;
    lv_obj_t *btn = ui_rect(panel, (pw - bw) / 2, by, bw, bh, bh / 2, PAL.accent);
    lv_obj_set_style_bg_opa(btn, 240, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    int ks = ui_clamp(bh - 14, 16, 22);
    lv_obj_t *key = ui_rect(btn, 12, (bh - ks) / 2, ks, ks, ks / 2, 0xFFF5F9FF);
    int key_h = ui_text_height(FONT_XS);
    int btn_text_h = ui_text_height(FONT_MD);
    ui_label_center(key, "A", FONT_XS, PAL.accent, 0, (ks - key_h) / 2,
                    ks, key_h);
    ui_label_center(btn, "添加游戏目录", FONT_MD, 0xFFFFFFFF,
                    ks + 18, (bh - btn_text_h) / 2, bw - ks - 30,
                    btn_text_h);
    ui_set_focus_ring(btn, true, PAL.accent2);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 20, 0);
    lv_obj_set_style_shadow_opa(btn, 70, 0);
    lv_obj_set_style_shadow_spread(btn, 1, 0);
    lv_obj_add_event_cb(btn, add_root_clicked, LV_EVENT_CLICKED, app);
    if (show_hint) {
        ui_label_center(panel, "按 Y 打开工具菜单", FONT_XS, PAL.text3,
                        20, ph - ui_text_height(FONT_XS) - 8, pw - 40,
                        ui_text_height(FONT_XS));
    }

    ui_footer_pill(app, "太极游戏库", 0, "A 添加目录", "上下左右 浏览", "X 搜索", "Y 工具");
}

static lv_obj_t *build_card(App *app, lv_obj_t *root, Collection *col, int index,
                            int x, int y, int w, int h)
{
    char *cover = col->cover[0] ? xstrdup(col->cover) : NULL;
    int radius = ui_clamp(ui_min(w, h) * 8 / 100, 10, 24);
    lv_obj_t *card = ui_rect(root, x, y, w, h, radius, PAL.card);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    bool draw_cover = cover && !app->config.low_power;
    if (draw_cover) {
        lv_obj_t *img = ui_image(card, cover, 0, 0, w, h, radius, true);
        lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
        lv_obj_t *dim = ui_rect(card, 0, 0, w, h, 0, 0x000000);
        lv_obj_set_style_bg_opa(dim, 125, 0);
    } else {
        ui_gradient(card, 0, 0, w, h, radius, PAL.elevated, PAL.card_soft);
    }

    lv_obj_t *tint = ui_gradient(card, 0, 0, w, h * 45 / 100, radius, col->accent, PAL.card);
    lv_obj_set_style_bg_opa(tint, 26, 0);

    bool tiny = h < 100;
    lv_font_t *title_font = h > 190 ? FONT_LG : (h > 140 ? FONT_MD : FONT_SM);
    lv_font_t *glyph_font = h > 190 ? FONT_HERO : (h > 140 ? FONT_XXL : FONT_XL);
    int logo_x = w * 10 / 100;
    int logo_y = tiny ? 4 : h * 12 / 100;
    int logo_w = w - logo_x * 2;
    int logo_h = tiny ? h * 40 / 100 : h * 38 / 100;
    if (col->logo[0] && file_exists(col->logo)) {
        lv_obj_t *lg = ui_image(card, col->logo, logo_x, logo_y, logo_w, logo_h, 0, false);
        lv_obj_set_style_bg_opa(lg, LV_OPA_TRANSP, 0);
    } else {
        char glyph[8] = {0};
        if (strcmp(col->id, "favorites") == 0) snprintf(glyph, sizeof(glyph), "藏");
        else if (strcmp(col->id, "recent") == 0) snprintf(glyph, sizeof(glyph), "近");
        else if (strcmp(col->id, "all") == 0) snprintf(glyph, sizeof(glyph), "全");
        else {
            const char *p = col->name;
            int bytes = 1;
            unsigned char c = (unsigned char)*p;
            if ((c & 0xE0) == 0xC0) bytes = 2;
            else if ((c & 0xF0) == 0xE0) bytes = 3;
            else if ((c & 0xF8) == 0xF0) bytes = 4;
            memcpy(glyph, p, (size_t)bytes);
        }
        ui_label_center(card, glyph, glyph_font, 0xFFFFFFFF, logo_x, logo_y,
                        logo_w, logo_h);
    }

    if (w >= 180 && h >= 130) {
        int badge_w = ui_clamp(w * 20 / 100, 40, 84);
        int badge_h = ui_clamp(h * 10 / 100, 20, 44);
        lv_obj_t *badge = ui_rect(card, w - badge_w - 8, 8, badge_w, badge_h,
                                  badge_h / 2, 0x000000);
        lv_obj_set_style_bg_opa(badge, 110, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(col->accent), 0);
        lv_obj_set_style_border_opa(badge, 130, 0);
        char badge_text[32];
        snprintf(badge_text, sizeof(badge_text), "%d", col->count);
        ui_label_center(badge, badge_text, FONT_XS, 0xFFFFFFFF, 0, 2,
                        badge_w, badge_h - 4);
    }

    int scrim_h = ui_clamp(h * 42 / 100, 30, 100);
    lv_obj_t *scrim = ui_gradient(card, 0, h - scrim_h, w, scrim_h, 0, 0x05070C, 0xFF05070C);
    lv_obj_set_style_bg_opa(scrim, 255, 0);
    int title_h = tiny ? 20 : ui_clamp(h * 14 / 100, 24, 56);
    ui_label(card, col->name, title_font, 0xFFFFFFFF, 12,
             h - title_h - (tiny ? 4 : 24), w - 24, title_h);
    if (!tiny) {
        ui_label(card, col->short_name, FONT_XS, 0xCCFFFFFF, 12, h - 24,
                 w - 24, 18);
    }

    HomeClickCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = index;
    lv_obj_add_event_cb(card, home_card_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, home_ctx_free, LV_EVENT_DELETE, ctx);
    free(cover);
    return card;
}

void ui_build_home(App *app)
{
    Collection cols[128];
    int total = app_collections(app, cols, 128);
    if (total <= 0) {
        ui_screen_background(app, NULL, PAL.accent);
        build_empty(app);
        return;
    }
    if (app->home_focus < 0) app->home_focus = 0;
    if (app->home_focus >= total) app->home_focus = total - 1;

    UiLayout *L = &app->layout;
    int per_page = L->home_page;
    int page = app->home_focus / per_page;
    int page_start = page * per_page;
    app->home_page = page;

    Collection *cur = &cols[app->home_focus];
    ui_screen_background(app, cur->cover[0] ? cur->cover : NULL, cur->accent);

    app->ui.home_card_count = 0;
    app->ui.home_page_start = page_start;
    for (int i = 0; i < per_page && page_start + i < total; i++) {
        int col = i % L->home_cols;
        int row = i / L->home_cols;
        lv_obj_t *card = build_card(app, app->ui.root, &cols[page_start + i],
                                    page_start + i,
                                    L->pad + col * (L->home_card_w + L->home_gap),
                                    L->content_y + row * (L->home_card_h + L->home_gap),
                                    L->home_card_w, L->home_card_h);
        app->ui.home_cards[app->ui.home_card_count++] = card;
    }

    char footer_title[160];
    snprintf(footer_title, sizeof(footer_title), "%s", cur->name);
    ui_footer_pill(app, footer_title, cur->count, "A 进入", "上下左右 浏览", "X 搜索", "Y 工具");
    char pagebuf[32];
    snprintf(pagebuf, sizeof(pagebuf), "%d / %d", page + 1, (total + per_page - 1) / per_page);
    app->ui.home_page_label = ui_label(app->ui.root, pagebuf, FONT_XS, PAL.text2,
                                       L->w - L->pad - 80, L->footer_y - 22, 80, 20);
    lv_obj_set_style_text_align(app->ui.home_page_label, LV_TEXT_ALIGN_RIGHT, 0);

    ui_update_home(app);
}

void ui_update_home(App *app)
{
    Collection cols[128];
    int total = app_collections(app, cols, 128);
    if (total <= 0) return;
    if (app->home_focus < 0) app->home_focus = 0;
    if (app->home_focus >= total) app->home_focus = total - 1;
    UiLayout *L = &app->layout;
    int per_page = L->home_page;
    int page = app->home_focus / per_page;
    int start = page * per_page;
    if (app->ui.home_page_start != start) {
        app_rebuild(app);
        return;
    }
    for (int i = 0; i < app->ui.home_card_count; i++) {
        int idx = start + i;
        ui_set_card_focus(app->ui.home_cards[i], idx == app->home_focus,
                          ui_focus_accent(idx));
    }
    Collection *cur = &cols[app->home_focus];
    ui_footer_update(app, cur->name, cur->count);
    if (!app->config.low_power && app->ui.bg_img && cur->cover[0]) {
        char *src = ui_img_src(cur->cover);
        lv_image_set_src(app->ui.bg_img, src);
        free(src);
    }
    if (app->ui.home_page_label) {
        char pagebuf[32];
        snprintf(pagebuf, sizeof(pagebuf), "%d / %d", page + 1,
                 (total + per_page - 1) / per_page);
        lv_label_set_text(app->ui.home_page_label, pagebuf);
    }
}
