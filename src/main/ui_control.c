// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "i18n.h"
#include "theme.h"
#include "system_brightness.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *battery_health_text(const char *raw);

static int control_text_height(const char *text, lv_font_t *font, int width)
{
    if (!text) text = "";
    lv_point_t size = {0};
    int max_w = ui_scale_x(width > 0 ? width : 1);
    lv_text_get_size(&size, tr(text), font, 0, 0, max_w, LV_TEXT_FLAG_NONE);
    int h = ui_unscale_y(size.y) + 2;
    int line = ui_text_height(font);
    return h < line ? line : h;
}

/* 计算非网络卡片需要的内容高度；文字换行越多，卡片就越高。 */
static int control_required_content_h(App *app, int cw, int base_content_h)
{
    if (app->control_view == STATUS_WIFI) return base_content_h;
    bool tiny = base_content_h < 210;
    int header_h = tiny ? 26 : ui_clamp(base_content_h * 9 / 100, 32, 48);
    int card_y = header_h + 6;
    int base_card_h = base_content_h - card_y - 10;
    if (base_card_h < 40) base_card_h = 40;
    int need = base_card_h;

    if (app->control_view == STATUS_BATTERY) {
        int pct = app->sys.battery;
        bool has_cpu = app->perf.available;
        bool has_gpu = app->gpu.available;
        int modules = (has_cpu ? 1 : 0) + (has_gpu ? 1 : 0);
        bool compact;
        bool very_compact;
        if (modules >= 2) {
            very_compact = base_card_h < 260;
            compact = base_card_h < 380;
        } else {
            very_compact = base_card_h < 190;
            compact = base_card_h < 240;
        }
        int module_h = very_compact ? 30 : (compact ? 34 : 54);
        int module_gap = very_compact ? 2 : 4;
        int footer_h = (modules > 0 && base_card_h >= 270) ? 40 : 0;
        int bottom_pad = footer_h > 0 ? 0 : (very_compact ? 4 : 6);
        int modules_total = modules > 0 ?
            modules * module_h + (modules - 1) * module_gap + footer_h + 4 + bottom_pad : 0;

        char state[128];
        if (pct < 0) snprintf(state, sizeof(state), "没有检测到电池");
        else snprintf(state, sizeof(state), "%s  /  %s",
                      app->sys.charging ? "充电中" : "使用电池",
                      pct <= 15 ? "电量偏低" : "电量正常");
        int health = app->sys.battery_health;
        char hbuf[96];
        if (health >= 0) {
            const char *raw = battery_health_text(app->sys.battery_health_label);
            if (raw && *raw)
                snprintf(hbuf, sizeof(hbuf), "电池健康度  %d%%  /  %s", health, raw);
            else
                snprintf(hbuf, sizeof(hbuf), "电池健康度  %d%%", health);
        } else {
            snprintf(hbuf, sizeof(hbuf), "电池健康度  未知");
        }
        int text_w = cw - 24;
        if (text_w < 40) text_w = 40;
        lv_font_t *big_font = very_compact ? FONT_LG : (compact ? FONT_XL : FONT_XXL);
        lv_font_t *state_font = (compact || very_compact) ? FONT_XS : FONT_SM;
        int icon_size = very_compact ? 0 : (compact ? 30 : ui_clamp(base_card_h * 18 / 100, 36, 64));
        int h_icon = very_compact ? 0 : (compact ? 30 : icon_size + 6);
        int h_big = ui_text_height(big_font);
        int h_state = control_text_height(state, state_font, text_w);
        int h_health = control_text_height(hbuf, FONT_XS, text_w);
        int gaps = (h_icon > 0 ? 1 : 0) + 2;
        int bright_h = very_compact ? 32 : (compact ? 36 : 44);
        /* Include the header/card gutters so the panel grows to the real
         * content height first.  Scrolling is only needed when the screen
         * cannot provide this height. */
        need = h_icon + h_big + h_state + h_health + gaps + 6 +
               modules_total + bright_h + 2 + card_y + 10;
    } else if (app->control_view == STATUS_VOLUME) {
        bool compact = base_card_h < 180;
        lv_font_t *big = compact ? FONT_XL : FONT_XXL;
        int h_big = ui_text_height(big);
        const char *hint = !app->sysvol.available ? "系统音量不可用（需要 PipeWire / wpctl）" :
                           "拖动滑块 / 左右键调节";
        int hint_h = control_text_height(hint, FONT_XS, cw - 8);
        int value_h = (compact ? 0 : 4) + h_big + 2 + h_big + 4;
        need = value_h + 8 + 24 + 8 + hint_h + 8;
    } else if (app->control_view == STATUS_CLOCK) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char datebuf[64];
        strftime(datebuf, sizeof(datebuf), "%Y-%m-%d  %A", &tmv);
        need = ui_text_height(FONT_HERO) + 4 +
               control_text_height(datebuf, FONT_MD, cw - 16) + 4 +
               ui_text_height(FONT_XS) + 12;
    } else if (app->control_view == STATUS_PAD) {
        const char *name = backend_controller_name(&app->backend);
        int batt = backend_controller_battery(&app->backend);
        lv_font_t *name_font = base_card_h >= 150 ? FONT_LG : FONT_MD;
        lv_font_t *line_font = base_card_h >= 150 ? FONT_SM : FONT_XS;
        const char *disp_name = name && *name ? name : "未连接手柄";
        char line[160];
        if (batt >= 0) snprintf(line, sizeof(line), "已连接  /  手柄电量 %d%%", batt);
        else if (name && *name) snprintf(line, sizeof(line), "已连接");
        else snprintf(line, sizeof(line), "使用键盘操作");
        char labels[128];
        snprintf(labels, sizeof(labels), "%s / %s    %s / %s",
                 backend_shoulder_left_label(&app->backend),
                 backend_shoulder_right_label(&app->backend),
                 backend_trigger_left_label(&app->backend),
                 backend_trigger_right_label(&app->backend));
        int text_w = cw - 16;
        char pad_hint[64];
        ui_button_hint(app, pad_hint, sizeof(pad_hint), "{A} 确认  {B} 返回");
        need = control_text_height(disp_name, name_font, text_w) + 4 +
               control_text_height(line, line_font, text_w) + 4 +
               control_text_height(labels, FONT_XS, text_w) + 4 +
               control_text_height(pad_hint, FONT_XS, text_w) + 16;
    }
    if (need < base_card_h) need = base_card_h;
    return card_y + need + 10;
}

typedef struct {
    App *app;
    int index;
} WifiClickCtx;

static void control_card_glow(App *app, lv_obj_t *obj, bool active, float cs)
{
    if (!obj) return;
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(
        active ? PAL.accent : PAL.border_focus), 0);
    lv_obj_set_style_border_opa(obj, active ? 180 : 110, 0);
    int sw = app->config.low_power ? 0 : (int)((active ? 16.0f : 10.0f) * cs);
    lv_obj_set_style_shadow_width(obj, sw, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(
        active ? PAL.accent : PAL.border_focus), 0);
    lv_obj_set_style_shadow_opa(obj, active ? 55 : 28, 0);
    lv_obj_set_style_shadow_spread(obj, sw > 0 ? 1 : 0, 0);
}

static void ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

typedef struct {
    App *app;
    int index;
} BtClickCtx;

static void bt_clicked(lv_event_t *e)
{
    BtClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    app_control_connect_bluetooth(ctx->app, ctx->index);
}

typedef struct {
    App *app;
    int col;
} RadioClickCtx;

static void radio_row_clicked(lv_event_t *e)
{
    RadioClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    app_control_select_radio(ctx->app, ctx->col);
}

static void refresh_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (app) app_control_rescan_wifi(app);
}

static void radio_switch_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    RadioClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    if (app->control_radio != ctx->col) {
        app->control_radio = ctx->col;
        app->control_col = 0;
        app->control_expanded = true;
        app->control_focus = 0;
    }
    if (ctx->col == 1) {
        app_control_toggle_bluetooth(app);
        bluetooth_scan_async(true);
    } else {
        app_control_toggle_wifi(app);
    }
}

static void control_radio_row(App *app, lv_obj_t *panel, int x, int y,
                              int w, int h, bool bt, float cs,
                              bool compact, bool tiny)
{
    int col = bt ? 1 : 0;
    bool selected = app->control_radio == col;
    bool expanded = selected && app->control_expanded;
    bool on = bt ? app->bt.enabled : app->wifi.enabled;
    bool available = bt ? app->bt.available : app->wifi.available;

    lv_obj_t *row = ui_rect(panel, x, y, w, h, 12,
                            expanded ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(row, expanded ? 34 : (selected ? 65 : 45), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(
        selected ? PAL.accent : PAL.border), 0);
    lv_obj_set_style_border_opa(row, selected ? 150 : 55, 0);
    control_card_glow(app, row, expanded, cs * 0.8f);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    RadioClickCtx *rowctx = xmalloc(sizeof(*rowctx));
    rowctx->app = app;
    rowctx->col = col;
    lv_obj_add_event_cb(row, radio_row_clicked, LV_EVENT_CLICKED, rowctx);
    lv_obj_add_event_cb(row, ctx_free, LV_EVENT_DELETE, rowctx);
    if (app->control_col == 0 && selected)
        ui_set_focus_ring(row, true, PAL.accent);

    int pad = 8;
    int icon = compact ? 18 : 22;
    ui_label(row, bt ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI,
             compact ? FONT_SM : FONT_MD,
             on ? PAL.success : PAL.text3, pad, 6, icon, icon + 4);

    int title_x = pad + icon + 6;
    int chev_w = 14;
    int toggle_w = compact ? 30 : 36;
    ui_label(row, bt ? "蓝牙" : "Wi-Fi",
             compact ? FONT_XS : FONT_SM, PAL.text,
             title_x, 5, w - title_x - chev_w - pad, 18);
    ui_label(row, expanded ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT, FONT_XS,
             selected ? PAL.accent2 : PAL.text3,
             w - chev_w - pad, 7, chev_w, 16);

    char sub[256];
    sub[0] = '\0';
    if (bt) {
        const char *connected = NULL;
        for (int i = 0; i < app->bt.count; i++) {
            if (app->bt.items[i].connected) {
                connected = app->bt.items[i].name;
                break;
            }
        }
        if (!available) {
            snprintf(sub, sizeof(sub), "%s",
                     app->bt.message[0] ? app->bt.message : "不可用");
        } else if (!on) {
            snprintf(sub, sizeof(sub), "已关闭");
        } else if (connected) {
            snprintf(sub, sizeof(sub), "%s", connected);
        } else {
            snprintf(sub, sizeof(sub), "未连接");
        }
    } else {
        if (!available) {
            snprintf(sub, sizeof(sub), "%s",
                     app->wifi.message[0] ? app->wifi.message : "不可用");
        } else if (!on) {
            snprintf(sub, sizeof(sub), "已关闭");
        } else if (app->wifi.current_ssid[0]) {
            snprintf(sub, sizeof(sub), "%s", app->wifi.current_ssid);
        } else {
            snprintf(sub, sizeof(sub), "未连接");
        }
    }
    lv_obj_t *value = ui_label(row, sub,
                               compact || tiny ? FONT_XS : FONT_XS,
                               on ? PAL.text2 : PAL.text3,
                               pad, h - 19,
                               w - pad * 2 - toggle_w - 4, 16);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);

    int toggle_h = 18;
    lv_obj_t *toggle = ui_rect(row, w - toggle_w - pad,
                               h - toggle_h - 5, toggle_w, toggle_h,
                               toggle_h / 2,
                               on ? PAL.success : PAL.elevated);
    lv_obj_set_style_bg_opa(toggle, on ? 220 : 180, 0);
    ui_label_center(toggle, on ? "开" : "关", FONT_XS,
                    on ? 0xFF06111A : PAL.text2, 0, 1, toggle_w, 16);
    lv_obj_add_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
    RadioClickCtx *swctx = xmalloc(sizeof(*swctx));
    swctx->app = app;
    swctx->col = col;
    lv_obj_add_event_cb(toggle, radio_switch_clicked, LV_EVENT_CLICKED, swctx);
    lv_obj_add_event_cb(toggle, ctx_free, LV_EVENT_DELETE, swctx);
}

static void network_clicked(lv_event_t *e)
{
    WifiClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    app_control_connect(ctx->app, ctx->index);
}

static const char *volume_symbol(int vol, bool muted)
{
    if (muted || vol <= 0) return LV_SYMBOL_MUTE;
    if (vol < 50) return LV_SYMBOL_VOLUME_MID;
    return LV_SYMBOL_VOLUME_MAX;
}

static void volume_slider_drag(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    lv_obj_t *hit = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(hit, &a);
    int knob = app->ui.control_volume_knob_size;
    int w = (a.x2 - a.x1) - knob;
    if (w <= 0) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int x = p.x - a.x1 - knob / 2;
    int val = (x * 100 + w / 2) / w;
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    app_control_volume_set(app, val);
}

void ui_update_volume_modal(App *app)
{
    if (!app) return;
    int vol = app->sysvol.percent;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    bool muted = !app->sysvol.available || app->sysvol.muted || vol <= 0;
    uint32_t color = muted ? PAL.text3 : PAL.text2;
    if (app->ui.control_volume_icon) {
        lv_label_set_text(app->ui.control_volume_icon, volume_symbol(vol, muted));
        lv_obj_set_style_text_color(app->ui.control_volume_icon,
                                    lv_color_hex(color), 0);
    }
    if (app->ui.control_volume_value) {
        char buf[16];
        if (!app->sysvol.available) snprintf(buf, sizeof(buf), "--");
        else snprintf(buf, sizeof(buf), "%d%%", vol);
        lv_label_set_text(app->ui.control_volume_value, buf);
        lv_obj_set_style_text_color(app->ui.control_volume_value,
                                    lv_color_hex(PAL.text), 0);
    }
    if (app->ui.control_volume_bar) {
        int w = app->ui.control_volume_bar_full * vol / 100;
        lv_obj_set_width(app->ui.control_volume_bar, w);
    }
    if (app->ui.control_volume_slider) {
        int span = app->ui.control_volume_track_w - app->ui.control_volume_knob_size;
        if (span < 0) span = 0;
        int x = app->ui.control_volume_track_x + span * vol / 100;
        lv_obj_set_x(app->ui.control_volume_slider, x);
    }
}

static void brightness_slider_drag(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    lv_obj_t *hit = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(hit, &a);
    int knob = app->ui.control_brightness_knob_size;
    int w = (a.x2 - a.x1) - knob;
    if (w <= 0) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int x = p.x - a.x1 - knob / 2;
    int val = (x * 100 + w / 2) / w;
    if (val < 1) val = 1;
    if (val > 100) val = 100;
    app_brightness_set(app, val);
}

void ui_update_brightness_modal(App *app)
{
    if (!app) return;
    int pct = system_brightness_get();
    if (pct < 0) pct = 0;
    if (app->ui.control_brightness_icon) {
        lv_label_set_text(app->ui.control_brightness_icon, LV_SYMBOL_EYE_OPEN);
    }
    if (app->ui.control_brightness_value) {
        char buf[16];
        if (system_brightness_available())
            snprintf(buf, sizeof(buf), "%d%%", pct);
        else
            snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(app->ui.control_brightness_value, buf);
    }
    if (app->ui.control_brightness_bar) {
        int w = app->ui.control_brightness_bar_full * pct / 100;
        lv_obj_set_width(app->ui.control_brightness_bar, w);
    }
    if (app->ui.control_brightness_slider) {
        int span = app->ui.control_brightness_track_w -
                   app->ui.control_brightness_knob_size;
        if (span < 0) span = 0;
        int x = app->ui.control_brightness_track_x + span * pct / 100;
        lv_obj_set_x(app->ui.control_brightness_slider, x);
    }
}

static void overlay_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    /* The click that opens the panel can also land on the freshly created
     * overlay. Ignore that first click so the control center stays open. */
    if (lv_tick_get() - app->control_opened_at < 300) return;
    app_close_modal(app);
}

static void panel_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void draw_brightness_strip(App *app, lv_obj_t *card, int y, int cw,
                                  int h, bool compact)
{
    int pct = system_brightness_get();
    if (pct < 0) pct = 0;
    int icon_h = ui_text_height(FONT_SM);
    int value_h = ui_text_height(FONT_XS);
    int row_h = icon_h > value_h ? icon_h : value_h;
    app->ui.control_brightness_icon =
        ui_label(card, LV_SYMBOL_EYE_OPEN, FONT_SM, PAL.text2,
                 14, y + (row_h - icon_h) / 2, 24, icon_h);
    char pbuf[16];
    if (system_brightness_available()) snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    else snprintf(pbuf, sizeof(pbuf), "--");
    app->ui.control_brightness_value =
        ui_label(card, pbuf, FONT_XS, PAL.text3, cw - 14 - 60,
                 y + (row_h - value_h) / 2, 60, value_h);
    lv_obj_set_style_text_align(app->ui.control_brightness_value,
                                LV_TEXT_ALIGN_RIGHT, 0);

    int track_x = 14;
    int track_w = cw - 28;
    if (track_w < 40) track_w = 40;
    int track_y = y + 20;
    int track_h = compact ? 4 : 6;
    int knob = compact ? 16 : 22;
    lv_obj_t *track = ui_rect(card, track_x, track_y, track_w, track_h,
                              track_h / 2, PAL.elevated);
    lv_obj_set_style_bg_opa(track, 235, 0);
    lv_obj_t *fill = ui_rect(card, track_x, track_y, track_w, track_h,
                             track_h / 2, PAL.accent2);
    lv_obj_t *knob_obj = ui_rect(card, track_x,
                                 track_y + track_h / 2 - knob / 2,
                                 knob, knob, knob / 2, 0xFFFFFF);
    lv_obj_set_style_bg_opa(knob_obj, 255, 0);
    lv_obj_set_style_shadow_width(knob_obj, compact ? 6 : 10, 0);
    lv_obj_set_style_shadow_color(knob_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(knob_obj, 90, 0);

    app->ui.control_brightness_bar = fill;
    app->ui.control_brightness_slider = knob_obj;
    app->ui.control_brightness_track_x = ui_scale_x(track_x);
    app->ui.control_brightness_track_w = ui_scale_x(track_w);
    app->ui.control_brightness_knob_size = ui_scale_x(knob);
    app->ui.control_brightness_bar_full = app->ui.control_brightness_track_w;

    int hit_h = h - 6;
    if (hit_h < 20) hit_h = 20;
    int hit_y = y + (h - hit_h) / 2;
    lv_obj_t *hit = ui_rect(card, track_x - knob / 2, hit_y,
                            track_w + knob, hit_h, hit_h / 2, 0x000000);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, brightness_slider_drag, LV_EVENT_PRESSED, app);
    lv_obj_add_event_cb(hit, brightness_slider_drag, LV_EVENT_PRESSING, app);
    (void)h;
    ui_update_brightness_modal(app);
}

static const char *battery_icon(int pct)
{
    if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 70) return LV_SYMBOL_BATTERY_3;
    if (pct >= 45) return LV_SYMBOL_BATTERY_2;
    if (pct >= 20) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static const char *battery_health_text(const char *raw)
{
    if (!raw || !*raw) return NULL;
    if (str_ieq(raw, "Good")) return "良好";
    if (str_ieq(raw, "Normal")) return "正常";
    if (str_ieq(raw, "Fair")) return "一般";
    if (str_ieq(raw, "Poor")) return "较差";
    if (str_ieq(raw, "Unknown")) return "未知";
    return raw;
}

typedef struct {
    App *app;
    int mode;
    bool gpu;
} PowerClickCtx;

static void power_mode_clicked(lv_event_t *e)
{
    PowerClickCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    if (ctx->gpu) app_control_set_gpu(ctx->app, ctx->mode);
    else app_control_set_perf(ctx->app, ctx->mode);
}

static void power_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void draw_power_module(App *app, lv_obj_t *card, int y, int cw,
                              int module_h, bool gpu, bool compact,
                              bool very_compact)
{
    const bool *modes = gpu ? app->gpu.modes : app->perf.modes;
    int count = gpu ? gpu_mode_count(&app->gpu) : perf_mode_count(&app->perf);
    if (count <= 0) return;
    bool active = app->power_module == (gpu ? 1 : 0);
    int title_h = (compact || very_compact) ? 16 : 18;
    lv_font_t *title_font = (compact || very_compact) ? FONT_XS : FONT_SM;
    uint32_t title_color = active ? PAL.accent2 : PAL.text2;
    ui_label(card, gpu ? "GPU" : "CPU", title_font, title_color,
             14, y, 46, title_h);
    if (gpu && !very_compact) {
        ui_label(card, app->gpu.name[0] ? app->gpu.name : "GPU",
                 FONT_XS, PAL.text3, 52, y + 1, cw - 160, title_h);
    }

    int row_y = y + title_h + ((compact || very_compact) ? 2 : 4);
    int pill_h = very_compact ? 18 : (compact ? 22 : 34);
    int gapw = 6;
    int bw = (cw - 24 - (count - 1) * gapw) / count;
    lv_font_t *pill_font = (compact || very_compact) ? FONT_XS : FONT_SM;
    for (int i = 0; i < count; i++) {
        int mode = gpu ? gpu_mode_at(&app->gpu, i) : perf_mode_at(&app->perf, i);
        if (mode < 0) continue;
        int current_mode = gpu ? app->gpu.mode : app->perf.mode;
        bool current = current_mode == mode;
        int px = 12 + i * (bw + gapw);
        lv_obj_t *btn = ui_rect(card, px, row_y, bw, pill_h, pill_h / 2,
                                current ? PAL.accent : PAL.surface);
        lv_obj_set_style_bg_opa(btn, current ? 55 : 80, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn,
            lv_color_hex(current ? PAL.accent : PAL.border), 0);
        lv_obj_set_style_border_opa(btn, current ? 220 : 70, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        if (active && app->control_focus == i)
            ui_set_focus_ring(btn, true, PAL.accent2);
        ui_label_center(btn,
                        gpu ? gpu_mode_label(mode) : perf_mode_label(mode),
                        pill_font, current ? PAL.text : PAL.text2,
                        0, (pill_h - 14) / 2, bw, 16);

        PowerClickCtx *ctx = xmalloc(sizeof(*ctx));
        ctx->app = app;
        ctx->mode = mode;
        ctx->gpu = gpu;
        lv_obj_add_event_cb(btn, power_mode_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, power_ctx_free, LV_EVENT_DELETE, ctx);
    }
    (void)modes;
}

static bool g_control_refresh = false;

void ui_refresh_control_modal(App *app)
{
    if (!app->ui.modal || !app->ui.modal_panel) return;
    if (app->ui.modal_anim_target) {
        lv_anim_del(app->ui.modal_anim_target, NULL);
        app->ui.modal_anim_target = NULL;
    }
    lv_obj_t *old = app->ui.modal_panel;
    app->ui.modal_panel = NULL;
    lv_obj_del(old);
    g_control_refresh = true;
    ui_build_modal_control(app);
    g_control_refresh = false;
    ui_set_draw_scale(1.0f);
}

void ui_build_modal_control(App *app)
{
    UiLayout *L = &app->layout;
    app->ui.control_row_count = 0;
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

    if (!g_control_refresh)
        lv_obj_add_event_cb(app->ui.modal, overlay_clicked, LV_EVENT_CLICKED, app);

    /* Keep the control card proportional to the rest of the UI.  At 720p the
     * scale is 1.0; larger displays grow the card, smaller ones shrink it. */
    float cs = (float)L->font_scale_x100 / 120.0f;
    if (cs < 0.70f) cs = 0.70f;
    if (cs > 1.80f) cs = 1.80f;

    /* Card size follows the viewport, not just the font scale: a 320x240
     * screen should get a small popup, while a 2K screen can grow larger. */
    float vscale = (float)L->w / 1280.0f;
    float vscale_h = (float)L->h / 720.0f;
    if (vscale_h < vscale) vscale = vscale_h;
    if (vscale < 0.65f) vscale = 0.65f;
    if (vscale > 1.75f) vscale = 1.75f;

    int base_w = 420, base_h = 340;
    if (app->control_view == STATUS_WIFI) { base_w = 620; base_h = 480; }
    if (app->control_view == STATUS_BATTERY) {
        base_w = 360;
        base_h = 320;
    }
    else if (app->control_view == STATUS_VOLUME) { base_w = 320; base_h = 240; }
    else if (app->control_view == STATUS_CLOCK) { base_w = 300; base_h = 200; }
    else if (app->control_view == STATUS_PAD) { base_w = 340; base_h = 240; }

    int max_w = L->w - L->pad * 2;
    int width_cap = (int)((float)L->w * 0.80f);
    if (max_w > width_cap) max_w = width_cap;
    int desired_w = (int)((float)base_w * vscale + 0.5f);
    int desired_h = (int)((float)base_h * vscale + 0.5f);
    int pw = ui_min(max_w, desired_w);
    if (pw < 230) pw = ui_min(max_w, 230);
    int py = L->top_y + L->top_h + 6;
    int avail_h = L->footer_y - py - 6;
    float hfrac = L->h <= 400 ? 0.90f : 0.96f;
    int height_cap = (int)((float)avail_h * hfrac);
    int screen_h_cap = (int)((float)L->h *
                             (app->control_view == STATUS_WIFI ? 0.70f : 0.60f));
    if (height_cap > screen_h_cap) height_cap = screen_h_cap;
    int base_ph = ui_min(avail_h, desired_h);
    if (base_ph > height_cap) base_ph = height_cap;
    int min_ph = ui_min(avail_h, 150);
    if (min_ph > height_cap) min_ph = height_cap;
    if (base_ph < min_ph) base_ph = min_ph;
    if (base_ph < 120) base_ph = 120;

    /* 先按实际文字行数测出需要的高度；只有内容变多才增加卡片高度。 */
    int measure_w = (int)((float)pw / cs + 0.5f);
    int measure_inner = ui_clamp(measure_w * 4 / 100, 12, 24);
    int measure_cw = measure_w - measure_inner * 2;
    int base_content_h = (int)((float)base_ph / cs + 0.5f);
    ui_set_draw_scale(cs);
    int need_content_h = control_required_content_h(app, measure_cw, base_content_h);
    int need_ph = (int)((float)need_content_h * cs + 0.5f);
    int ph = base_ph;
    if (need_ph > ph) ph = need_ph;
    if (ph > avail_h) ph = avail_h;

    int px = L->w - L->pad - pw;
    if (px < L->pad) px = L->pad;
    if (px + pw > L->w - L->pad) px = L->w - L->pad - pw;
    if (py < L->top_y + L->top_h) py = L->top_y + L->top_h;
    if (py + ph > L->footer_y - 2) ph = L->footer_y - 2 - py;
    if (ph < 120 && avail_h >= 120) ph = 120;
    /* The battery card has the most content (brightness + battery + CPU/GPU).
     * Let it use the free space below the top bar instead of a fixed height;
     * the card itself still scrolls if even that is not enough. */
    if (app->control_view == STATUS_BATTERY) {
        /* Use every pixel available below the top bar; scroll only if the
         * content is still taller than this. */
        int target = avail_h;
        if (target < 120) target = avail_h;
        if (ph < target) ph = target;
    }

    /* 外层面板保持原始大小，不乘 cs；内部内容再按 cs 缩放。 */
    ui_set_draw_scale(1.0f);
    lv_obj_t *panel = ui_rect(app->ui.modal, px, py, pw, ph,
                              ui_clamp((int)(22.0f * cs), 14, 40), PAL.surface);
    lv_obj_set_style_bg_opa(panel, 245, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border_focus), 0);
    lv_obj_set_style_border_opa(panel, 100, 0);
    control_card_glow(app, panel, false, cs);
    lv_obj_set_style_shadow_opa(panel, 38, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, panel_clicked, LV_EVENT_CLICKED, NULL);
    app->ui.modal_panel = panel;
    if (!g_control_refresh) ui_fade_in_modal(app);

    /* 面板本身不使用 cs；内部内容除以 cs 后，缩放结果刚好铺满面板。 */
    ui_set_draw_scale(cs);
    int content_w = (int)((float)pw / cs + 0.5f);
    int content_h = (int)((float)ph / cs + 0.5f);
    bool tiny = content_h < 210;
    int inner = ui_clamp(content_w * 4 / 100, 12, 24);
    int cw = content_w - inner * 2;
    int header_h = tiny ? 26 : ui_clamp(content_h * 9 / 100, 32, 48);
    const char *heading = app->control_view == STATUS_BATTERY ? "电池" :
                          app->control_view == STATUS_VOLUME ? "音量" :
                          app->control_view == STATUS_CLOCK ? "时间" :
                          app->control_view == STATUS_PAD ? "手柄" : "网络与蓝牙";
    lv_font_t *hfont = tiny ? FONT_MD : (L->w < 600 ? FONT_LG : FONT_XL);
    while (hfont != FONT_XS && lv_font_get_line_height(hfont) > header_h - 2) {
        if (hfont == FONT_XL) hfont = FONT_LG;
        else if (hfont == FONT_LG) hfont = FONT_MD;
        else if (hfont == FONT_MD) hfont = FONT_SM;
        else hfont = FONT_XS;
    }
    int hline = lv_font_get_line_height(hfont);
    ui_label(panel, heading, hfont, PAL.text, inner,
             (header_h - hline) / 2, cw, hline + 2);

    if (app->control_view != STATUS_WIFI) {
        int card_y = header_h + 6;
        int card_h = content_h - card_y - 10;
        lv_obj_t *card = ui_rect(panel, inner, card_y, cw, card_h, 16, PAL.card);
        lv_obj_set_style_bg_opa(card, 90, 0);
        if (app->control_view == STATUS_BATTERY) {
            lv_obj_set_style_pad_all(card, 0, 0);
            lv_obj_set_scroll_dir(card, LV_DIR_VER);
            ui_scrollbar_style(card);
            lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        }
        control_card_glow(app, card, false, cs);
        if (app->control_view == STATUS_BATTERY) {
            int pct = app->sys.battery;
            bool has_cpu = app->perf.available;
            bool has_gpu = app->gpu.available;
            int modules = (has_cpu ? 1 : 0) + (has_gpu ? 1 : 0);
            bool compact;
            bool very_compact;
            if (modules >= 2) {
                very_compact = card_h < 260;
                compact = card_h < 380;
            } else {
                very_compact = card_h < 190;
                compact = card_h < 240;
            }
            int module_h = very_compact ? 30 : (compact ? 34 : 54);
            int module_gap = very_compact ? 2 : 4;
            int bright_h = very_compact ? 32 : (compact ? 36 : 44);
            int bright_gap = 2;
            int body_y = bright_h + bright_gap;
            int body_h = card_h - body_y;
            if (body_h < 40) body_h = 40;
            int footer_h = 0;
            int bottom_pad = 6;
            int modules_total = modules > 0 ?
                modules * module_h + (modules - 1) * module_gap + 6 : 0;
            int hero_h = body_h - modules_total;
            if (hero_h < 0) hero_h = 0;

            int icon_size = very_compact ? 0 : (compact ? 30 : ui_clamp(card_h * 18 / 100, 36, 64));
            lv_font_t *icon_font = icon_size > 54 ? FONT_HERO : FONT_XXL;
            lv_font_t *big_font = very_compact ? FONT_LG : (compact ? FONT_XL : FONT_XXL);
            lv_font_t *state_font = (compact || very_compact) ? FONT_XS : FONT_SM;
            char state[128];
            if (pct < 0) snprintf(state, sizeof(state), "没有检测到电池");
            else snprintf(state, sizeof(state), "%s  /  %s",
                          app->sys.charging ? "充电中" : "使用电池",
                          pct <= 15 ? "电量偏低" : "电量正常");
            int health = app->sys.battery_health;
            char hbuf[96];
            uint32_t hcolor = PAL.text3;
            if (health >= 0) {
                const char *raw = battery_health_text(app->sys.battery_health_label);
                if (raw && *raw)
                    snprintf(hbuf, sizeof(hbuf), "电池健康度  %d%%  /  %s", health, raw);
                else
                    snprintf(hbuf, sizeof(hbuf), "电池健康度  %d%%", health);
                hcolor = health >= 80 ? PAL.success : (health >= 60 ? PAL.warning : PAL.danger);
            } else {
                snprintf(hbuf, sizeof(hbuf), "电池健康度  未知");
            }

            int text_w = cw - 24;
            if (text_w < 40) text_w = 40;
            int h_icon = very_compact ? 0 : (compact ? 30 : icon_size + 6);
            int h_big = very_compact ? 26 : (compact ? 32 : 42);
            int h_state = control_text_height(state, state_font, text_w);
            int h_health = control_text_height(hbuf, FONT_XS, text_w);
            /* 极端缩放时只保留核心信息，避免多行文字叠在卡片底部。 */
            if (card_h < 84) {
                icon_size = 0;
                h_icon = 0;
                big_font = card_h < 58 ? FONT_SM : FONT_MD;
                h_big = ui_text_height(big_font);
                h_health = 0;
                if (card_h < 46) h_state = 0;
            }
            int gap2 = 1;
            int items = (h_icon > 0 ? 1 : 0) + 1 +
                        (h_state > 0 ? 1 : 0) + (h_health > 0 ? 1 : 0);
            int gaps = items > 1 ? items - 1 : 0;
            int stack = h_icon + h_big + h_state + h_health + gap2 * gaps;
            /* The card is vertically scrollable; keep all modules and let the
             * content extend below the viewport instead of hiding them. */
            if (hero_h < stack + 6) hero_h = stack + 6;
            int top = body_y + (hero_h - stack) / 2;
            if (top < body_y + 1) top = body_y + 1;

            if (icon_size > 0) {
                ui_label_center(card, pct >= 0 ? battery_icon(pct) : LV_SYMBOL_CHARGE,
                                icon_font,
                                pct >= 0 && pct <= 15 ? PAL.danger :
                                (app->sys.charging ? PAL.success : PAL.text2),
                                0, top, cw, h_icon);
            }
            char big[64];
            if (pct >= 0) snprintf(big, sizeof(big), "%d%%", pct);
            else snprintf(big, sizeof(big), "AC / 未知");
            int y_cur = top + h_icon;
            if (h_icon > 0) y_cur += gap2;
            ui_label_center(card, big, big_font, PAL.text, 0, y_cur, cw, h_big);
            y_cur += h_big + gap2;
            if (h_state > 0) {
                ui_label_center(card, state, state_font, PAL.text2,
                                (cw - text_w) / 2, y_cur, text_w, h_state);
                y_cur += h_state + gap2;
            }
            if (h_health > 0) {
                ui_label_center(card, hbuf, FONT_XS, hcolor,
                                (cw - text_w) / 2, y_cur, text_w, h_health);
            }

            if (modules > 0) {
                lv_obj_t *divider = ui_rect(card, 12, body_y + hero_h, cw - 24, 1, 0, PAL.border);
                lv_obj_set_style_bg_opa(divider, 120, 0);
                int my = body_y + hero_h + 4;
                if (has_cpu) {
                    draw_power_module(app, card, my, cw, module_h, false,
                                      compact || very_compact, very_compact);
                    my += module_h + (has_gpu ? module_gap : 0);
                }
                if (has_gpu) {
                    draw_power_module(app, card, my, cw, module_h, true,
                                      compact || very_compact, very_compact);
                }
                if (footer_h > 0) {
                    char hint[96];
                    ui_button_hint(app, hint, sizeof(hint), modules > 1 ?
                        "上下切换模块 / 左右选择 / {A} 应用" :
                        "左右选择 / {A} 应用");
                    ui_label(card, hint, FONT_XS, PAL.text3,
                             14, body_y + body_h - 36, cw - 28, 16);
                    const char *detail = (app->power_module == 1 && has_gpu) ?
                        app->gpu.detail : app->perf.detail;
                    if (!detail || !*detail) detail = "由系统驱动管理";
                    ui_label(card, detail, FONT_XS, PAL.text3,
                             14, body_y + body_h - 18, cw - 28, 18);
                }
            }
            draw_brightness_strip(app, card, 2, cw, bright_h,
                                  compact || very_compact);
        } else if (app->control_view == STATUS_VOLUME) {
            int vol = app->sysvol.percent;
            bool muted = !app->sysvol.available || app->sysvol.muted || vol <= 0;
            bool compact = card_h < 180;
            bool ultra = card_h < 92;
            bool show_icon = card_h >= 72;
            bool show_hint = card_h >= 70;
            lv_font_t *big = ultra ? FONT_MD : (compact ? FONT_XL : FONT_XXL);
            int h_big = ui_text_height(big);
            int top = 0;
            app->ui.control_volume_icon = NULL;
            if (show_icon) {
                app->ui.control_volume_icon =
                    ui_label_center(card, volume_symbol(vol, muted), big,
                                    muted ? PAL.text3 : PAL.text2,
                                    0, top, cw, h_big);
                top += h_big + 2;
            }
            char pbuf[16];
            if (!app->sysvol.available) snprintf(pbuf, sizeof(pbuf), "--");
            else snprintf(pbuf, sizeof(pbuf), "%d%%", vol);
            app->ui.control_volume_value =
                ui_label_center(card, pbuf, big, PAL.text,
                                0, top, cw, h_big + 4);

            const char *hint = !app->sysvol.available ? "系统音量不可用（需要 PipeWire / wpctl）" :
                               "拖动滑块 / 左右键调节";
            int hint_h = show_hint ? control_text_height(hint, FONT_XS, cw - 8) : 0;
            int hint_y = show_hint ? card_h - hint_h - 4 : card_h;
            int value_bottom = top + h_big + 4;
            int track_w = cw - 48;
            int track_x = (cw - track_w) / 2;
            int track_y = card_h * 62 / 100;
            if (track_y < value_bottom + 8) track_y = value_bottom + 8;
            if (show_hint && track_y > hint_y - 24) track_y = hint_y - 24;
            if (track_y > card_h - 10) track_y = card_h - 10;
            if (track_y < value_bottom + 4) track_y = value_bottom + 4;
            int track_h = compact ? 4 : 6;
            int knob = compact ? 16 : 22;
            lv_obj_t *track = ui_rect(card, track_x, track_y, track_w, track_h,
                                      track_h / 2, PAL.elevated);
            lv_obj_set_style_bg_opa(track, 235, 0);
            lv_obj_t *fill = ui_rect(card, track_x, track_y, track_w, track_h,
                                     track_h / 2, PAL.accent2);
            lv_obj_t *knob_obj = ui_rect(card, track_x,
                                         track_y + track_h / 2 - knob / 2,
                                         knob, knob, knob / 2, 0xFFFFFF);
            lv_obj_set_style_bg_opa(knob_obj, 255, 0);
            lv_obj_set_style_shadow_width(knob_obj, compact ? 6 : 10, 0);
            lv_obj_set_style_shadow_color(knob_obj, lv_color_hex(0x000000), 0);
            lv_obj_set_style_shadow_opa(knob_obj, 90, 0);

            app->ui.control_volume_bar = fill;
            app->ui.control_volume_slider = knob_obj;
            app->ui.control_volume_track_x = ui_scale_x(track_x);
            app->ui.control_volume_track_w = ui_scale_x(track_w);
            app->ui.control_volume_knob_size = ui_scale_x(knob);
            app->ui.control_volume_bar_full = app->ui.control_volume_track_w;

            lv_obj_t *hit = ui_rect(card, track_x - knob / 2,
                                    track_y - (36 - track_h) / 2,
                                    track_w + knob, 36, 18, 0x000000);
            lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(hit, volume_slider_drag, LV_EVENT_PRESSED, app);
            lv_obj_add_event_cb(hit, volume_slider_drag, LV_EVENT_PRESSING, app);

            if (show_hint) {
                ui_label_center(card, hint, FONT_XS, PAL.text3,
                                4, hint_y, cw - 8, hint_h);
            }
            ui_update_volume_modal(app);
        } else if (app->control_view == STATUS_CLOCK) {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char timebuf[32], datebuf[64];
            strftime(timebuf, sizeof(timebuf), "%H:%M", &tmv);
            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d  %A", &tmv);
            bool ultra = card_h < 84;
            lv_font_t *time_font = ultra ? (card_h < 58 ? FONT_MD : FONT_LG) : FONT_HERO;
            lv_font_t *date_font = ultra ? FONT_XS : FONT_MD;
            bool show_date = card_h >= 52;
            bool show_local = card_h >= 76;
            int time_h = ui_text_height(time_font);
            int date_h = show_date ? control_text_height(datebuf, date_font, cw - 16) : 0;
            int local_h = show_local ? ui_text_height(FONT_XS) : 0;
            int total_h = time_h +
                          (show_date ? 4 + date_h : 0) +
                          (show_local ? 4 + local_h : 0);
            int top_y = (card_h - total_h) / 2;
            if (top_y < 4) top_y = 4;
            ui_label_center(card, timebuf, time_font, PAL.text, 0, top_y, cw, time_h);
            top_y += time_h;
            if (show_date) {
                top_y += 4;
                ui_label_center(card, datebuf, date_font, PAL.text2, 8, top_y,
                                cw - 16, date_h);
                top_y += date_h;
            }
            if (show_local) {
                top_y += 4;
                ui_label_center(card, "本地时间", FONT_XS, PAL.text3, 0, top_y,
                                cw, local_h);
            }
        } else {
            const char *name = backend_controller_name(&app->backend);
            int batt = backend_controller_battery(&app->backend);
            bool ultra = card_h < 84;
            bool show_labels = !ultra && card_h >= 120;
            bool show_hint = card_h >= 60;
            lv_font_t *name_font = ultra ? FONT_SM : (card_h >= 150 ? FONT_LG : FONT_MD);
            lv_font_t *line_font = ultra ? FONT_XS : (card_h >= 150 ? FONT_SM : FONT_XS);
            const char *disp_name = name && *name ? name : "未连接手柄";
            char line[160];
            if (batt >= 0) snprintf(line, sizeof(line), "已连接  /  手柄电量 %d%%", batt);
            else if (name && *name) snprintf(line, sizeof(line), "已连接");
            else snprintf(line, sizeof(line), "使用键盘操作");
            char labels[128];
            snprintf(labels, sizeof(labels), "%s / %s    %s / %s",
                     backend_shoulder_left_label(&app->backend),
                     backend_shoulder_right_label(&app->backend),
                     backend_trigger_left_label(&app->backend),
                     backend_trigger_right_label(&app->backend));
            char hint[64];
            ui_button_hint(app, hint, sizeof(hint), "{A} 确认  {B} 返回");
            int text_w = cw - 16;
            int name_h = control_text_height(disp_name, name_font, text_w);
            int line_h = control_text_height(line, line_font, text_w);
            int labels_h = show_labels ? control_text_height(labels, FONT_XS, text_w) : 0;
            int hint_h = show_hint ? control_text_height(hint, FONT_XS, text_w) : 0;
            int total_h = name_h + 4 + line_h +
                          (labels_h > 0 ? 4 + labels_h : 0);
            int body_h = card_h - (show_hint ? hint_h + 6 : 0);
            int top_y = (body_h - total_h) / 2;
            if (top_y < 4) top_y = 4;
            ui_label_center(card, disp_name, name_font, PAL.text,
                            8, top_y, text_w, name_h);
            top_y += name_h + 4;
            ui_label_center(card, line, line_font, PAL.text2,
                            8, top_y, text_w, line_h);
            top_y += line_h;
            if (labels_h > 0) {
                top_y += 4;
                ui_label_center(card, labels, FONT_XS, PAL.text3,
                                8, top_y, text_w, labels_h);
            }
            if (show_hint) {
                int hint_y = card_h - hint_h - 4;
                ui_label_center(card, hint, FONT_XS, PAL.text3,
                                8, hint_y, text_w, hint_h);
            }
        }
        return;
    }

    int y = header_h;
    int side_w = ui_clamp(cw * 34 / 100, 116, 210);
    int gap = ui_clamp(cw * 2 / 100, 6, 12);
    int right_x = inner + side_w + gap;
    int right_w = cw - side_w - gap;
    if (right_w < 90) right_w = 90;
    bool compact = side_w < 150 || right_w < 260;
    char hintbuf[160];
    ui_button_hint(app, hintbuf, sizeof(hintbuf), compact
        ? "{A} 选择  {X} 开关  {Y} 扫描  {B} 返回"
        : "左右 切换列表    {A} 展开/连接    {X} 开关    {Y} 刷新    {B} 返回");
    const char *hint = hintbuf;
    int bottom_h = control_text_height(hint, FONT_XS, cw - 8) + 4;
    int area_bottom = content_h - bottom_h - 8;
    if (area_bottom < y + 40) area_bottom = y + 40;

    int radio_h = tiny ? 46 : ui_clamp(content_h * 18 / 100, 54, 72);
    control_radio_row(app, panel, inner, y, side_w, radio_h, false,
                      cs, compact, tiny);
    control_radio_row(app, panel, inner, y + radio_h + 8, side_w, radio_h,
                      true, cs, compact, tiny);

    lv_obj_t *divider = ui_rect(panel, inner + side_w + gap / 2, y, 1,
                                area_bottom - y, 0, PAL.border);
    lv_obj_set_style_bg_opa(divider, 120, 0);

    int section_h = tiny ? 16 : 20;
    bool bt = app->control_radio == 1;
    int count = bt ? app->bt.count : app->wifi.count;
    char section[64];
    if (bt) snprintf(section, sizeof(section), "蓝牙设备  %d", count);
    else snprintf(section, sizeof(section), "可用网络  %d", count);
    ui_label(panel, section, FONT_XS, PAL.text3,
             right_x, y, right_w - 66, section_h);
    lv_obj_t *refresh = ui_rect(panel, right_x + right_w - 64, y - 2,
                                64, section_h + 4, 9, PAL.card);
    lv_obj_set_style_bg_opa(refresh, 0, 0);
    lv_obj_set_style_border_width(refresh, 1, 0);
    lv_obj_set_style_border_color(refresh, lv_color_hex(PAL.border), 0);
    lv_obj_set_style_border_opa(refresh, 90, 0);
    lv_obj_add_flag(refresh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, app);
    char refresh_label[32];
    snprintf(refresh_label, sizeof(refresh_label), "%s 刷新",
             backend_button_north_label(&app->backend));
    ui_label_center(refresh, refresh_label, FONT_XS, PAL.text2,
                    0, 1, 64, section_h);

    if (!app->control_expanded) {
        ui_label(panel, "点击左侧的 Wi-Fi 或蓝牙查看扫描设备",
                 FONT_XS, PAL.text3, right_x, y + section_h + 20,
                 right_w, 40);
    } else {
        int list_y = y + section_h + 2;
        int list_h = area_bottom - list_y;
        if (list_h < 28) list_h = 28;
        int row_h = ui_clamp(list_h / 4, 28, 42);

        lv_obj_t *list = lv_obj_create(panel);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, ui_scale_x(right_x), ui_scale_y(list_y));
        lv_obj_set_size(list, ui_scale_x(right_w), ui_scale_y(list_h));
        lv_obj_set_style_pad_all(list, 0, 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        ui_scrollbar_style(list);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

        if (count == 0) {
            const char *empty;
            if (bt) {
                if (!app->bt.available)
                    empty = app->bt.message[0] ? app->bt.message : "BlueZ 不可用";
                else if (!app->bt.enabled)
                    empty = "蓝牙已关闭";
                else
                    empty = "未发现设备，按 {Y} 重新扫描";
            } else {
                empty = app->wifi.available
                    ? (app->wifi.message[0] ? app->wifi.message :
                       "未发现网络，按 {Y} 重新扫描")
                    : "NetworkManager 不可用";
            }
            char emptybuf[192];
            ui_button_hint(app, emptybuf, sizeof(emptybuf), empty);
            int empty_h = control_text_height(emptybuf, FONT_XS, right_w - 8);
            if (empty_h < 22) empty_h = 22;
            ui_label(list, emptybuf, FONT_XS, PAL.text3, 2, 6, right_w - 8, empty_h);
        } else if (!bt) {
            int meta_w = compact ? 44 : 80;
            for (int i = 0; i < app->wifi.count; i++) {
                WifiNetwork *n = &app->wifi.items[i];
                int ry = i * row_h;
                lv_obj_t *row = ui_rect(list, 0, ry, right_w - 4, row_h - 4, 9,
                                        n->active ? PAL.accent : PAL.card);
                lv_obj_set_style_bg_opa(row, n->active ? 30 : 45, 0);
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_color(row,
                    lv_color_hex(n->active ? PAL.accent : PAL.border), 0);
                lv_obj_set_style_border_opa(row, n->active ? 130 : 50, 0);
                control_card_glow(app, row, n->active, cs * 0.65f);
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                if (app->control_col == 1 && app->control_focus == i) {
                    ui_set_focus_ring(row, true, PAL.accent);
                    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
                }

                ui_label(row, LV_SYMBOL_WIFI, FONT_XS,
                         n->signal >= 60 ? PAL.success : PAL.warning,
                         7, (row_h - 18) / 2, 18, 18);
                int name_w = right_w - 26 - meta_w - 8;
                if (name_w < 24) name_w = 24;
                lv_obj_t *name = ui_label(row, n->ssid, FONT_XS, PAL.text,
                                          26, (row_h - 18) / 2, name_w, 18);
                lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
                char meta[64];
                if (compact) {
                    snprintf(meta, sizeof(meta), "%d%%", n->signal);
                } else {
                    snprintf(meta, sizeof(meta), "%d%%%s%s", n->signal,
                             n->secure ? " 加密" : "",
                             n->active ? " 已连接" : "");
                }
                lv_obj_t *m = ui_label(row, meta, FONT_XS,
                                       n->active ? PAL.accent2 : PAL.text3,
                                       right_w - meta_w - 6,
                                       (row_h - 16) / 2, meta_w, 16);
                lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_RIGHT, 0);

                WifiClickCtx *ctx = xmalloc(sizeof(*ctx));
                ctx->app = app;
                ctx->index = i;
                lv_obj_add_event_cb(row, network_clicked, LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(row, ctx_free, LV_EVENT_DELETE, ctx);
                if (app->ui.control_row_count < 64)
                    app->ui.control_rows[app->ui.control_row_count++] = row;
            }
        } else {
            int meta_w = compact ? 40 : 66;
            for (int i = 0; i < app->bt.count; i++) {
                BtDevice *d = &app->bt.items[i];
                int ry = i * row_h;
                lv_obj_t *row = ui_rect(list, 0, ry, right_w - 4, row_h - 4, 9,
                                        d->connected ? PAL.accent : PAL.card);
                lv_obj_set_style_bg_opa(row, d->connected ? 30 : 45, 0);
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_color(row,
                    lv_color_hex(d->connected ? PAL.accent : PAL.border), 0);
                lv_obj_set_style_border_opa(row, d->connected ? 130 : 50, 0);
                control_card_glow(app, row, d->connected, cs * 0.65f);
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                if (app->control_col == 1 && app->control_focus == i) {
                    ui_set_focus_ring(row, true, PAL.accent);
                    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
                }

                ui_label(row, LV_SYMBOL_BLUETOOTH, FONT_XS,
                         d->connected ? PAL.success : PAL.text3,
                         7, (row_h - 18) / 2, 18, 18);
                int name_w = right_w - 26 - meta_w - 8;
                if (name_w < 24) name_w = 24;
                const char *label = d->name[0] ? d->name :
                                   (d->address[0] ? d->address : "未知设备");
                lv_obj_t *name = ui_label(row, label, FONT_XS, PAL.text,
                                          26, (row_h - 18) / 2, name_w, 18);
                lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
                const char *meta = d->connected ?
                    (compact ? "已连" : "已连接") :
                    (d->paired ? (compact ? "配对" : "已配对") : "");
                lv_obj_t *m = ui_label(row, meta, FONT_XS,
                                       d->connected ? PAL.accent2 : PAL.text3,
                                       right_w - meta_w - 6,
                                       (row_h - 16) / 2, meta_w, 16);
                lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_RIGHT, 0);

                BtClickCtx *ctx = xmalloc(sizeof(*ctx));
                ctx->app = app;
                ctx->index = i;
                lv_obj_add_event_cb(row, bt_clicked, LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(row, ctx_free, LV_EVENT_DELETE, ctx);
                if (app->ui.control_row_count < 64)
                    app->ui.control_rows[app->ui.control_row_count++] = row;
            }
        }
    }
    y = content_h - bottom_h - 2;
    /* 底部按键提示；网络面板不显示电量，避免底边与背景大卡片边框重叠。 */
    ui_label_center(panel, hint, FONT_XS, PAL.text3,
                    inner, y, cw - 8, bottom_h);

}
