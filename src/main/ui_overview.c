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
    /* Android-style: one tap on any task card opens it.  Defer the screen
     * change until after LVGL's event dispatch so the card object is not
     * deleted while its own CLICKED handler is running. */
    audio_play(SND_ENTER);
    app->pending_overview_activate = ctx->index;
}

typedef struct {
    App *app;
    int index;
} OverviewCloseCtx;

static bool g_overview_closing = false;

static void close_anim_exec(void *var, int32_t v)
{
    lv_obj_t *card = var;
    int h = lv_obj_get_height(card);
    int opa = 255 - 255 * v / 1000;
    if (opa < 0) opa = 0;
    lv_obj_set_style_translate_y(card, -((h + ui_scale_y(40)) * v / 1000), 0);
    lv_obj_set_style_opa(card, (lv_opa_t)opa, 0);
}

static void close_anim_completed(lv_anim_t *a)
{
    OverviewCloseCtx *ctx = lv_anim_get_user_data(a);
    if (!ctx || !ctx->app) return;
    /* Do not rebuild the UI from inside an animation callback. Queue the
     * close and let ui_tick() run it on the next frame. */
    ctx->app->overview_close_pending = ctx->index;
}

static void close_anim_deleted(lv_anim_t *a)
{
    OverviewCloseCtx *ctx = lv_anim_get_user_data(a);
    g_overview_closing = false;
    free(ctx);
}

static void overview_close_animate(App *app, int index)
{
    lv_obj_t *card = app->ui.overview_main_card;
    if (!card) return;
    OverviewCloseCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->index = index;
    g_overview_closing = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, close_anim_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_completed_cb(&a, close_anim_completed);
    lv_anim_set_deleted_cb(&a, close_anim_deleted);
    lv_anim_start(&a);
}

static void overview_background_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    audio_play(SND_BACK);
    app->pending_overview_close = true;
}

static void overview_gesture(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t dir = indev ? lv_indev_get_gesture_dir(indev) : LV_DIR_NONE;
    /* A swipe must not turn into a click on release. */
    if (indev) lv_indev_wait_release(indev);

    int n = app->tasks.count;
    if (n <= 0) return;
    if (g_overview_closing || app->overview_close_pending >= 0) return;
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        if (n <= 1) return;
        int focus = app->overview_focus + (dir == LV_DIR_LEFT ? 1 : -1);
        focus = (focus + n) % n;
        if (focus == app->overview_focus) return;
        app->overview_slide_dir = (dir == LV_DIR_LEFT) ? 1 : -1;
        app->pending_overview_focus = focus;
        app->overview_kill_armed = false;
        audio_play(SND_NAV);
    } else if (dir == LV_DIR_TOP) {
        int focus = app->overview_focus;
        if (focus < 0 || focus >= n) return;
        if (app->tasks.items[focus].kind == TASK_KIND_TAIJI) {
            ui_toast(app, "不能关闭太极");
            return;
        }
        audio_play(SND_BACK);
        overview_close_animate(app, focus);
    }
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

static void slide_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
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
        char back_hint[64];
        ui_button_hint(app, back_hint, sizeof(back_hint), "按 {B} 返回主界面");
        ui_label_center(panel, back_hint, FONT_SM, PAL.text2, 20, 74, pw - 40, 28);
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
    /* Tapping empty space acts like Android's back gesture. */
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage, overview_background_clicked, LV_EVENT_CLICKED, app);
    /* Stop the default gesture bubbling at the stage, otherwise LVGL forwards
     * the gesture to the screen and this handler never runs. */
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(stage, overview_gesture, LV_EVENT_GESTURE, app);
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
    app->ui.overview_main_card = main_card;
    if (app->ui.overview_card_count < 16)
        app->ui.overview_cards[app->ui.overview_card_count++] = main_card;

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d / %d", focus + 1, total);
    lv_obj_t *indicator = ui_label(app->ui.root, count_buf, FONT_XS, PAL.text2,
                                   L->w - L->pad - 80, L->footer_y - 24, 80, 20);
    lv_obj_set_style_text_align(indicator, LV_TEXT_ALIGN_RIGHT, 0);

    ui_footer_pill(app, "多任务", total, "A 切换", "B 返回", "X 关闭", "");
    if (L->w < 500) {
        char close_hint[40];
        snprintf(close_hint, sizeof(close_hint), "%s 关闭任务",
                 backend_button_west_label(&app->backend));
        ui_label(app->ui.root, close_hint, FONT_XS, PAL.text2,
                 L->pad, L->footer_y - 24, 140, 20);
    }

    int slide = app->overview_slide_dir;
    app->overview_slide_dir = 0;
    bool animate = app->overview_animate;
    app->overview_animate = false;
    if (slide && !app->config.reduce_motion) {
        int off = ui_scale_x(L->w * 35 / 100) * slide;
        lv_obj_set_style_translate_x(stage, off, 0);
        lv_obj_set_style_opa(stage, 0, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, stage);
        lv_anim_set_exec_cb(&a, slide_exec);
        lv_anim_set_values(&a, off, 0);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        lv_anim_init(&a);
        lv_anim_set_var(&a, stage);
        lv_anim_set_exec_cb(&a, fade_exec);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else if (animate && !app->config.reduce_motion) {
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
