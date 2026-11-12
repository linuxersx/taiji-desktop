// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "external_osk.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static float g_sx = 1.0f;
static float g_sy = 1.0f;
static float g_base_x = 1.0f;
static float g_base_y = 1.0f;
static bool g_low_power = false;
static bool g_ui_building = false;

static int sxv(int v) { return (int)lroundf((float)v * g_sx * g_base_x); }
static int syv(int v) { return (int)lroundf((float)v * g_sy * g_base_y); }
static int ui_vx(int v) { return (int)lroundf((float)v / g_base_x); }
static int ui_vy(int v) { return (int)lroundf((float)v / g_base_y); }

static lv_obj_t *g_clock_label;
static lv_obj_t *g_controller_icon;
static bool g_controller_connected;
static lv_obj_t *g_wifi_label;
static lv_obj_t *g_bt_label;
static lv_obj_t *g_battery_label;
static lv_obj_t *g_battery_percent;
static lv_obj_t *g_volume_label;
static lv_obj_t *g_volume_percent;
static lv_obj_t *g_toast_obj;
static uint32_t g_last_clock;

static lv_obj_t *status_pad_icon(lv_obj_t *parent, int x, int y, int w, int h,
                                 bool connected);

/* ── Basic helpers ─────────────────────────────────────────────────────────── */

lv_obj_t *ui_obj(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    /* Decorative rectangles must not swallow taps from the clickable card,
     * row or button above them.  Interactive objects add CLICKABLE back. */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    x = sxv(x); y = syv(y); w = sxv(w); h = syv(h);
    if (radius > 0) radius = (int)lroundf((float)radius *
        (g_sx * g_base_x + g_sy * g_base_y) * 0.5f);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_rect(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    return ui_obj(parent, x, y, w, h, radius, color);
}

lv_obj_t *ui_gradient(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                      uint32_t top, uint32_t bottom)
{
    lv_obj_t *o = ui_obj(parent, x, y, w, h, radius, top);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_font_t *font,
                   uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, text ? tr(text) : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, sxv(x), syv(y));
    lv_obj_set_size(l, sxv(w), syv(h));
    return l;
}

lv_obj_t *ui_label_center(lv_obj_t *parent, const char *text, lv_font_t *font,
                          uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = ui_label(parent, text, font, color, x, y, w, h);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

char *ui_img_src(const char *path)
{
    if (!path || !*path) return NULL;
    char *out = xmalloc(strlen(path) + 3);
    memcpy(out, "A:", 2);
    strcpy(out + 2, path);
    return out;
}

lv_obj_t *ui_image(lv_obj_t *parent, const char *path, int x, int y, int w, int h,
                   int radius, bool cover)
{
    lv_obj_t *wrap = ui_rect(parent, x, y, w, h, radius, PAL.card_soft);
    lv_obj_set_style_clip_corner(wrap, true, 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
    if (!path || !*path || !file_exists(path)) {
        return wrap;
    }
    char *src = ui_img_src(path);
    lv_obj_t *img = lv_image_create(wrap);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style_all(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(img, sxv(w), syv(h));
    lv_obj_center(img);
    lv_image_set_src(img, src);
    lv_image_set_inner_align(img, cover ? LV_IMAGE_ALIGN_COVER : LV_IMAGE_ALIGN_CONTAIN);
    free(src);
    return wrap;
}

lv_obj_t *ui_round_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    return ui_rect(parent, x, y, w, h, radius, color);
}

/* 焦点描边随机取色：同一次运行里同一项颜色稳定，避免刷新时闪烁；
 * 重新启动后种子变化，配色也随之变化。 */
uint32_t ui_focus_accent(int index)
{
    static uint32_t seed;
    if (seed == 0)
        seed = (uint32_t)time(NULL) ^ ((uint32_t)clock() << 16) ^ 0x9E3779B9u;
    uint32_t x = seed + (uint32_t)index * 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    uint16_t hue = (uint16_t)(x % 360u);
    uint8_t sat = (uint8_t)(70u + ((x >> 9) % 21u));
    uint8_t val = (uint8_t)(88u + ((x >> 17) % 13u));
    return lv_color_to_u32(lv_color_hsv_to_rgb(hue, sat, val));
}

void ui_set_focus_ring(lv_obj_t *obj, bool focused, uint32_t accent)
{
    if (!obj) return;
    /* Keep the focus cue consistent and theme-aware.  Random per-object hues
     * could blend into card backgrounds and made the ring hard to track. */
    uint32_t ring = focused ? PAL.border_focus : accent;
    /* Draw the ring as an outline rather than a border.  A border is part of
     * the content box, so switching it on shifted absolutely positioned
     * children (icons, labels) by the ring width and made the ring look
     * off-centre.  outline_pad = -width keeps the stroke inside the object
     * bounds, so the geometry matches the old border exactly. */
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 4, 0);
    lv_obj_set_style_outline_pad(obj, -4, 0);
    lv_obj_set_style_outline_color(obj, lv_color_hex(ring), 0);
    lv_obj_set_style_outline_opa(obj, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(obj, (focused && !g_low_power) ? 24 : 0, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(ring), 0);
    lv_obj_set_style_shadow_opa(obj, (focused && !g_low_power) ? 90 : 0, 0);
    lv_obj_set_style_shadow_spread(obj, (focused && !g_low_power) ? 2 : 0, 0);
}

void ui_set_card_focus(lv_obj_t *obj, bool focused, uint32_t accent)
{
    (void)accent;
    if (!obj) return;
    uint32_t ring = focused ? PAL.border_focus : PAL.border;
    /* Same trick as ui_set_focus_ring: keep the 1px card border so the
     * content box never moves, and paint the thicker focus stroke with an
     * outline that is pulled inside the card bounds. */
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(ring), 0);
    lv_obj_set_style_border_opa(obj, focused ? LV_OPA_COVER : (PAL.is_light ? LV_OPA_30 : LV_OPA_40), 0);
    lv_obj_set_style_outline_width(obj, focused ? 5 : 0, 0);
    lv_obj_set_style_outline_pad(obj, -5, 0);
    lv_obj_set_style_outline_color(obj, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_outline_opa(obj, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(obj, (focused && !g_low_power) ? 26 : 0, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(ring), 0);
    lv_obj_set_style_shadow_opa(obj, (focused && !g_low_power) ? 90 : 0, 0);
    lv_obj_set_style_shadow_spread(obj, (focused && !g_low_power) ? 2 : 0, 0);
}

/* Scrollbars stay hidden while a list is idle and fade back in for a moment
 * whenever the content scrolls (touch or D-pad).  The bar is drawn just
 * outside the object so it never covers a row's focus ring. */
typedef struct {
    lv_obj_t *obj;
    lv_timer_t *timer;
    uint32_t last_scroll;
} ScrollbarFade;

static void scrollbar_fade_delete(lv_event_t *e)
{
    ScrollbarFade *f = lv_event_get_user_data(e);
    if (!f) return;
    if (f->timer) lv_timer_del(f->timer);
    f->timer = NULL;
    f->obj = NULL;
    free(f);
}

static void scrollbar_fade_tick(lv_timer_t *t)
{
    ScrollbarFade *f = lv_timer_get_user_data(t);
    if (!f || !f->obj) return;
    if (lv_tick_get() - f->last_scroll < 900) return;
    if (lv_obj_get_style_bg_opa(f->obj, LV_PART_SCROLLBAR) != LV_OPA_TRANSP)
        lv_obj_set_style_bg_opa(f->obj, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
}

static void scrollbar_fade_scroll(lv_event_t *e)
{
    ScrollbarFade *f = lv_event_get_user_data(e);
    if (!f || !f->obj) return;
    f->last_scroll = lv_tick_get();
    lv_obj_set_style_bg_opa(f->obj, 120, LV_PART_SCROLLBAR);
}

void ui_scrollbar_style(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(obj, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(PAL.border_focus), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 3, LV_PART_SCROLLBAR);
    /* Keep the track inside the list, 4px clear of the focus ring stroke
     * (the ring is drawn on the outer 4px of each row). */
    lv_obj_set_style_pad_right(obj, 8, LV_PART_SCROLLBAR);
    ScrollbarFade *f = xmalloc(sizeof(*f));
    f->obj = obj;
    f->last_scroll = 0;
    lv_obj_add_event_cb(obj, scrollbar_fade_scroll, LV_EVENT_SCROLL, f);
    lv_obj_add_event_cb(obj, scrollbar_fade_delete, LV_EVENT_DELETE, f);
    f->timer = lv_timer_create(scrollbar_fade_tick, 200, f);
}

void ui_scroll_into_view(lv_obj_t *obj, bool allow_anim)
{
    if (!obj) return;
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (!parent) return;
    lv_area_t oa, pa;
    lv_obj_get_coords(obj, &oa);
    lv_obj_get_content_coords(parent, &pa);
    if (oa.x1 >= pa.x1 && oa.x2 <= pa.x2 &&
        oa.y1 >= pa.y1 && oa.y2 <= pa.y2)
        return;
    lv_obj_scroll_to_view(obj,
        (allow_anim && !g_ui_building) ? LV_ANIM_ON : LV_ANIM_OFF);
}

void ui_init(Ui *ui)
{
    memset(ui, 0, sizeof(*ui));
    g_clock_label = NULL;
    g_controller_icon = NULL;
    g_controller_connected = false;
    g_wifi_label = NULL;
    g_bt_label = NULL;
    g_battery_label = NULL;
    g_battery_percent = NULL;
    g_volume_label = NULL;
    g_volume_percent = NULL;
    g_toast_obj = NULL;
}

int ui_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int ui_min(int a, int b) { return a < b ? a : b; }
int ui_scale_x(int v) { return sxv(v); }
void ui_set_draw_scale(float scale) { g_sx = scale; g_sy = scale; }
int ui_scale_y(int v) { return syv(v); }
int ui_text_height(lv_font_t *font) {
    return ui_unscale_y(lv_font_get_line_height(font)) + 2;
}
int ui_unscale_x(int v) { return ui_vx(v); }
int ui_unscale_y(int v) { return ui_vy(v); }

void ui_layout_update(App *app)
{
    UiLayout *L = &app->layout;
    int phys_w = app->backend.width > 0 ? app->backend.width : 1280;
    int phys_h = app->backend.height > 0 ? app->backend.height : 720;
    int ui_percent = app_effective_ui_scale(app);
    if (ui_percent < 100) ui_percent = 100;
    if (ui_percent > 300) ui_percent = 300;
    int ui_max = app_ui_scale_max(app);
    if (ui_percent > ui_max) ui_percent = ui_max;
    int w = (int)lroundf((float)phys_w * 100.0f / (float)ui_percent);
    int h = (int)lroundf((float)phys_h * 100.0f / (float)ui_percent);
    if (w < 160) w = 160;
    if (h < 120) h = 120;
    g_base_x = (float)phys_w / (float)w;
    g_base_y = (float)phys_h / (float)h;
    L->w = w;
    L->h = h;
    L->pad = ui_clamp(ui_min(w, h) * 5 / 100, 10, 72);

    /* Fonts scale with the physical screen, then the user UI scale is applied
     * on top. Layout metrics below use the virtual size instead. */
    float fs = (phys_h <= 360) ? 0.85f : (phys_h <= 520) ? 1.0f :
               (phys_h <= 800) ? 1.2f : (phys_h <= 1100) ? 1.5f :
               (phys_h <= 1600) ? 2.0f : 2.6f;
    theme_set_scale(fs * (g_base_x + g_base_y) * 0.5f);
    g_low_power = app->config.low_power;
    L->font_scale_x100 = (int)lroundf(fs * 100.0f);

    L->top_y = ui_clamp(h * 18 / 1000, 6, 28);
    L->top_h = ui_clamp(h * 68 / 1000, 34, 120);
    L->top_x = L->pad;
    L->top_w = w - L->pad * 2;
    L->nav_gap = ui_clamp(w * 8 / 1000, 6, 12);
    L->status_w = ui_clamp(w * 30 / 100, 130, 380);
    L->nav_w = 0; /* nav items size themselves from their text */

    L->footer_h = ui_clamp(h * 72 / 1000, 36, 120);
    L->footer_y = h - L->footer_h - ui_clamp(h * 22 / 1000, 4, 14);
    int extra = h > 720 ? (h - 720) * 55 / 100 : 0;
    L->footer_w = ui_min(w - L->pad * 2, 640 + extra);
    L->footer_x = (w - L->footer_w) / 2;

    L->content_y = L->top_y + L->top_h + ui_clamp(h * 12 / 1000, 4, 10);
    L->content_h = L->footer_y - L->content_y - ui_clamp(h * 8 / 1000, 4, 8);
    L->header_h = ui_clamp(h * 9 / 100, 40, 140);
    int header_min = ui_unscale_y(lv_font_get_line_height(FONT_XL) +
                                  lv_font_get_line_height(FONT_XS) + 14);
    if (L->header_h < header_min) L->header_h = header_min;

    /* Home platform grid */
    int cols = w < 600 ? 3 : (w < 1600 ? 4 : 5);
    int gap = ui_clamp(w * 12 / 1000, 6, 30);
    int card_w = (w - L->pad * 2 - gap * (cols - 1)) / cols;
    int rows = 2;
    if (L->content_h > card_w * 255 / 100) rows = 3;
    int card_h = (L->content_h - gap * (rows - 1)) / rows;
    if (card_h > card_w * 3 / 2) card_h = card_w * 3 / 2;
    if (card_h < card_w * 55 / 100) card_h = card_w * 55 / 100;
    L->home_cols = cols;
    L->home_rows = rows;
    L->home_gap = gap;
    L->home_card_w = card_w;
    L->home_card_h = card_h;
    L->home_page = cols * rows;

    /* Game grid */
    bool panel = (w >= 980) && app->config.show_cover_panel;
    L->panel_w = panel ? ui_clamp(w * 30 / 100, 260, 800) : 0;
    gap = ui_clamp(w * 10 / 1000, 4, 12);
    L->game_grid_x = L->pad;
    L->game_grid_y = L->content_y + L->header_h + gap;
    L->game_grid_w = w - L->pad * 2 - (panel ? L->panel_w + gap : 0);
    L->game_grid_h = L->footer_y - L->game_grid_y - gap;
    int min_card = w < 600 ? 64 : w < 900 ? 78 : w < 1400 ? 110 : 140;
    int gcols = ui_clamp(L->game_grid_w / (min_card + gap), 2, 8);
    L->game_title_h = ui_clamp(h * 45 / 1000, 20, 60);
    int grows;
    if (L->game_grid_h >= 3 * (70 + L->game_title_h + gap)) grows = 3;
    else if (L->game_grid_h >= 2 * (54 + L->game_title_h + gap)) grows = 2;
    else grows = 1;
    if (app->grid_columns >= 2 && app->grid_columns <= 8) gcols = app->grid_columns;
    L->game_cols = gcols;
    L->game_rows = grows;
    L->game_page = gcols * grows;
    L->panel_x = w - L->pad - L->panel_w;
    L->panel_y = L->content_y;
    L->panel_h = L->footer_y - L->content_y - 24;

    /* App launcher grid: no cover panel, large touch-friendly cards. */
    gap = ui_clamp(w * 12 / 1000, 6, 24);
    L->app_grid_x = L->pad;
    /* The app screen already has a top-bar tab; do not repeat a large title. */
    L->app_cat_y = L->content_y + (app->screen == SCREEN_APPS ? 26 : L->header_h);
    L->app_cat_h = ui_clamp(h * 6 / 100, 28, 96);
    L->app_grid_y = L->app_cat_y + L->app_cat_h + gap;
    L->app_grid_w = w - L->pad * 2;
    L->app_grid_h = L->footer_y - L->app_grid_y - gap;
    int app_min = w < 600 ? 92 : w < 900 ? 112 : w < 1400 ? 150 : 190;
    int acols = ui_clamp((L->app_grid_w + gap) / (app_min + gap), 2, 6);
    int arows;
    if (L->app_grid_h >= 3 * (app_min + gap)) arows = 3;
    else if (L->app_grid_h >= 2 * (app_min + gap)) arows = 2;
    else arows = 1;
    L->app_cols = acols;
    L->app_rows = arows;
    L->app_page = acols * arows;
    L->app_card_w = (L->app_grid_w - gap * (acols - 1)) / acols;
    L->app_card_h = (L->app_grid_h - gap * (arows - 1)) / arows;

    /* Settings */
    L->rail_w = ui_clamp(w * 22 / 100, 96, 480);
    L->rail_x = L->pad;
    L->rail_y = L->content_y;
    L->rail_h = L->content_h;
    L->rail_gap = L->rail_h < 320 ? 2 : 4;
    L->rail_row_h = ui_clamp((L->rail_h - 16 - L->rail_gap * 5) / 6, 20, 110);
    L->settings_panel_x = L->pad + L->rail_w + gap;
    L->settings_panel_y = L->content_y;
    L->settings_panel_w = w - L->pad - L->settings_panel_x;
    L->settings_panel_h = L->content_h;
    L->settings_header_h = ui_clamp(h * 15 / 100, 64, 160);
    if (ui_percent > 100 && L->settings_header_h < 100) L->settings_header_h = 100;
    L->settings_row_h = ui_clamp((L->settings_panel_h - L->settings_header_h) / 8, 28, 84);
    L->settings_rows = ui_clamp((L->settings_panel_h - L->settings_header_h) / L->settings_row_h, 3, 8);
}

/* ── Background ───────────────────────────────────────────────────────────── */

void ui_screen_background(App *app, const char *cover_path, uint32_t accent)
{
    UiLayout *L = &app->layout;
    lv_obj_t *root = app->ui.root;
    lv_obj_set_style_bg_color(root, lv_color_hex(PAL.bg), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(PAL.bg_bottom), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    if (!app->config.low_power) {
        int orb = ui_min(L->w, L->h);
        lv_obj_t *orb1 = ui_rect(root, -orb * 20 / 100, -orb * 25 / 100, orb * 72 / 100, orb * 72 / 100,
                                 LV_RADIUS_CIRCLE, accent);
        lv_obj_set_style_bg_opa(orb1, 16, 0);
        lv_obj_t *orb2 = ui_rect(root, L->w * 73 / 100, L->h * 36 / 100, orb * 64 / 100, orb * 64 / 100,
                                 LV_RADIUS_CIRCLE, PAL.accent2);
        lv_obj_set_style_bg_opa(orb2, 12, 0);
        lv_obj_t *orb3 = ui_rect(root, L->w * 52 / 100, -orb * 33 / 100, orb * 58 / 100, orb * 58 / 100,
                                 LV_RADIUS_CIRCLE, PAL.accent);
        lv_obj_set_style_bg_opa(orb3, 9, 0);
    }

    if (!app->config.low_power && cover_path && *cover_path && file_exists(cover_path)) {
        char *src = ui_img_src(cover_path);
        lv_obj_t *img = lv_image_create(root);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style_all(img);
        lv_obj_set_pos(img, 0, 0);
        lv_obj_set_size(img, ui_scale_x(L->w), ui_scale_y(L->h));
        lv_image_set_src(img, src);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
        lv_obj_set_style_image_opa(img, PAL.is_light ? 40 : 90, 0);
        free(src);
        app->ui.bg_img = img;
    } else {
        app->ui.bg_img = NULL;
    }

    lv_obj_t *dim = ui_rect(root, 0, 0, L->w, L->h, 0, PAL.is_light ? 0xFFF2F4F7 : 0xFF05070C);
    lv_obj_set_style_bg_opa(dim, PAL.is_light ? 210 : 168, 0);
}

/* ── Top navigation / status pill ─────────────────────────────────────────── */

static lv_obj_t *nav_item(App *app, lv_obj_t *parent, int x, const char *icon,
                          const char *label, Screen target);

static int nav_item_width(const char *icon, const char *label)
{
    lv_font_t *font = FONT_SM;
    int need = lv_text_get_width(icon, (uint32_t)strlen(icon), font, 0) +
               lv_text_get_width(" ", 1, font, 0) +
               lv_text_get_width(label, (uint32_t)strlen(label), font, 0) +
               ui_scale_x(12) + ui_scale_x(8);
    int w = ui_unscale_x(need);
    return w < 44 ? 44 : w;
}

static int nav_add(App *app, lv_obj_t *parent, int x, const char *icon,
                   const char *label, Screen target)
{
    int i = app->ui.nav_count;
    app->ui.nav_items[i] = nav_item(app, parent, x, icon, label, target);
    app->ui.nav_screens[i] = target;
    app->ui.nav_count = i + 1;
    return nav_item_width(icon, label);
}

typedef struct {
    App *app;
    Screen screen;
} NavCtx;

static void nav_click_cb(lv_event_t *e)
{
    NavCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->app->files_pick != FILES_PICK_NONE &&
        ctx->screen != SCREEN_FILES)
        return;
    app_goto_screen(ctx->app, ctx->screen);
    audio_play(SND_NAV_STRONG);
}

static void free_ctx_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static lv_obj_t *nav_item(App *app, lv_obj_t *parent, int x, const char *icon,
                          const char *label, Screen target)
{
    UiLayout *L = &app->layout;
    bool locked = app->files_pick != FILES_PICK_NONE && target != SCREEN_FILES;
    bool selected = !locked &&
                    (target == app->screen ||
                     (target == SCREEN_HOME && (app->screen == SCREEN_GAMES ||
                                                app->screen == SCREEN_RECENT)) ||
                     (target == SCREEN_FAVORITES && app->screen == SCREEN_FAVORITES));
    int w = nav_item_width(icon, label);
    int h = L->top_h - 4;
    lv_obj_t *item = ui_rect(parent, x, 2, w, h, h / 2, selected ? PAL.accent : PAL.surface);
    lv_obj_set_style_bg_opa(item, locked ? 45 : (selected ? 48 : 120), 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_border_color(item, selected ? lv_color_hex(PAL.border_focus) : lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(item, locked ? 25 : (selected ? 150 : 90), 0);
    if (!locked) lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    if (selected) {
        lv_obj_set_style_shadow_width(item, 14, 0);
        lv_obj_set_style_shadow_color(item, lv_color_hex(PAL.accent), 0);
        lv_obj_set_style_shadow_opa(item, 40, 0);
    }
    lv_font_t *font = FONT_SM;
    char text[128];
    snprintf(text, sizeof(text), "%s %s", icon, label);
    int line_h = lv_font_get_line_height(font);
    int line_v = ui_unscale_y(line_h);
    int ty = (h - line_v) / 2;
    lv_obj_t *l = ui_label(item, text, font,
                           locked ? PAL.text3 : (selected ? PAL.text : PAL.text2),
                           2, ty, w - 4, line_v + 2);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (locked) lv_obj_set_style_text_opa(l, 120, 0);
    (void)l;
    if (target == SCREEN_APPS || target == SCREEN_STORE) {
        int dot = ui_clamp(h * 22 / 100, 6, 11);
        lv_obj_t *badge = ui_rect(item, w - dot - 3, 3, dot, dot, dot / 2, PAL.danger);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(PAL.surface), 0);
        lv_obj_set_style_border_opa(badge, 220, 0);
        lv_obj_set_style_shadow_width(badge, 6, 0);
        lv_obj_set_style_shadow_color(badge, lv_color_hex(PAL.danger), 0);
        lv_obj_set_style_shadow_opa(badge, 90, 0);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        if (target == SCREEN_APPS) app->ui.nav_app_badge = badge;
        else app->ui.nav_store_badge = badge;
    }
    lv_obj_set_user_data(item, (void *)(intptr_t)(target + 1));
    if (!locked) {
        NavCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->screen = target;
        lv_obj_add_event_cb(item, nav_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(item, free_ctx_cb, LV_EVENT_DELETE, ctx);
    }
    return item;
}

typedef struct {
    App *app;
    int kind;
} StatusClickCtx;

static void status_item_clicked(lv_event_t *e)
{
    StatusClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    ctx->app->status_on_bar = false;
    ui_update_status_focus(ctx->app);
    app_open_control_center_view(ctx->app, ctx->kind);
}

/* The individual status items are narrow. If a touch lands in the padding
 * between them, route it to the nearest item instead of swallowing the tap. */
static void status_pill_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app || app->ui.status_item_count <= 0) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int best = 0;
    int best_dist = 0x7fffffff;
    for (int i = 0; i < app->ui.status_item_count; i++) {
        lv_obj_t *item = app->ui.status_items[i];
        if (!item) continue;
        lv_area_t a;
        lv_obj_get_coords(item, &a);
        int dx = p.x - (a.x1 + a.x2) / 2;
        int dy = p.y - (a.y1 + a.y2) / 2;
        int dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    app->status_on_bar = false;
    ui_update_status_focus(app);
    app_open_control_center_view(app, app->ui.status_kinds[best]);
}

static void status_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

void ui_topbar(App *app)
{
    UiLayout *L = &app->layout;
    lv_obj_t *bar = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, ui_scale_x(L->top_x), ui_scale_y(L->top_y));
    lv_obj_set_size(bar, ui_scale_x(L->top_w), ui_scale_y(L->top_h));
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);

    lv_obj_t *nav_scroll = lv_obj_create(bar);
    lv_obj_remove_style_all(nav_scroll);
    lv_obj_set_pos(nav_scroll, 0, 0);
    lv_obj_set_size(nav_scroll, ui_scale_x(L->top_w), ui_scale_y(L->top_h));
    lv_obj_set_style_pad_all(nav_scroll, 0, 0);
    lv_obj_set_style_bg_opa(nav_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(nav_scroll, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(nav_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(nav_scroll, LV_OBJ_FLAG_SCROLLABLE);
    app->ui.nav_scroll = nav_scroll;

    int x = 0;
    app->ui.nav_count = 0;
    app->ui.nav_app_badge = NULL;
    app->ui.nav_store_badge = NULL;
    x += nav_add(app, nav_scroll, x, LV_SYMBOL_DIRECTORY, "游戏库", SCREEN_HOME) + L->nav_gap;
    x += nav_add(app, nav_scroll, x, LV_SYMBOL_IMAGE, "收藏", SCREEN_FAVORITES) + L->nav_gap;
    x += nav_add(app, nav_scroll, x, LV_SYMBOL_LIST, "应用", SCREEN_APPS) + L->nav_gap;
    x += nav_add(app, nav_scroll, x, LV_SYMBOL_DOWNLOAD, "商店", SCREEN_STORE) + L->nav_gap;
    x += nav_add(app, nav_scroll, x, LV_SYMBOL_FILE, "文件", SCREEN_FILES) + L->nav_gap;
    x += nav_add(app, nav_scroll, x, LV_SYMBOL_SETTINGS, "设置", SCREEN_SETTINGS) + L->nav_gap;
    /* 六个导航项固定存在，不再按可用性增删，避免任何索引错位。 */

    int sh = L->top_h - 4;
    int sh2 = sh;
    lv_font_t *status_font = L->h <= 360 ? FONT_XS : (L->h <= 520 ? FONT_SM : FONT_MD);
    int line_h = lv_font_get_line_height(status_font);
    int touch_ext = ui_scale_x(ui_clamp(ui_unscale_y(line_h) * 50 / 100, 6, 20));
    int pad_x = ui_clamp(line_h * 42 / 100, 5, 18);
    int gap = ui_clamp(line_h * 30 / 100, 4, 16);
    int batt_gap = ui_clamp(line_h / 4, 2, 8);
    int edge = ui_clamp(line_h * 55 / 100, 4, 20);

    time_t tnow = time(NULL);
    struct tm tmv;
    localtime_r(&tnow, &tmv);
    char clock[16];
    strftime(clock, sizeof(clock), "%H:%M", &tmv);

    /* Measure the real labels so the pill always fits its contents. */
    int sym_wifi = lv_text_get_width(LV_SYMBOL_WIFI,
                                     (uint32_t)strlen(LV_SYMBOL_WIFI), status_font, 0);
    int sym_bt = lv_text_get_width(LV_SYMBOL_BLUETOOTH,
                                   (uint32_t)strlen(LV_SYMBOL_BLUETOOTH), status_font, 0);
    int bt_gap = ui_clamp(line_h * 18 / 100, 3, 10);
    int sym_batt = lv_text_get_width(LV_SYMBOL_BATTERY_FULL,
                                     (uint32_t)strlen(LV_SYMBOL_BATTERY_FULL), status_font, 0);
    int sym_vol = lv_text_get_width(LV_SYMBOL_VOLUME_MAX,
                                    (uint32_t)strlen(LV_SYMBOL_VOLUME_MAX), status_font, 0);
    int pct_w = lv_text_get_width("100%", 4, status_font, 0);
    bool show_volume_pct = L->w >= 400;
    int padv = ui_vx(pad_x);
    int gapv = ui_vx(gap);
    int bgapv = ui_vx(batt_gap);
    int sym_wifi_v = ui_vx(sym_wifi);
    int sym_bt_v = ui_vx(sym_bt);
    int sym_batt_v = ui_vx(sym_batt);
    int sym_vol_v = ui_vx(sym_vol);
    int pct_wv = ui_vx(pct_w);
    int wifi_w = ui_vx(sym_wifi + bt_gap + sym_bt + pad_x * 2);
    int battery_w = ui_vx(pad_x + sym_batt + batt_gap + pct_w + pad_x);
    int volume_w = ui_vx(pad_x + sym_vol +
                         (show_volume_pct ? batt_gap + pct_w : 0) + pad_x);
    int clock_w = ui_vx(lv_text_get_width("00:00", 5, status_font, 0) + pad_x * 2);
    int pad_w = 0, pad_icon_w = 0, pad_icon_h = 0;
    const char *pad = backend_controller_name(&app->backend);
    bool pad_connected = pad && *pad;
    bool show_pad = L->w >= 480;
    if (show_pad) {
        int icon_px_w = ui_clamp(line_h * 16 / 10, 20, 56);
        int icon_px_h = ui_clamp(line_h, 14, 36);
        pad_icon_w = ui_vx(icon_px_w);
        pad_icon_h = ui_vx(icon_px_h);
        pad_w = ui_vx(pad_x + icon_px_w + pad_x);
    }
    int min_w = ui_clamp(sh2 / 2, 22, 44);
    if (wifi_w < min_w) wifi_w = min_w;
    if (battery_w < min_w) battery_w = min_w;
    if (volume_w < min_w) volume_w = min_w;
    if (clock_w < min_w) clock_w = min_w;
    if (pad_w && pad_w < min_w) pad_w = min_w;
    int items = 4 + (pad_w ? 1 : 0);
    int total = wifi_w + battery_w + volume_w + clock_w + pad_w + gapv * (items - 1);
    int pill_w = total + edge * 2;
    if (pill_w > L->top_w) pill_w = L->top_w;

    lv_obj_t *status = ui_rect(bar, L->top_w - pill_w, 2, pill_w, sh2, sh2 / 2, PAL.surface);
    lv_obj_set_style_bg_opa(status, 150, 0);
    lv_obj_set_style_border_width(status, 1, 0);
    lv_obj_set_style_border_color(status, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(status, 120, 0);
    lv_obj_set_style_shadow_width(status, 10, 0);
    lv_obj_set_style_shadow_color(status, lv_color_hex(PAL.accent), 0);
    lv_obj_set_style_shadow_opa(status, 28, 0);
    lv_obj_set_style_shadow_spread(status, 1, 0);
    lv_obj_add_flag(status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status, status_pill_clicked, LV_EVENT_CLICKED, app);

    int stx = ui_vx(edge);
    int text_h = ui_vy(lv_font_get_line_height(status_font));
    app->ui.status_item_count = 0;

    /* Wi-Fi */
    lv_obj_t *wifi = ui_rect(status, stx, 0, wifi_w, sh2, sh2 / 2, PAL.surface);
    lv_obj_set_style_bg_opa(wifi, 0, 0);
    lv_obj_set_style_border_width(wifi, 0, 0);
    lv_obj_add_flag(wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(wifi, touch_ext);
    g_wifi_label = ui_label(wifi, LV_SYMBOL_WIFI, status_font, PAL.text3,
                            padv, (sh2 - text_h) / 2,
                            sym_wifi_v, text_h + 2);
    g_bt_label = ui_label(wifi, LV_SYMBOL_BLUETOOTH, status_font, PAL.text3,
                          padv + sym_wifi_v + ui_vx(bt_gap),
                          (sh2 - text_h) / 2, sym_bt_v, text_h + 2);
    app->ui.status_items[app->ui.status_item_count] = wifi;
    app->ui.status_kinds[app->ui.status_item_count++] = STATUS_WIFI;
    StatusClickCtx *wctx = xmalloc(sizeof(*wctx));
    wctx->app = app; wctx->kind = STATUS_WIFI;
    lv_obj_add_event_cb(wifi, status_item_clicked, LV_EVENT_CLICKED, wctx);
    lv_obj_add_event_cb(wifi, status_ctx_free, LV_EVENT_DELETE, wctx);
    stx += wifi_w + gapv;

    /* Battery */
    lv_obj_t *battery = ui_rect(status, stx, 0, battery_w, sh2, sh2 / 2, PAL.surface);
    lv_obj_set_style_bg_opa(battery, 0, 0);
    lv_obj_set_style_border_width(battery, 0, 0);
    lv_obj_add_flag(battery, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(battery, touch_ext);
    int bi = sym_batt_v;
    int pct_x = padv + bi + bgapv;
    g_battery_label = ui_label(battery, LV_SYMBOL_BATTERY_FULL, status_font, PAL.text2,
                               padv, (sh2 - text_h) / 2, bi, text_h + 2);
    g_battery_percent = ui_label(battery, "--", status_font, PAL.text2,
                                 pct_x, (sh2 - text_h) / 2,
                                 pct_wv, text_h + 2);
    lv_obj_set_style_text_align(g_battery_percent, LV_TEXT_ALIGN_RIGHT, 0);
    app->ui.status_items[app->ui.status_item_count] = battery;
    app->ui.status_kinds[app->ui.status_item_count++] = STATUS_BATTERY;
    StatusClickCtx *bctx = xmalloc(sizeof(*bctx));
    bctx->app = app; bctx->kind = STATUS_BATTERY;
    lv_obj_add_event_cb(battery, status_item_clicked, LV_EVENT_CLICKED, bctx);
    lv_obj_add_event_cb(battery, status_ctx_free, LV_EVENT_DELETE, bctx);
    stx += battery_w + gapv;

    /* Volume */
    lv_obj_t *volume = ui_rect(status, stx, 0, volume_w, sh2, sh2 / 2, PAL.surface);
    lv_obj_set_style_bg_opa(volume, 0, 0);
    lv_obj_set_style_border_width(volume, 0, 0);
    lv_obj_add_flag(volume, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(volume, touch_ext);
    g_volume_label = ui_label(volume, LV_SYMBOL_VOLUME_MAX, status_font, PAL.text2,
                              padv, (sh2 - text_h) / 2, sym_vol_v, text_h + 2);
    if (show_volume_pct) {
        g_volume_percent = ui_label(volume, "--", status_font, PAL.text2,
                                    padv + sym_vol_v + bgapv,
                                    (sh2 - text_h) / 2,
                                    pct_wv,
                                    text_h + 2);
        lv_obj_set_style_text_align(g_volume_percent, LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        g_volume_percent = NULL;
    }
    app->ui.status_items[app->ui.status_item_count] = volume;
    app->ui.status_kinds[app->ui.status_item_count++] = STATUS_VOLUME;
    StatusClickCtx *vctx = xmalloc(sizeof(*vctx));
    vctx->app = app; vctx->kind = STATUS_VOLUME;
    lv_obj_add_event_cb(volume, status_item_clicked, LV_EVENT_CLICKED, vctx);
    lv_obj_add_event_cb(volume, status_ctx_free, LV_EVENT_DELETE, vctx);
    stx += volume_w + gapv;

    /* Clock */
    lv_obj_t *clock_box = ui_rect(status, stx, 0, clock_w, sh2, sh2 / 2, PAL.surface);
    lv_obj_set_style_bg_opa(clock_box, 0, 0);
    lv_obj_set_style_border_width(clock_box, 0, 0);
    lv_obj_add_flag(clock_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(clock_box, touch_ext);
    g_clock_label = ui_label_center(clock_box, clock, status_font,
                                    PAL.text, 0, (sh2 - text_h) / 2, clock_w, text_h + 2);
    app->ui.status_items[app->ui.status_item_count] = clock_box;
    app->ui.status_kinds[app->ui.status_item_count++] = STATUS_CLOCK;
    StatusClickCtx *cctx = xmalloc(sizeof(*cctx));
    cctx->app = app; cctx->kind = STATUS_CLOCK;
    lv_obj_add_event_cb(clock_box, status_item_clicked, LV_EVENT_CLICKED, cctx);
    lv_obj_add_event_cb(clock_box, status_ctx_free, LV_EVENT_DELETE, cctx);
    stx += clock_w + gapv;

    /* Controller / keyboard */
    if (pad_w > 0) {
        lv_obj_t *padbox = ui_rect(status, stx, 0, pad_w, sh2, sh2 / 2, PAL.surface);
        lv_obj_set_style_bg_opa(padbox, 0, 0);
        lv_obj_set_style_border_width(padbox, 0, 0);
        lv_obj_add_flag(padbox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(padbox, touch_ext);
        g_controller_icon = status_pad_icon(padbox, padv, (sh2 - pad_icon_h) / 2,
                                            pad_icon_w, pad_icon_h, pad_connected);
        app->ui.status_items[app->ui.status_item_count] = padbox;
        app->ui.status_kinds[app->ui.status_item_count++] = STATUS_PAD;
        StatusClickCtx *pctx = xmalloc(sizeof(*pctx));
        pctx->app = app; pctx->kind = STATUS_PAD;
        lv_obj_add_event_cb(padbox, status_item_clicked, LV_EVENT_CLICKED, pctx);
        lv_obj_add_event_cb(padbox, status_ctx_free, LV_EVENT_DELETE, pctx);
        stx += pad_w + gapv;
    } else {
        g_controller_icon = NULL;
    }

    int nav_area = (L->top_w - pill_w) - L->nav_gap;
    if (nav_area < 40) nav_area = 40;
    lv_obj_set_width(nav_scroll, ui_scale_x(nav_area));
    lv_obj_update_layout(nav_scroll);

    app->ui.topbar = bar;
    ui_update_status(app);
    ui_update_status_focus(app);
    ui_update_nav_focus(app);
    ui_update_app_badge(app);
    ui_update_store_badge(app);
}

/* ── Status bar controller / keyboard icon ────────────────────────────────── */

static void status_pad_icon_apply(lv_obj_t *img, bool connected)
{
    if (!img) return;
    char *path = asset_path(connected ? "images/status_gamepad.png"
                                      : "images/status_keyboard.png");
    if (!path) return;
    char *src = ui_img_src(path);
    lv_image_set_src(img, src);
    free(src);
    free(path);
    lv_obj_set_style_image_recolor(img,
        lv_color_hex(connected ? PAL.success : PAL.text2), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    g_controller_connected = connected;
}

static lv_obj_t *status_pad_icon(lv_obj_t *parent, int x, int y, int w, int h,
                                 bool connected)
{
    lv_obj_t *img = lv_image_create(parent);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style_all(img);
    lv_obj_set_pos(img, sxv(x), syv(y));
    lv_obj_set_size(img, sxv(w), syv(h));
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    status_pad_icon_apply(img, connected);
    return img;
}

/* ── Footer action pill ───────────────────────────────────────────────────── */

void ui_button_hint(App *app, char *out, size_t out_size, const char *fmt)
{
    if (!out || out_size == 0) return;
    const char *p = fmt ? fmt : "";
    size_t w = 0;
    while (*p && w + 1 < out_size) {
        const char *label = NULL;
        size_t token_len = 0;
        if (strncmp(p, "{A}", 3) == 0) {
            label = backend_button_confirm_label(&app->backend); token_len = 3;
        } else if (strncmp(p, "{B}", 3) == 0) {
            label = backend_button_cancel_label(&app->backend); token_len = 3;
        } else if (strncmp(p, "{X}", 3) == 0) {
            label = backend_button_west_label(&app->backend); token_len = 3;
        } else if (strncmp(p, "{Y}", 3) == 0) {
            label = backend_button_north_label(&app->backend); token_len = 3;
        } else if (strncmp(p, "{START}", 7) == 0) {
            label = backend_start_label(&app->backend); token_len = 7;
        } else if (strncmp(p, "{BACK}", 6) == 0) {
            label = backend_back_label(&app->backend); token_len = 6;
        } else if (strncmp(p, "{GUIDE}", 7) == 0) {
            label = backend_guide_label(&app->backend); token_len = 7;
        } else if (strncmp(p, "{L1}", 4) == 0) {
            label = backend_shoulder_left_label(&app->backend); token_len = 4;
        } else if (strncmp(p, "{R1}", 4) == 0) {
            label = backend_shoulder_right_label(&app->backend); token_len = 4;
        } else if (strncmp(p, "{L2}", 4) == 0) {
            label = backend_trigger_left_label(&app->backend); token_len = 4;
        } else if (strncmp(p, "{R2}", 4) == 0) {
            label = backend_trigger_right_label(&app->backend); token_len = 4;
        }
        if (label) {
            size_t len = strlen(label);
            if (w + len >= out_size) len = out_size - 1 - w;
            memcpy(out + w, label, len);
            w += len;
            p += token_len;
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
}

static const char *footer_button_hint(App *app, const char *src,
                                      char *buf, size_t buf_size)
{
    if (!src || !*src) return src ? src : "";
    const char *label = NULL;
    if (src[1] == ' ' || src[1] == '\0') {
        if (src[0] == 'A') label = backend_button_confirm_label(&app->backend);
        else if (src[0] == 'B') label = backend_button_cancel_label(&app->backend);
        else if (src[0] == 'X') label = backend_button_west_label(&app->backend);
        else if (src[0] == 'Y') label = backend_button_north_label(&app->backend);
    }
    if (label) {
        snprintf(buf, buf_size, "%s%s", label, src + 1);
        return buf;
    }
    if (strncmp(src, "Start", 5) == 0 && (src[5] == ' ' || src[5] == '\0')) {
        snprintf(buf, buf_size, "%s%s", backend_start_label(&app->backend), src + 5);
        return buf;
    }
    if (strncmp(src, "Back", 4) == 0 && (src[4] == ' ' || src[4] == '\0')) {
        snprintf(buf, buf_size, "%s%s", backend_back_label(&app->backend), src + 4);
        return buf;
    }
    if (strncmp(src, "Select", 6) == 0 && (src[6] == ' ' || src[6] == '\0')) {
        snprintf(buf, buf_size, "%s%s", backend_back_label(&app->backend), src + 6);
        return buf;
    }
    if (strncmp(src, "Guide", 5) == 0 && (src[5] == ' ' || src[5] == '\0')) {
        snprintf(buf, buf_size, "%s%s", backend_guide_label(&app->backend), src + 5);
        return buf;
    }
    return src;
}

void ui_footer_pill(App *app, const char *title, int count,
                    const char *a, const char *b, const char *x, const char *y)
{
    char ba[96], bb[96], bx[96], by[96];
    a = footer_button_hint(app, a, ba, sizeof(ba));
    b = footer_button_hint(app, b, bb, sizeof(bb));
    x = footer_button_hint(app, x, bx, sizeof(bx));
    y = footer_button_hint(app, y, by, sizeof(by));
    UiLayout *L = &app->layout;
    lv_obj_t *root = app->ui.root;
    int fw = L->footer_w;
    int fh = L->footer_h;
    lv_obj_t *pill = ui_rect(root, L->footer_x, L->footer_y, fw, fh, fh / 2,
                             PAL.is_light ? 0xFFFFFFFF : 0xFF05070C);
    lv_obj_set_style_bg_opa(pill, PAL.is_light ? 230 : 175, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(PAL.is_light ? PAL.border : PAL.border_focus), 0);
    lv_obj_set_style_border_opa(pill, PAL.is_light ? 180 : 55, 0);
    app->ui.footer = pill;

    lv_font_t *title_font = L->h < 520 ? FONT_XS : FONT_SM;
    int title_h = ui_text_height(title_font);
    int ty = (fh - title_h) / 2;
    if (ty < 0) ty = 0;
    int title_w = fw * 36 / 100;
    lv_obj_t *name = ui_label(pill, title ? title : "", title_font,
                              PAL.text, 10, ty, title_w, title_h);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    app->ui.footer_title_label = name;
    int chip_w = fw * 17 / 100;
    if (chip_w < 56) chip_w = 56;
    int count_h = ui_text_height(FONT_XS);
    int chip_h = ui_clamp(fh * 36 / 100, count_h + 6, 40);
    if (chip_h > fh - 4) chip_h = fh - 4;
    if (chip_h < count_h) chip_h = count_h;
    lv_obj_t *chip = ui_rect(pill, 10 + title_w, (fh - chip_h) / 2, chip_w, chip_h,
                             chip_h / 2, PAL.accent);
    lv_obj_set_style_bg_opa(chip, 35, 0);
    char cbuf[32];
    snprintf(cbuf, sizeof(cbuf), "%d %s", count,
             app->screen == SCREEN_APPS ? "个应用" :
             app->screen == SCREEN_OVERVIEW ? "个任务" :
             app->screen == SCREEN_FILES &&
             app->files_panel == FILES_PANEL_CONNECT ? "台服务器" :
             app->screen == SCREEN_FILES &&
             app->files_panel == FILES_PANEL_MANAGE ? "项共享" :
             app->screen == SCREEN_FILES ? "个文件" :
             app->screen == SCREEN_EDITOR ? "行" :
             app->screen == SCREEN_STORE ? "个应用" : "个游戏");
    lv_obj_t *cl = ui_label_center(chip, cbuf, FONT_XS,
                                   PAL.is_light ? PAL.accent : PAL.accent2,
                                   0, (chip_h - count_h) / 2, chip_w, count_h);
    app->ui.footer_count_label = cl;

    char hints[256] = "";
    const char *parts[4] = { a, b, x, y };
    for (int i = 0; i < 4; i++) {
        if (!parts[i] || !*parts[i]) continue;
        if (fw < 500 && i >= 2) continue;
        size_t cur = strlen(hints);
        snprintf(hints + cur, sizeof(hints) - cur, "%s%s",
                 cur ? "   " : "", parts[i]);
    }
    int hint_h = ui_text_height(FONT_XS);
    int hy = (fh - hint_h) / 2;
    if (hy < 0) hy = 0;
    int hx = 10 + title_w + chip_w + 8;
    lv_obj_t *h = ui_label(pill, hints, FONT_XS, PAL.text2, hx, hy,
                           fw - hx - 12, hint_h);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
}


/* ── View assembly ────────────────────────────────────────────────────────── */

static void ui_root_gesture(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app || app->modal != MODAL_NONE) return;
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t dir = indev ? lv_indev_get_gesture_dir(indev) : LV_DIR_NONE;
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
    if (indev) lv_indev_wait_release(indev);

    switch (app->screen) {
        case SCREEN_HOME:
        case SCREEN_GAMES:
        case SCREEN_FAVORITES:
        case SCREEN_RECENT:
        case SCREEN_APPS:
            /* Swipe left = next page/category, swipe right = previous.
             * Do not rebuild the screen from inside the gesture callback:
             * the root object is deleted by app_rebuild(). */
            app->pending_input_action = dir == LV_DIR_LEFT ? IN_RT : IN_LT;
            break;
        default:
            break;
    }
}

void ui_build(App *app)
{
    /* 记住时区列表的滚动位置：整页重建后仍从原位置继续，
     * 只有焦点真正到达可视区边缘时才滚动。 */
    if (app->oobe && app->ui.oobe_tz_list)
        app->oobe_tz_scroll = lv_obj_get_scroll_y(app->ui.oobe_tz_list);
    if (app->oobe && app->ui.oobe_content)
        app->oobe_content_scroll = lv_obj_get_scroll_y(app->ui.oobe_content);
    g_ui_building = true;
    if (app->ui.modal) {
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    g_sx = 1.0f;
    g_sy = 1.0f;
    app->ui = (Ui){0};
    g_clock_label = NULL;
    g_controller_icon = NULL;
    g_controller_connected = false;
    g_wifi_label = NULL;
    g_bt_label = NULL;
    g_battery_label = NULL;
    g_battery_percent = NULL;
    g_volume_label = NULL;
    g_volume_percent = NULL;
    g_toast_obj = NULL;
    ui_layout_update(app);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, ui_scale_x(app->layout.w), ui_scale_y(app->layout.h));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* Let horizontal swipes reach the root on non-scrollable pages. */
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, ui_root_gesture, LV_EVENT_GESTURE, app);
    app->ui.root = root;

    /* A full rebuild recreates every widget: drop cached OOBE clock labels so
     * the periodic tick never touches freed objects. */
    app->ui.oobe_big_clock = NULL;
    app->ui.oobe_big_date = NULL;
    app->ui.oobe_big_zone[0] = '\0';
    app->ui.oobe_date_label = NULL;
    app->ui.oobe_clock_count = 0;
    app->ui.oobe_content = NULL;
    app->ui.oobe_focus_obj = NULL;
    app->ui.oobe_tz_list = NULL;

    if (app->oobe && app->files_pick == FILES_PICK_NONE) {
        ui_build_oobe(app);
        if (app->modal != MODAL_NONE) ui_build_modal(app);
        if (app->toast_text[0] && app->toast_until > lv_tick_get())
            ui_toast(app, app->toast_text);
        g_ui_building = false;
        return;
    }

    switch (app->screen) {
        case SCREEN_GAMES:
            ui_build_games(app, app->collection_id);
            break;
        case SCREEN_FAVORITES:
            ui_build_games(app, "favorites");
            break;
        case SCREEN_RECENT:
            ui_build_games(app, "recent");
            break;
        case SCREEN_APPS:
            ui_build_apps(app);
            break;
        case SCREEN_SETTINGS:
            ui_build_settings(app);
            break;
        case SCREEN_STORE:
            ui_build_store(app);
            break;
        case SCREEN_FILES:
            ui_build_files(app);
            break;
        case SCREEN_EDITOR:
            ui_build_editor(app);
            break;
        case SCREEN_OVERVIEW:
            ui_build_overview(app);
            break;
        case SCREEN_HOME:
        default:
            ui_build_home(app);
            break;
    }

    ui_topbar(app);
    if (app->modal != MODAL_NONE) ui_build_modal(app);
    if (app->toast_text[0] && app->toast_until > lv_tick_get()) {
        ui_toast(app, app->toast_text);
    }
    g_ui_building = false;
}

void ui_update(App *app)
{
    switch (app->screen) {
        case SCREEN_GAMES:
        case SCREEN_FAVORITES:
        case SCREEN_RECENT:
            ui_update_games(app);
            break;
        case SCREEN_APPS:
            ui_update_apps(app);
            break;
        case SCREEN_SETTINGS:
            ui_update_settings(app);
            break;
        case SCREEN_FILES:
            ui_update_files(app);
            break;
        case SCREEN_OVERVIEW:
            break;
        default:
            ui_update_home(app);
            break;
    }
}

void ui_apply_theme_to_screen(App *app)
{
    app_rebuild(app);
}

/* ── Toast ────────────────────────────────────────────────────────────────── */

void ui_toast(App *app, const char *text)
{
    if (!text || !*text) return;
    text = tr(text);
    snprintf(app->toast_text, sizeof(app->toast_text), "%s", text);
    app->toast_until = lv_tick_get() + 3200;
    if (g_toast_obj) {
        lv_obj_del(g_toast_obj);
        g_toast_obj = NULL;
    }
    UiLayout *L = &app->layout;
    int tw = ui_min(L->w - L->pad * 2, 500);
    int th = ui_clamp(L->h * 6 / 100, 34, 44);
    lv_obj_t *t = ui_rect(app->ui.root, (L->w - tw) / 2, L->footer_y - th - 14,
                          tw, th, th / 2, PAL.surface);
    lv_obj_set_style_bg_opa(t, 235, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(t, 100, 0);
    lv_obj_set_style_shadow_width(t, 20, 0);
    lv_obj_set_style_shadow_color(t, lv_color_hex(0xFF000000), 0);
    lv_obj_set_style_shadow_opa(t, 90, 0);
    ui_label_center(t, text, FONT_SM, PAL.text, 14, (th - 18) / 2, tw - 28, th - 8);
    g_toast_obj = t;
    lv_obj_move_foreground(t);
}

static void fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

typedef struct {
    App *app;
    lv_obj_t *overlay;
    lv_obj_t *panel;
    int modal;
} FadeCtx;

static void fade_out_done(lv_anim_t *a)
{
    FadeCtx *ctx = lv_anim_get_user_data(a);
    if (!ctx) return;
    App *app = ctx->app;
    if (app->ui.modal == ctx->overlay) {
        lv_obj_del(ctx->overlay);
        app->ui.modal = NULL;
    }
    if (app->ui.modal_anim_target == ctx->panel)
        app->ui.modal_anim_target = NULL;
    app->ui.modal_panel = NULL;
    app->ui.modal_input = NULL;
    app->ui.modal_list_count = 0;
    if (app->ui.control_volume_value) {
        app->ui.control_volume_value = NULL;
        app->ui.control_volume_icon = NULL;
        app->ui.control_volume_bar = NULL;
        app->ui.control_volume_slider = NULL;
    }
    app->ui.control_brightness_value = NULL;
    app->ui.control_brightness_icon = NULL;
    app->ui.control_brightness_bar = NULL;
    app->ui.control_brightness_slider = NULL;
    for (int i = 0; i < 5; i++) {
        app->ui.oobe_date_boxes[i] = NULL;
        app->ui.oobe_date_values[i] = NULL;
    }
    app->control_closing = false;
    app->modal_closing = false;
    if (app->modal == ctx->modal) app->modal = MODAL_NONE;
    free(ctx);
}

void ui_fade_in_modal(App *app)
{
    if (!app->ui.modal) return;
    if (app->config.reduce_motion) {
        lv_obj_t *target = app->ui.modal_panel ? app->ui.modal_panel : app->ui.modal;
        lv_obj_set_style_opa(target, 255, 0);
        return;
    }
    /* Only the card fades.  The full-screen dim layer appears immediately so
     * the whole desktop does not blink on every status-bar click. */
    lv_obj_t *target = app->ui.modal_panel ? app->ui.modal_panel : app->ui.modal;
    app->ui.modal_anim_target = target;
    lv_obj_set_style_opa(target, 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, fade_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void ui_fade_out_modal(App *app)
{
    if (!app->ui.modal) {
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        app->control_closing = false;
        return;
    }
    if (app->config.reduce_motion) {
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        app->control_closing = false;
        ui_close_modal_visual(app);
        return;
    }
    FadeCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->overlay = app->ui.modal;
    ctx->panel = app->ui.modal_panel ? app->ui.modal_panel : app->ui.modal;
    ctx->modal = app->modal;
    app->ui.modal_anim_target = ctx->panel;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx->panel);
    lv_anim_set_exec_cb(&a, fade_exec);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_completed_cb(&a, fade_out_done);
    lv_anim_start(&a);
}

static const char *battery_symbol(int pct)
{
    if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 70) return LV_SYMBOL_BATTERY_3;
    if (pct >= 45) return LV_SYMBOL_BATTERY_2;
    if (pct >= 20) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

void ui_update_status_focus(App *app)
{
    int n = app->ui.status_item_count;
    for (int i = 0; i < n; i++) {
        lv_obj_t *item = app->ui.status_items[i];
        if (!item) continue;
        bool focused = app->status_on_bar && i == app->status_focus;
        /* Use the same focus ring as the rest of the UI.  The ring is drawn
         * with an outline instead of a border: a border is part of the
         * object's content box, so the 4px stroke pushed the icon and the
         * percentage 4px right/down and made the ring look off-centre.
         * outline_pad = -4 keeps the stroke exactly where the old border was,
         * inside the item bounds, without moving the children.  Geometry is
         * constant and only opacity animates, so moving between items does
         * not trigger a repaint flash. */
        lv_obj_set_style_bg_opa(item, 0, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_outline_width(item, 4, 0);
        lv_obj_set_style_outline_pad(item, -4, 0);
        lv_obj_set_style_outline_color(item, lv_color_hex(PAL.border_focus), 0);
        lv_obj_set_style_outline_opa(item, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(item, g_low_power ? 0 : 24, 0);
        lv_obj_set_style_shadow_color(item, lv_color_hex(PAL.border_focus), 0);
        lv_obj_set_style_shadow_opa(item, (focused && !g_low_power) ? 90 : 0, 0);
        lv_obj_set_style_shadow_spread(item, (focused && !g_low_power) ? 2 : 0, 0);
    }
}

static bool nav_is_selected(App *app, int index)
{
    if (index < 0 || index >= app->ui.nav_count) return false;
    lv_obj_t *item = app->ui.nav_items[index];
    if (!item) return false;
    intptr_t v = (intptr_t)lv_obj_get_user_data(item);
    if (v <= 0) return false;
    Screen target = (Screen)(v - 1);
    if (target == SCREEN_HOME)
        return app->screen == SCREEN_HOME || app->screen == SCREEN_GAMES ||
               app->screen == SCREEN_RECENT;
    if (target == SCREEN_FILES && app->screen == SCREEN_EDITOR) return true;
    return app->screen == target;
}

void ui_update_nav_focus(App *app)
{
    for (int i = 0; i < app->ui.nav_count; i++) {
        lv_obj_t *item = app->ui.nav_items[i];
        if (!item) continue;
        bool locked = app->files_pick != FILES_PICK_NONE &&
                      app->ui.nav_screens[i] != SCREEN_FILES;
        if (locked) {
            lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(item, 45, 0);
            lv_obj_set_style_border_width(item, 1, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(PAL.border), 0);
            lv_obj_set_style_border_opa(item, 25, 0);
            lv_obj_set_style_shadow_width(item, 0, 0);
            lv_obj_t *l = lv_obj_get_child(item, 0);
            if (l) {
                lv_obj_set_style_text_color(l, lv_color_hex(PAL.text3), 0);
                lv_obj_set_style_text_opa(l, 120, 0);
            }
            continue;
        }
        bool selected = nav_is_selected(app, i);
        bool focused = app->nav_on_bar && i == app->nav_focus;
        lv_obj_set_style_bg_opa(item, focused ? 80 : (selected ? 48 : 120), 0);
        if (focused) {
            uint32_t ring = PAL.border_focus;
            lv_obj_set_style_border_width(item, 1, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(ring), 0);
            lv_obj_set_style_border_opa(item, 255, 0);
            lv_obj_set_style_outline_width(item, 4, 0);
            lv_obj_set_style_outline_pad(item, -4, 0);
            lv_obj_set_style_outline_color(item, lv_color_hex(ring), 0);
            lv_obj_set_style_outline_opa(item, 255, 0);
            lv_obj_set_style_shadow_width(item, g_low_power ? 0 : 20, 0);
            lv_obj_set_style_shadow_color(item, lv_color_hex(ring), 0);
            lv_obj_set_style_shadow_opa(item, g_low_power ? 0 : 80, 0);
            lv_obj_set_style_shadow_spread(item, g_low_power ? 0 : 2, 0);
        } else {
            lv_obj_set_style_border_width(item, 1, 0);
            lv_obj_set_style_border_color(item,
                lv_color_hex(selected ? PAL.border_focus : PAL.border), 0);
            lv_obj_set_style_border_opa(item, selected ? 150 : 90, 0);
            lv_obj_set_style_outline_width(item, 0, 0);
            lv_obj_set_style_outline_opa(item, LV_OPA_TRANSP, 0);
            lv_obj_set_style_shadow_width(item, 0, 0);
        }
    }
    int target = -1;
    if (app->nav_on_bar) target = app->nav_focus;
    if (target < 0) {
        for (int i = 0; i < app->ui.nav_count; i++) {
            if (nav_is_selected(app, i)) { target = i; break; }
        }
    }
    if (target >= 0 && target < app->ui.nav_count && app->ui.nav_items[target])
        lv_obj_scroll_to_view(app->ui.nav_items[target], LV_ANIM_OFF);
}

void ui_update_app_badge(App *app)
{
    if (!app || !app->ui.nav_app_badge) return;
    if (app->app_badge) {
        lv_obj_clear_flag(app->ui.nav_app_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(app->ui.nav_app_badge);
    } else {
        lv_obj_add_flag(app->ui.nav_app_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_store_badge(App *app)
{
    if (!app || !app->ui.nav_store_badge) return;
    if (app->store_update_count > 0) {
        lv_obj_clear_flag(app->ui.nav_store_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(app->ui.nav_store_badge);
    } else {
        lv_obj_add_flag(app->ui.nav_store_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_status(App *app)
{
    if (!app) return;
    if (g_wifi_label) {
        uint32_t c = app->sys.wifi == 1 ? PAL.success :
                     (app->wifi.enabled ? PAL.text2 : PAL.text3);
        lv_obj_set_style_text_color(g_wifi_label, lv_color_hex(c), 0);
        lv_obj_set_style_text_opa(g_wifi_label, app->sys.wifi < 0 ? 140 : 255, 0);
    }
    if (g_bt_label) {
        bool bt_connected = false;
        for (int i = 0; i < app->bt.count; i++) {
            if (app->bt.items[i].connected) { bt_connected = true; break; }
        }
        uint32_t c = bt_connected ? PAL.success :
                     (app->bt.enabled ? PAL.text2 : PAL.text3);
        lv_obj_set_style_text_color(g_bt_label, lv_color_hex(c), 0);
        lv_obj_set_style_text_opa(g_bt_label, app->bt.available ? 255 : 120, 0);
    }
    if (g_volume_label) {
        int vol = app->sysvol.percent;
        bool muted = !app->sysvol.available || app->sysvol.muted || vol <= 0;
        const char *sym = muted ? LV_SYMBOL_MUTE :
                          (vol < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
        uint32_t c = muted ? PAL.text3 : PAL.text2;
        lv_label_set_text(g_volume_label, sym);
        lv_obj_set_style_text_color(g_volume_label, lv_color_hex(c), 0);
        if (g_volume_percent) {
            char buf[16];
            if (!app->sysvol.available) snprintf(buf, sizeof(buf), "--");
            else if (app->layout.w < 400) snprintf(buf, sizeof(buf), "%d", vol);
            else snprintf(buf, sizeof(buf), "%d%%", vol);
            lv_label_set_text(g_volume_percent, buf);
            lv_obj_set_style_text_color(g_volume_percent, lv_color_hex(c), 0);
        }
    }
    if (g_battery_label && g_battery_percent) {
        if (app->sys.battery < 0) {
            lv_label_set_text(g_battery_label, LV_SYMBOL_POWER);
            lv_obj_set_style_text_color(g_battery_label, lv_color_hex(PAL.text3), 0);
            lv_label_set_text(g_battery_percent, "AC");
            lv_obj_set_style_text_color(g_battery_percent, lv_color_hex(PAL.text3), 0);
        } else {
            uint32_t c = app->sys.battery <= 15 ? PAL.danger :
                         (app->sys.charging ? PAL.success : PAL.text2);
            lv_label_set_text(g_battery_label, battery_symbol(app->sys.battery));
            lv_obj_set_style_text_color(g_battery_label, lv_color_hex(c), 0);
            char buf[16];
            if (app->layout.w < 400) snprintf(buf, sizeof(buf), "%d", app->sys.battery);
            else snprintf(buf, sizeof(buf), "%d%%", app->sys.battery);
            lv_label_set_text(g_battery_percent, buf);
            lv_obj_set_style_text_color(g_battery_percent, lv_color_hex(c), 0);
        }
    }
}

void ui_tick(App *app)
{
    if (app->pending_input_action) {
        int action = app->pending_input_action;
        app->pending_input_action = 0;
        app_input(app, (InputAction)action, NULL);
    }
    if (app->pending_overview_focus >= 0) {
        int focus = app->pending_overview_focus;
        app->pending_overview_focus = -1;
        if (app->screen == SCREEN_OVERVIEW) {
            app->overview_focus = focus;
            app_rebuild(app);
        }
    }
    if (app->pending_overview_activate >= 0) {
        int index = app->pending_overview_activate;
        app->pending_overview_activate = -1;
        if (app->screen == SCREEN_OVERVIEW)
            app_activate_task(app, index);
    }
    if (app->pending_overview_close) {
        app->pending_overview_close = false;
        if (app->screen == SCREEN_OVERVIEW)
            app_close_overview(app);
    }
    if (app->overview_close_pending >= 0) {
        int idx = app->overview_close_pending;
        app->overview_close_pending = -1;
        if (app->screen == SCREEN_OVERVIEW) app_close_task(app, idx);
    }
    uint32_t now = lv_tick_get();
    if (g_clock_label) {
        if (now - g_last_clock > 15000) {
            g_last_clock = now;
            time_t t = time(NULL);
            struct tm tmv;
            localtime_r(&t, &tmv);
            char clock[16];
            strftime(clock, sizeof(clock), "%H:%M", &tmv);
            lv_label_set_text(g_clock_label, clock);
        }
    }
    if (g_controller_icon) {
        const char *pad = backend_controller_name(&app->backend);
        bool connected = pad && *pad;
        if (connected != g_controller_connected)
            status_pad_icon_apply(g_controller_icon, connected);
    }
    if (g_toast_obj && app->toast_until && now > app->toast_until) {
        lv_obj_del(g_toast_obj);
        g_toast_obj = NULL;
        app->toast_text[0] = '\0';
        app->toast_until = 0;
    }
}

void ui_set_input_active(bool active)
{
    /* 所有文本输入统一由 armdeck-osk 负责。装了外置键盘时不再启用
     * SDL/Wayland 的系统输入法，避免同一串字符被输入两次；没有外置
     * 键盘的开发环境仍允许实体键盘输入。 */
    if (external_osk_available()) {
        backend_text_input(false);
        return;
    }
    backend_text_input(active);
}

/* ── Modal shell ──────────────────────────────────────────────────────────── */

static void modal_panel_stop(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void modal_overlay_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    switch (app->modal) {
        case MODAL_TEXT:
        case MODAL_SEARCH:
        case MODAL_SCAN:
        case MODAL_MAPPING:
        case MODAL_CONTROL:
        case MODAL_OOBE_DATE:
            return;
        default:
            break;
    }
    audio_play(SND_BACK);
    app_close_modal(app);
}

void ui_close_modal_visual(App *app)
{
    if (app->ui.modal_anim_target) {
        lv_anim_del(app->ui.modal_anim_target, NULL);
        app->ui.modal_anim_target = NULL;
    }
    app->modal_closing = false;
    app->control_closing = false;
    if (app->ui.modal) {
        lv_obj_del(app->ui.modal);
        app->ui.modal = NULL;
    }
    app->ui.modal_panel = NULL;
    app->ui.modal_scroll = NULL;
    app->ui.modal_list_count = 0;
    app->ui.modal_input = NULL;
    app->ui.control_wifi_value = NULL;
    app->ui.control_battery_value = NULL;
    app->ui.control_volume_value = NULL;
    app->ui.control_volume_icon = NULL;
    app->ui.control_volume_bar = NULL;
    app->ui.control_volume_slider = NULL;
    app->ui.control_volume_bar_full = 0;
    app->ui.control_volume_track_x = 0;
    app->ui.control_volume_track_w = 0;
    app->ui.control_volume_knob_size = 0;
    app->ui.control_brightness_value = NULL;
    app->ui.control_brightness_icon = NULL;
    app->ui.control_brightness_bar = NULL;
    app->ui.control_brightness_slider = NULL;
    app->ui.control_brightness_bar_full = 0;
    app->ui.control_brightness_track_x = 0;
    app->ui.control_brightness_track_w = 0;
    app->ui.control_brightness_knob_size = 0;
}

static void modal_fonts_begin(float scale);
static void modal_fonts_end(void);

static void ui_build_modal_body(App *app)
{
    switch (app->modal) {
        case MODAL_SEARCH: ui_build_modal_search(app); break;
        case MODAL_DETAIL: ui_build_modal_detail(app); break;
        case MODAL_TEXT: ui_build_modal_text(app); break;
        case MODAL_CONFIRM: ui_build_modal_confirm(app); break;
        case MODAL_CHOICE: ui_build_modal_choice(app); break;
        case MODAL_MENU: ui_build_modal_menu(app); break;
        case MODAL_ROOTS: ui_build_modal_roots(app); break;
        case MODAL_MAPPING: ui_build_modal_mapping(app); break;
        case MODAL_HELP: ui_build_modal_help(app); break;
        case MODAL_EDITOR_HELP: ui_build_modal_editor_help(app); break;
        case MODAL_SCAN: ui_build_modal_scan(app); break;
        case MODAL_APP: ui_build_modal_app(app); break;
        case MODAL_CONTROL: ui_build_modal_control(app); break;
        case MODAL_POWER: ui_build_modal_power(app); break;
        case MODAL_GAMEPAD: ui_build_modal_gamepad(app); break;
        case MODAL_SMB_SHARE: ui_build_modal_smb_share(app); break;
        case MODAL_OOBE_DATE: ui_build_modal_oobe_date(app); break;
        default: break;
    }
}

void ui_build_modal(App *app)
{
    ui_close_modal_visual(app);
    UiLayout *L = &app->layout;
    g_sx = 1.0f;
    g_sy = 1.0f;
    lv_obj_t *overlay = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, ui_scale_x(L->w), ui_scale_y(L->h));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, PAL.is_light ? 110 : 170, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, modal_overlay_clicked, LV_EVENT_CLICKED, app);
    app->ui.modal = overlay;
    app->ui.modal_scroll = NULL;
    lv_obj_move_foreground(overlay);

    ui_build_modal_body(app);
    g_sx = 1.0f;
    g_sy = 1.0f;
    modal_fonts_end();
    g_sx = 1.0f;
    g_sy = 1.0f;
    if (app->modal == MODAL_TEXT || app->modal == MODAL_SEARCH)
        ui_ime_show(app);
    if (app->modal != MODAL_CONTROL) ui_fade_in_modal(app);
}

void ui_refresh_modal(App *app)
{
    if (!app->ui.modal || !app->ui.modal_panel) {
        ui_build_modal(app);
        return;
    }
    if (app->modal == MODAL_CONTROL) {
        ui_refresh_control_modal(app);
        return;
    }
    if (app->ui.modal_anim_target) {
        lv_anim_del(app->ui.modal_anim_target, NULL);
        app->ui.modal_anim_target = NULL;
    }
    lv_obj_t *old = app->ui.modal_panel;
    app->ui.modal_panel = NULL;
    app->ui.modal_scroll = NULL;
    lv_obj_del(old);
    g_sx = 1.0f;
    g_sy = 1.0f;
    ui_build_modal_body(app);
    g_sx = 1.0f;
    g_sy = 1.0f;
    modal_fonts_end();
}

/* Shared helpers for modal builders. */
static lv_font_t *saved_fonts[7];
static bool fonts_saved = false;

static void modal_fonts_begin(float scale)
{
    if (fonts_saved) return;
    lv_font_t *cur[7] = {FONT_XS, FONT_SM, FONT_MD, FONT_LG, FONT_XL, FONT_XXL, FONT_HERO};
    for (int i = 0; i < 7; i++) saved_fonts[i] = cur[i];
    fonts_saved = true;
    if (scale < 0.98f) {
        if (scale < 0.48f) {
            /* 320x240-class screens: positions shrink faster than readable
             * fonts, so keep every modal label on the smallest face. */
            FONT_SM = cur[0];
            FONT_MD = cur[0];
            FONT_LG = cur[0];
            FONT_XL = cur[0];
            FONT_XXL = cur[1];
            FONT_HERO = cur[2];
        } else if (scale < 0.62f) {
            /* Two steps down for 480x320-class screens. */
            FONT_SM = cur[0];
            FONT_MD = cur[0];
            FONT_LG = cur[1];
            FONT_XL = cur[2];
            FONT_XXL = cur[3];
            FONT_HERO = cur[4];
        } else {
            FONT_SM = cur[0];
            FONT_MD = cur[1];
            FONT_LG = cur[2];
            FONT_XL = cur[3];
            FONT_XXL = cur[4];
            FONT_HERO = cur[5];
        }
    }
}

static void modal_fonts_end(void)
{
    if (!fonts_saved) return;
    FONT_XS = saved_fonts[0];
    FONT_SM = saved_fonts[1];
    FONT_MD = saved_fonts[2];
    FONT_LG = saved_fonts[3];
    FONT_XL = saved_fonts[4];
    FONT_XXL = saved_fonts[5];
    FONT_HERO = saved_fonts[6];
    fonts_saved = false;
}

lv_obj_t *ui_modal_panel(struct App *app, int w, int h)
{
    UiLayout *L = &app->layout;
    float sw = (float)(L->w - L->pad * 2) / (float)w;
    float sh = (float)(L->h - L->pad * 2) / (float)h;
    float scale = sw < sh ? sw : sh;
    if (scale > 1.9f) scale = 1.9f;
    if (scale < 0.25f) scale = 0.25f;
    int aw = (int)((float)w * scale);
    int ah = (int)((float)h * scale);
    g_sx = 1.0f;
    g_sy = 1.0f;
    lv_obj_t *panel = ui_rect(app->ui.modal, (L->w - aw) / 2, (L->h - ah) / 2,
                              aw, ah, 24, PAL.surface);
    g_sx = scale;
    g_sy = scale;
    modal_fonts_begin(scale);
    lv_obj_set_style_bg_opa(panel, 245, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(panel, 100, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, modal_panel_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_shadow_width(panel, 40, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(panel, 150, 0);
    app->ui.modal_panel = panel;
    return panel;
}

void ui_draw_spinner_overlay(struct App *app, const char *title, const char *detail)
{
    if (app->ui.modal) return;
    lv_obj_t *overlay = lv_obj_create(app->ui.root);
    lv_obj_remove_style_all(overlay);
    UiLayout *L = &app->layout;
    lv_obj_set_size(overlay, ui_scale_x(L->w), ui_scale_y(L->h));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, 160, 0);
    app->ui.modal = overlay;
    int pw = ui_min(L->w - L->pad * 2, 400);
    int ph = ui_min(L->h - L->pad * 2, 160);
    lv_obj_t *panel = ui_rect(overlay, (L->w - pw) / 2, (L->h - ph) / 2, pw, ph, 24, PAL.surface);
    ui_label_center(panel, title, FONT_LG, PAL.text, 20, ph / 4, pw - 40, 30);
    ui_label_center(panel, detail, FONT_SM, PAL.text2, 20, ph / 2 + 10, pw - 40, 24);
    lv_refr_now(NULL);
}

void ui_footer_update(App *app, const char *title, int count)
{
    if (app->ui.footer_title_label) {
        lv_label_set_text(app->ui.footer_title_label, title ? title : "");
    }
    if (app->ui.footer_count_label) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d %s", count,
                 app->screen == SCREEN_APPS ? "个应用" : "个游戏");
        lv_label_set_text(app->ui.footer_count_label, buf);
    }
}

void ui_splash(App *app, const char *message)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_layout_update(app);
    UiLayout *L = &app->layout;
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, ui_scale_x(L->w), ui_scale_y(L->h));
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFF05070C), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0xFF101625), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    char *img_path = asset_path("images/splash.jpg");
    if (!app->config.low_power && L->w <= 1600 && file_exists(img_path)) {
        char *src = ui_img_src(img_path);
        lv_obj_t *img = lv_image_create(root);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style_all(img);
        lv_obj_set_size(img, ui_scale_x(L->w), ui_scale_y(L->h));
        lv_image_set_src(img, src);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
        lv_obj_set_style_image_opa(img, 90, 0);
        free(src);
        lv_obj_t *dim = ui_rect(root, 0, 0, L->w, L->h, 0, 0x05070C);
        lv_obj_set_style_bg_opa(dim, 175, 0);
    }
    free(img_path);
    int cy = L->h * 33 / 100;
    ui_label_center(root, "太极", FONT_HERO, 0xFFFFFFFF, 0, cy, L->w, 80);
    ui_label_center(root, "TAIJI  /  NOVARETRO DESKTOP", FONT_SM, 0x99FFFFFF, 0, cy + 82, L->w, 24);
    ui_label_center(root, message ? message : "正在准备游戏库...", FONT_MD, PAL.accent2, 0, cy + 150, L->w, 28);
    lv_obj_t *spinner = lv_spinner_create(root);
    int spinner_s = ui_scale_x(34);
    lv_obj_set_size(spinner, spinner_s, spinner_s);
    lv_obj_set_pos(spinner, ui_scale_x(L->w / 2 - 17), ui_scale_y(cy + 200));
    lv_obj_set_style_arc_color(spinner, lv_color_hex(PAL.accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
    app->ui.root = root;
    lv_refr_now(NULL);
    app->backend.needs_present = true;
    backend_present(&app->backend);
}
