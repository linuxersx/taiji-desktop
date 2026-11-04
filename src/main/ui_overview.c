// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    int index;
} OverviewCtx;

static void overview_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void overview_clicked(lv_event_t *e)
{
    OverviewCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    App *app = ctx->app;
    if (app->overview_focus != ctx->index) {
        app->overview_focus = ctx->index;
        app_rebuild(app);
        audio_play(SND_NAV);
        return;
    }
    audio_play(SND_ENTER);
    app_activate_task(app, ctx->index);
}

static const char *task_glyph(const TaskEntry *t)
{
    if (t->kind == TASK_KIND_TAIJI) return "太";
    if (t->kind == TASK_KIND_MUSIC) return "乐";
    if (t->kind == TASK_KIND_APP) return "应";
    return "窗";
}

static void fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static lv_obj_t *overview_card(App *app, lv_obj_t *parent, TaskEntry *t,
                               int index, int x, int y, int w, int h,
                               int radius, bool selected, int dim)
{
    lv_obj_t *card = ui_rect(parent, x, y, w, h, radius, PAL.card);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    if (t->thumb[0]) {
        lv_obj_t *img = ui_image(card, t->thumb, 0, 0, w, h, radius, true);
        lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
    } else {
        ui_gradient(card, 0, 0, w, h, radius, PAL.elevated, PAL.card_soft);
        int glyph_h = h * 46 / 100;
        ui_label_center(card, task_glyph(t), w > 240 ? FONT_HERO : FONT_XXL,
                        0x55FFFFFF, 0, h * 8 / 100, w, glyph_h);
    }

    if (dim > 0) {
        lv_obj_t *shade = ui_rect(card, 0, 0, w, h, radius, 0x05070C);
        lv_obj_set_style_bg_opa(shade, dim, 0);
    }

    /* Name + state sit in a compact bar above the preview. */
    int top_h = ui_clamp(h * 20 / 100, 26, 74);
    lv_obj_t *top = ui_rect(card, 0, 0, w, top_h, 0, 0x05070C);
    lv_obj_set_style_bg_opa(top, 168, 0);

    char label[420];
    if ((t->kind == TASK_KIND_APP || t->kind == TASK_KIND_MUSIC) &&
        t->subtitle[0]) {
        snprintf(label, sizeof(label), "%s (%s)", t->title, t->subtitle);
    } else {
        snprintf(label, sizeof(label), "%s", t->title);
    }
    int pad_x = ui_clamp(w * 4 / 100, 8, 22);
    lv_font_t *title_font = h > 200 ? FONT_MD : FONT_SM;
    lv_obj_t *name = ui_label(card, label, title_font, 0xFFFFFFFF, pad_x,
                              (top_h - 22) / 2, w - pad_x * 2, 24);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    if (selected) {
        lv_obj_set_style_shadow_width(card, app->config.low_power ? 0 : 22, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(PAL.accent), 0);
        lv_obj_set_style_shadow_opa(card, 90, 0);
    }

    OverviewCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = index;
    lv_obj_add_event_cb(card, overview_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, overview_ctx_free, LV_EVENT_DELETE, ctx);
    return card;
}

void ui_build_overview(App *app)
{
    UiLayout *L = &app->layout;
    ui_screen_background(app, NULL, PAL.accent);
    app->ui.overview_stage = NULL;
    app->ui.overview_card_count = 0;

    int total = app->tasks.count;
    if (total <= 0) {
        int pw = ui_min(L->w - L->pad * 2, 520);
        int ph = ui_min(L->content_h, 200);
        int px = (L->w - pw) / 2;
        int py = L->content_y + (L->content_h - ph) / 2;
        lv_obj_t *panel = ui_rect(app->ui.root, px, py, pw, ph, 20, PAL.surface);
        lv_obj_set_style_bg_opa(panel, 220, 0);
        ui_label_center(panel, "没有可切换的任务", FONT_LG, PAL.text, 20, 30, pw - 40, 36);
        ui_label_center(panel, "按 B 返回主界面", FONT_SM, PAL.text2, 20, 74, pw - 40, 28);
        ui_footer_pill(app, "多任务", 0, "B 返回", "", "", "");
        return;
    }
    if (app->overview_focus < 0) app->overview_focus = 0;
    if (app->overview_focus >= total) app->overview_focus = total - 1;
    int focus = app->overview_focus;

    lv_obj_t *stage = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(stage);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_size(stage, ui_scale_x(L->w), ui_scale_y(L->h));
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(stage, 0, 0);
    app->ui.overview_stage = stage;

    int content_w = L->w - L->pad * 2;
    int card_h = L->content_h * 72 / 100;
    int card_w = card_h * 16 / 9;
    int max_w = content_w * 58 / 100;
    if (card_w > max_w) {
        card_w = max_w;
        card_h = card_w * 9 / 16;
    }
    if (card_w < 120) {
        card_w = 120;
        card_h = 68;
    }
    int center_x = L->w / 2 - card_w / 2;
    int center_y = L->content_y + (L->content_h - card_h) / 2;
    int radius = ui_clamp(ui_min(card_w, card_h) * 6 / 100, 10, 22);
    int side_w = card_w * 60 / 100;
    int side_h = card_h * 60 / 100;
    int gap = ui_clamp(card_w * 8 / 100, 8, 26);

    /* Far cards first so the focused card ends up on top. */
    for (int d = 2; d >= 1; d--) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int idx = focus + sign * d;
            if (idx < 0 || idx >= total) continue;
            int sx = sign > 0
                ? center_x + card_w + gap + (d - 1) * (side_w + gap / 2)
                : center_x - side_w - gap - (d - 1) * (side_w + gap / 2);
            if (sx + side_w < 0 || sx > L->w) continue;
            int sy = center_y + (card_h - side_h) / 2;
            lv_obj_t *c = overview_card(app, stage, &app->tasks.items[idx], idx,
                                        sx, sy, side_w, side_h,
                                        ui_clamp(radius * 80 / 100, 8, 20),
                                        false, d == 1 ? 90 : 150);
            if (app->ui.overview_card_count < 16)
                app->ui.overview_cards[app->ui.overview_card_count++] = c;
        }
    }

    lv_obj_t *main_card = overview_card(app, stage, &app->tasks.items[focus], focus,
                                        center_x, center_y, card_w, card_h,
                                        radius, true, 0);
    if (app->ui.overview_card_count < 16)
        app->ui.overview_cards[app->ui.overview_card_count++] = main_card;

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d / %d", focus + 1, total);
    lv_obj_t *indicator = ui_label(app->ui.root, count_buf, FONT_XS, PAL.text2,
                                   L->w - L->pad - 80, L->footer_y - 24, 80, 20);
    lv_obj_set_style_text_align(indicator, LV_TEXT_ALIGN_RIGHT, 0);

    ui_footer_pill(app, "多任务", total, "A 切换", "B 返回", "X 关闭", "");
    if (L->w < 500) {
        ui_label(app->ui.root, "X 关闭任务", FONT_XS, PAL.text2,
                 L->pad, L->footer_y - 24, 140, 20);
    }

    bool animate = app->overview_animate;
    app->overview_animate = false;
    if (animate && !app->config.reduce_motion) {
        lv_obj_set_style_opa(stage, 0, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, stage);
        lv_anim_set_exec_cb(&a, fade_exec);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}
