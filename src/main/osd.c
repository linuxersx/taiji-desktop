// SPDX-License-Identifier: GPL-3.0-or-later
/* Taiji volume OSD.
 *
 * Runs as a small session daemon and shows a volume overlay that stays above
 * fullscreen applications:
 *   - Wayland/sway: wlr-layer-shell OVERLAY surface
 *   - X11: override-redirect SDL window
 *
 * Communication is a Unix datagram socket:
 *   $XDG_RUNTIME_DIR/taiji/osd.sock
 * Messages: "show <percent> <muted>" / "hide"
 */
#define _GNU_SOURCE
#include "config.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include "protocol/wlr-layer-shell-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <lvgl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/memfd.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#include <wayland-client.h>

#define OSD_HIDE_MS 2000
#define OSD_MARGIN 24

typedef enum {
    BACKEND_NONE = 0,
    BACKEND_WAYLAND,
    BACKEND_X11,
} BackendKind;

/* ── shared state ─────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop = 0;
static BackendKind g_backend = BACKEND_NONE;
static int g_w = 320;
static int g_h = 88;
static int g_pad = 0;
static int g_stage_w = 320;
static int g_stage_h = 88;
static int g_card_x = 0;
static int g_card_y = 0;
static int g_screen_w = 1024;
static int g_screen_h = 600;
static int g_percent = 50;
static bool g_muted = false;
static bool g_visible = false;
static int64_t g_deadline = 0;

static lv_display_t *g_disp;
static lv_obj_t *g_scr;
static lv_obj_t *g_card;
static lv_obj_t *g_icon;
static lv_obj_t *g_label;
static lv_obj_t *g_track;
static lv_obj_t *g_fill;
static int g_track_w = 0;
static uint8_t *g_lv_buf;
static size_t g_lv_buf_size;

static int g_sock = -1;
static char g_sock_path[PATH_MAX];
static char g_lock_path[PATH_MAX];

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint32_t tick_cb(void)
{
    return (uint32_t)now_ms();
}

static const char *runtime_dir(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    return rt && *rt ? rt : "/tmp";
}

/* ── LVGL offscreen display ───────────────────────────────────────────────── */

static void flush_noop(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static int osd_radius(void)
{
    int r = g_h * 28 / 100;
    if (r < 12) r = 12;
    if (r > g_h / 2) r = g_h / 2;
    return r;
}

static void ui_build(void)
{
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED);
    g_lv_buf_size = (size_t)g_stage_w * (size_t)g_stage_h * 4u;
    g_lv_buf = calloc(1, g_lv_buf_size);
    lv_display_set_buffers(g_disp, g_lv_buf, NULL, (uint32_t)g_lv_buf_size,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(g_disp, flush_noop);

    g_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_size(g_scr, g_stage_w, g_stage_h);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(g_scr);

    int radius = osd_radius();
    g_card = lv_obj_create(g_scr);
    lv_obj_remove_style_all(g_card);
    lv_obj_set_pos(g_card, g_card_x, g_card_y);
    lv_obj_set_size(g_card, g_w, g_h);
    lv_obj_set_style_radius(g_card, radius, 0);
    lv_obj_set_style_bg_color(g_card, lv_color_hex(PAL.surface), 0);
    lv_obj_set_style_bg_opa(g_card, 244, 0);
    lv_obj_remove_flag(g_card, LV_OBJ_FLAG_SCROLLABLE);
    if (g_backend == BACKEND_WAYLAND) {
        lv_obj_set_style_border_width(g_card, 0, 0);
        lv_obj_set_style_shadow_width(g_card, 16, 0);
        lv_obj_set_style_shadow_color(g_card, lv_color_hex(PAL.accent2), 0);
        lv_obj_set_style_shadow_opa(g_card, 120, 0);
        lv_obj_set_style_shadow_spread(g_card, 1, 0);
    } else {
        lv_obj_set_style_border_width(g_card, 2, 0);
        lv_obj_set_style_border_color(
            g_card, lv_color_hex(color_blend(PAL.card, PAL.accent2, 130)), 0);
        lv_obj_set_style_border_opa(g_card, 255, 0);
        lv_obj_set_style_shadow_width(g_card, 0, 0);
    }

    int pad = g_h * 18 / 100;
    int right_pad = g_h * 34 / 100;
    int icon_w = g_h * 42 / 100;
    lv_font_t *text_font = g_h >= 80 ? FONT_LG : FONT_MD;
    int bar_h = g_h * 9 / 100;
    if (bar_h < 5) bar_h = 5;
    if (bar_h > 10) bar_h = 10;
    int bar_y = g_h - bar_h - g_h * 22 / 100;
    int line_h = lv_font_get_line_height(text_font);
    int text_y = (bar_y - line_h) / 2;
    if (text_y < g_h * 6 / 100) text_y = g_h * 6 / 100;

    g_icon = lv_label_create(g_card);
    lv_obj_set_style_text_font(g_icon, text_font, 0);
    lv_obj_set_style_text_color(g_icon, lv_color_hex(PAL.text), 0);
    lv_obj_set_pos(g_icon, pad, text_y);
    lv_label_set_text(g_icon, LV_SYMBOL_VOLUME_MAX);

    int text_x = pad + icon_w;
    int text_w = g_w - text_x - right_pad;
    if (text_w < 40) text_w = 40;
    g_label = lv_label_create(g_card);
    lv_obj_set_style_text_font(g_label, text_font, 0);
    lv_obj_set_style_text_color(g_label, lv_color_hex(PAL.text), 0);
    lv_obj_set_pos(g_label, text_x, text_y);
    lv_obj_set_width(g_label, text_w);
    lv_label_set_long_mode(g_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_label, "50%");

    g_track_w = text_w;
    g_track = lv_obj_create(g_card);
    lv_obj_remove_style_all(g_track);
    lv_obj_set_pos(g_track, text_x, bar_y);
    lv_obj_set_size(g_track, g_track_w, bar_h);
    lv_obj_set_style_radius(g_track, bar_h / 2, 0);
    lv_obj_set_style_bg_color(g_track, lv_color_hex(PAL.elevated), 0);
    lv_obj_set_style_bg_opa(g_track, 210, 0);

    g_fill = lv_obj_create(g_card);
    lv_obj_remove_style_all(g_fill);
    lv_obj_set_pos(g_fill, text_x, bar_y);
    lv_obj_set_size(g_fill, g_track_w, bar_h);
    lv_obj_set_style_radius(g_fill, bar_h / 2, 0);
    lv_obj_set_style_bg_color(g_fill, lv_color_hex(PAL.accent2), 0);
    lv_obj_set_style_bg_opa(g_fill, 255, 0);
}

static void ui_update(void)
{
    int pct = g_percent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (g_muted) {
        lv_label_set_text(g_icon, LV_SYMBOL_MUTE);
        lv_label_set_text(g_label, tr("静音"));
    } else {
        const char *sym = pct >= 50 ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_VOLUME_MID;
        lv_label_set_text(g_icon, sym);
        lv_label_set_text_fmt(g_label, "%d%%", pct);
    }
    int fill_w = g_track_w * pct / 100;
    if (g_muted) fill_w = 0;
    lv_obj_set_width(g_fill, fill_w > 0 ? fill_w : 0);
    lv_obj_invalidate(g_scr);
}

static void render_lvgl(void)
{
    memset(g_lv_buf, 0, g_lv_buf_size);
    lv_obj_invalidate(g_scr);
    lv_refr_now(g_disp);

}

/* ── Wayland layer-shell backend ──────────────────────────────────────────── */

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
} ShmBuffer;

static struct wl_display *wl_dpy;
static struct wl_compositor *wl_compositor;
static struct wl_shm *wl_shm;
static struct zwlr_layer_shell_v1 *wl_layer_shell;
static struct wl_output *wl_output;
static int wl_out_w = 1024, wl_out_h = 600;

static struct wl_surface *wl_surface;
static struct zwlr_layer_surface_v1 *wl_layer;
static ShmBuffer *wl_current;
static bool wl_configured = false;

static void wl_buffer_release(void *data, struct wl_buffer *buffer)
{
    ShmBuffer *b = data;
    wl_buffer_destroy(buffer);
    if (b->data) munmap(b->data, b->size);
    if (wl_current == b) wl_current = NULL;
    free(b);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static void wl_present(void);

static void wl_layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                               uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;
    (void)width;
    (void)height;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    wl_configured = true;
    if (g_visible) wl_present();
}

static void wl_layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)data;
    (void)layer;
    wl_configured = false;
}

static const struct zwlr_layer_surface_v1_listener wl_layer_listener = {
    .configure = wl_layer_configure,
    .closed = wl_layer_closed,
};

static void wl_output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                               int32_t pw, int32_t ph, int32_t subpixel,
                               const char *make, const char *model, int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void wl_output_mode(void *data, struct wl_output *o, uint32_t flags,
                           int32_t w, int32_t h, int32_t refresh)
{
    (void)data; (void)o; (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        wl_out_w = w > 0 ? w : wl_out_w;
        wl_out_h = h > 0 ? h : wl_out_h;
    }
}

static void wl_output_done(void *data, struct wl_output *o)
{
    (void)data; (void)o;
}

static void wl_output_scale(void *data, struct wl_output *o, int32_t factor)
{
    (void)data; (void)o; (void)factor;
}

static const struct wl_output_listener wl_output_listener = {
    .geometry = wl_output_geometry,
    .mode = wl_output_mode,
    .done = wl_output_done,
    .scale = wl_output_scale,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl_layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && !wl_output) {
        wl_output = wl_registry_bind(reg, name, &wl_output_interface,
                                     version < 2 ? version : 2);
        wl_output_add_listener(wl_output, &wl_output_listener, NULL);
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static bool wl_init(int *screen_w, int *screen_h)
{
    wl_dpy = wl_display_connect(NULL);
    if (!wl_dpy) return false;
    struct wl_registry *reg = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(wl_dpy);
    wl_display_roundtrip(wl_dpy);
    if (!wl_compositor || !wl_shm || !wl_layer_shell) {
        fprintf(stderr, "[taiji-osd] missing layer-shell/compositor/shm\n");
        wl_display_disconnect(wl_dpy);
        wl_dpy = NULL;
        return false;
    }
    *screen_w = wl_out_w;
    *screen_h = wl_out_h;
    return true;
}

static ShmBuffer *wl_create_buffer(void)
{
    int fd = memfd_create("taiji-osd", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    size_t stride = (size_t)g_stage_w * 4u;
    size_t size = stride * (size_t)g_stage_h;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, g_stage_w, g_stage_h,
                                                         (int32_t)stride,
                                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    ShmBuffer *b = calloc(1, sizeof(*b));
    if (!b) {
        wl_buffer_destroy(buffer);
        munmap(data, size);
        return NULL;
    }
    b->buffer = buffer;
    b->data = data;
    b->size = size;
    wl_buffer_add_listener(buffer, &wl_buffer_listener, b);
    return b;
}

static void wl_present(void)
{
    if (!wl_surface || !wl_configured || !g_visible) return;
    render_lvgl();
    ShmBuffer *b = wl_create_buffer();
    if (!b) return;
    memcpy(b->data, g_lv_buf, g_lv_buf_size);
    wl_surface_attach(wl_surface, b->buffer, 0, 0);
    wl_surface_damage_buffer(wl_surface, 0, 0, g_stage_w, g_stage_h);
    wl_surface_commit(wl_surface);
    wl_current = b;
    wl_display_flush(wl_dpy);
}

static void wl_show(void)
{
    if (!wl_surface) {
        wl_surface = wl_compositor_create_surface(wl_compositor);
        wl_layer = zwlr_layer_shell_v1_get_layer_surface(
            wl_layer_shell, wl_surface, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "taiji-osd");
        zwlr_layer_surface_v1_add_listener(wl_layer, &wl_layer_listener, NULL);
        int margin = OSD_MARGIN - g_pad;
        if (margin < 10) margin = 10;
        zwlr_layer_surface_v1_set_anchor(
            wl_layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_margin(wl_layer, (int32_t)margin, (int32_t)margin, 0, 0);
        zwlr_layer_surface_v1_set_size(wl_layer, (uint32_t)g_stage_w, (uint32_t)g_stage_h);
        zwlr_layer_surface_v1_set_exclusive_zone(wl_layer, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            wl_layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        struct wl_region *empty = wl_compositor_create_region(wl_compositor);
        wl_surface_set_input_region(wl_surface, empty);
        wl_region_destroy(empty);

        wl_configured = false;
        wl_surface_commit(wl_surface);
        wl_display_flush(wl_dpy);
    }

    if (wl_configured) wl_present();
}

static void wl_hide(void)
{
    if (wl_layer) {
        zwlr_layer_surface_v1_destroy(wl_layer);
        wl_layer = NULL;
    }
    if (wl_surface) {
        wl_surface_destroy(wl_surface);
        wl_surface = NULL;
    }
    wl_configured = false;
    if (wl_dpy) wl_display_flush(wl_dpy);
}

static void wl_shutdown(void)
{
    wl_hide();
    if (wl_output) wl_output_destroy(wl_output);
    if (wl_layer_shell) zwlr_layer_shell_v1_destroy(wl_layer_shell);
    if (wl_shm) wl_shm_destroy(wl_shm);
    if (wl_compositor) wl_compositor_destroy(wl_compositor);
    if (wl_dpy) wl_display_disconnect(wl_dpy);
}

/* ── X11 override-redirect backend ────────────────────────────────────────── */

static SDL_Window *x_win;
static SDL_Renderer *x_ren;
static SDL_Texture *x_tex;
static Display *x_dpy;
static Window x_wid;
static int x_win_x, x_win_y;

static void x11_apply_shape(void)
{
    if (!x_dpy || !x_wid) return;
    Pixmap mask = XCreatePixmap(x_dpy, x_wid, (unsigned)g_stage_w, (unsigned)g_stage_h, 1);
    if (!mask) return;
    GC gc = XCreateGC(x_dpy, mask, 0, NULL);
    XSetForeground(x_dpy, gc, 0);
    XFillRectangle(x_dpy, mask, gc, 0, 0, (unsigned)g_stage_w, (unsigned)g_stage_h);
    XSetForeground(x_dpy, gc, 1);
    int r = osd_radius();
    if (r > g_w / 2) r = g_w / 2;
    XFillRectangle(x_dpy, mask, gc, r, 0, (unsigned)(g_w - 2 * r), (unsigned)g_h);
    XFillArc(x_dpy, mask, gc, 0, 0, (unsigned)(2 * r), (unsigned)g_h, 90 * 64, 180 * 64);
    XFillArc(x_dpy, mask, gc, g_w - 2 * r, 0, (unsigned)(2 * r), (unsigned)g_h,
             270 * 64, 180 * 64);
    XShapeCombineMask(x_dpy, x_wid, ShapeBounding, 0, 0, mask, ShapeSet);
    XShapeCombineRectangles(x_dpy, x_wid, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    XFreePixmap(x_dpy, mask);
    XFreeGC(x_dpy, gc);
    XFlush(x_dpy);
}

static void x11_set_above(void)
{
    if (!x_dpy || !x_wid) return;
    Atom state = XInternAtom(x_dpy, "_NET_WM_STATE", False);
    Atom above = XInternAtom(x_dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(x_dpy, x_wid, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&above, 1);
    Atom type = XInternAtom(x_dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom notify = XInternAtom(x_dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    XChangeProperty(x_dpy, x_wid, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&notify, 1);
    XWindowAttributes attrs;
    if (XGetWindowAttributes(x_dpy, x_wid, &attrs) && !attrs.override_redirect) {
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.override_redirect = True;
        XChangeWindowAttributes(x_dpy, x_wid, CWOverrideRedirect, &swa);
    }
    XFlush(x_dpy);
}

static bool x11_init(int *screen_w, int *screen_h)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[taiji-osd] SDL init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(0, &r) != 0) {
        r.x = 0; r.y = 0; r.w = 1024; r.h = 600;
    }
    *screen_w = r.w;
    *screen_h = r.h;
    return true;
}

static void x11_create_window(int screen_w, int screen_h)
{
    (void)screen_h;
    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                   SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_HIDDEN;
#ifdef SDL_WINDOW_POPUP_MENU
    flags |= SDL_WINDOW_POPUP_MENU;
#endif
    x_win = SDL_CreateWindow("Taiji Volume OSD", 0, 0, g_stage_w, g_stage_h, flags);
    if (!x_win) {
        fprintf(stderr, "[taiji-osd] window failed: %s\n", SDL_GetError());
        return;
    }
    x_ren = SDL_CreateRenderer(x_win, -1, SDL_RENDERER_SOFTWARE);
    if (!x_ren) x_ren = SDL_CreateRenderer(x_win, -1, 0);
    if (x_ren) {
        x_tex = SDL_CreateTexture(x_ren, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, g_stage_w, g_stage_h);
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(x_win, &info) && info.subsystem == SDL_SYSWM_X11) {
        x_dpy = info.info.x11.display;
        x_wid = info.info.x11.window;
        x11_set_above();
        x11_apply_shape();
    }
    x_win_x = screen_w - g_w - OSD_MARGIN;
    if (x_win_x < 0) x_win_x = 0;
    x_win_y = OSD_MARGIN;
    SDL_SetWindowPosition(x_win, x_win_x, x_win_y);
}

static void x11_show(void)
{
    if (!x_win) return;
    x_win_x = g_screen_w - g_stage_w - OSD_MARGIN;
    if (x_win_x < 0) x_win_x = 0;
    SDL_SetWindowPosition(x_win, x_win_x, OSD_MARGIN);
    SDL_ShowWindow(x_win);
    SDL_RaiseWindow(x_win);

    render_lvgl();
    if (x_ren && x_tex) {
        SDL_UpdateTexture(x_tex, NULL, g_lv_buf, g_stage_w * 4);
        SDL_SetTextureBlendMode(x_tex, SDL_BLENDMODE_NONE);
        SDL_RenderClear(x_ren);
        SDL_RenderCopy(x_ren, x_tex, NULL, NULL);
        SDL_RenderPresent(x_ren);
    }
    if (x_dpy && x_wid) {
        XRaiseWindow(x_dpy, x_wid);
        XFlush(x_dpy);
    }
}

static void x11_hide(void)
{
    if (x_win) SDL_HideWindow(x_win);
}

static void x11_shutdown(void)
{
    if (x_tex) SDL_DestroyTexture(x_tex);
    if (x_ren) SDL_DestroyRenderer(x_ren);
    if (x_win) SDL_DestroyWindow(x_win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* ── show / hide ──────────────────────────────────────────────────────────── */

static void osd_show(void)
{
    g_visible = true;
    g_deadline = now_ms() + OSD_HIDE_MS;
    ui_update();
    if (g_backend == BACKEND_WAYLAND) wl_show();
    else if (g_backend == BACKEND_X11) x11_show();
}

static void osd_hide(void)
{
    if (!g_visible) return;
    if (g_backend == BACKEND_WAYLAND) wl_hide();
    else if (g_backend == BACKEND_X11) x11_hide();
    g_visible = false;
}

/* ── socket ───────────────────────────────────────────────────────────────── */

static int open_socket(void)
{
    const char *rt = runtime_dir();
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/taiji", rt);
    mkdir(dir, 0700);
    snprintf(g_sock_path, sizeof(g_sock_path), "%s/osd.sock", dir);
    snprintf(g_lock_path, sizeof(g_lock_path), "%s/osd.lock", dir);

    int lock = open(g_lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
        close(lock);
        fprintf(stderr, "[taiji-osd] already running\n");
        return -1;
    }

    unlink(g_sock_path);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(g_sock_path);
    if (plen >= sizeof(addr.sun_path)) plen = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, g_sock_path, plen);
    addr.sun_path[plen] = '\0';
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(g_sock_path, 0600);
    g_sock = fd;
    return 0;
}

static void handle_message(const char *msg)
{
    fprintf(stderr, "[taiji-osd] message: %s\n", msg);
    if (strncmp(msg, "show", 4) == 0) {
        int pct = g_percent;
        int muted = g_muted ? 1 : 0;
        sscanf(msg + 4, "%d %d", &pct, &muted);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        g_percent = pct;
        g_muted = muted != 0;
        osd_show();
    } else if (strncmp(msg, "hide", 4) == 0) {
        osd_hide();
    }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

static bool pick_backend(int *sw, int *sh)
{
    const char *wd = getenv("WAYLAND_DISPLAY");
    const char *dis = getenv("DISPLAY");
    if ((wd && *wd) || !dis || !*dis) {
        if (wl_init(sw, sh)) {
            g_backend = BACKEND_WAYLAND;
            return true;
        }
    }
    if (dis && *dis) {
        if (x11_init(sw, sh)) {
            g_backend = BACKEND_X11;
            return true;
        }
    }
    if (!wd || !*wd) {
        if (wl_init(sw, sh)) {
            g_backend = BACKEND_WAYLAND;
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!getenv("TAIJI_ASSETS") && dir_exists("/usr/share/taiji/assets"))
        setenv("TAIJI_ASSETS", "/usr/share/taiji/assets", 1);

    /* Never inherit Taiji's renderer override; the OSD picks its own. */
    unsetenv("SDL_RENDER_DRIVER");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGCHLD, SIG_IGN);

    if (open_socket() != 0) return 0;

    Config cfg;
    config_init(&cfg);
    config_load(&cfg);
    i18n_set_language(cfg.language);

    if (!pick_backend(&g_screen_w, &g_screen_h)) {
        fprintf(stderr, "[taiji-osd] no usable display backend\n");
        return 1;
    }

    g_pad = g_backend == BACKEND_WAYLAND ? 18 : 0;
    int max_w = g_screen_w - OSD_MARGIN * 2 - g_pad * 2;
    g_w = g_screen_w * 30 / 100;
    if (g_w < 220) g_w = 220;
    if (g_w > 400) g_w = 400;
    if (g_w > max_w) g_w = max_w;
    if (g_w < 80) g_w = 80;
    g_h = g_w * 28 / 100;
    if (g_h < 64) g_h = 64;
    if (g_h > 112) g_h = 112;
    g_card_x = g_pad;
    g_card_y = g_pad;
    g_stage_w = g_w + g_pad * 2;
    g_stage_h = g_h + g_pad * 2;

    lv_init();
    lv_tick_set_cb(tick_cb);
    g_disp = lv_display_create(g_stage_w, g_stage_h);
    theme_init();
    theme_set_preset(cfg.theme);
    theme_set_scale(1.0f);
    ui_build();

    if (g_backend == BACKEND_X11) {
        x11_create_window(g_screen_w, g_screen_h);
        if (!x_win) return 1;
    }

    int wl_fd = g_backend == BACKEND_WAYLAND && wl_dpy ? wl_display_get_fd(wl_dpy) : -1;

    while (!g_stop) {
        struct pollfd fds[2];
        int n = 0;
        fds[n].fd = g_sock;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
        if (wl_fd >= 0) {
            fds[n].fd = wl_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }

        int timeout = 200;
        if (g_visible) {
            int64_t left = g_deadline - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }

        int rc = poll(fds, (nfds_t)n, timeout);
        if (rc < 0 && errno != EINTR) break;

        if (fds[0].revents & POLLIN) {
            char buf[128];
            ssize_t got = recv(g_sock, buf, sizeof(buf) - 1, 0);
            if (got > 0) {
                buf[got] = '\0';
                handle_message(buf);
            }
        }

        if (wl_fd >= 0) {
            if (fds[1].revents & POLLIN) {
                if (wl_display_dispatch(wl_dpy) < 0) break;
            } else if (fds[1].revents & POLLERR) {
                break;
            }
            wl_display_flush(wl_dpy);
        }

        if (g_backend == BACKEND_X11) SDL_PumpEvents();

        if (g_visible && now_ms() >= g_deadline) osd_hide();
    }

    osd_hide();
    if (g_backend == BACKEND_WAYLAND) wl_shutdown();
    else if (g_backend == BACKEND_X11) x11_shutdown();
    if (g_sock >= 0) close(g_sock);
    unlink(g_sock_path);
    config_free(&cfg);
    free(g_lv_buf);
    return 0;
}
