// SPDX-License-Identifier: GPL-3.0-or-later
#include "store_ui.h"
#include "store_cache.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static float g_ds = 1.0f;

static int ds(int v)
{
    int r = (int)((float)v * g_ds + 0.5f);
    return r < 1 ? 1 : r;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int r, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, lv_font_t *font,
                       uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_label_set_text(l, tr(text ? text : ""));
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    return l;
}

static lv_obj_t *label_center(lv_obj_t *parent, const char *text, lv_font_t *font,
                              uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = label(parent, text, font, color, x, y, w, h);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static char *img_src(const char *path)
{
    char *out = xmalloc(strlen(path) + 3);
    memcpy(out, "A:", 2);
    strcpy(out + 2, path);
    return out;
}

const char *store_category_name(int category)
{
    static const char *names[] = {
        "全部", "已安装", "可更新", "游戏", "开发", "影音",
        "网络", "系统", "工具", "桌面"
    };
    if (category < 0 || category >= STORE_CAT_COUNT) category = 0;
    return names[category];
}

bool store_ui_any_backend(char *error, size_t error_size)
{
    int n = store_backend_count();
    for (int i = 0; i < n; i++) {
        StoreBackend *b = store_backend_at(i);
        char err[256] = "";
        if (b && b->probe(err, sizeof(err))) return true;
        if (error && !error[0]) snprintf(error, error_size, "%s", err);
    }
    if (error && !error[0]) snprintf(error, error_size, "没有可用的软件源");
    return false;
}

static bool category_matches(const StorePackage *p, int category)
{
    switch (category) {
        case STORE_CAT_ALL: return true;
        case STORE_CAT_INSTALLED: return p->installed;
        case STORE_CAT_UPDATES: return p->update_available;
        case STORE_CAT_GAMES: return strcmp(p->category, "games") == 0;
        case STORE_CAT_DEV: return strcmp(p->category, "development") == 0;
        case STORE_CAT_MEDIA: return strcmp(p->category, "media") == 0;
        case STORE_CAT_NETWORK: return strcmp(p->category, "network") == 0;
        case STORE_CAT_SYSTEM: return strcmp(p->category, "system") == 0;
        case STORE_CAT_UTILITY: return strcmp(p->category, "utility") == 0;
        case STORE_CAT_DESKTOP: return strcmp(p->category, "desktop") == 0;
        default: return true;
    }
}

static bool search_matches(const StorePackage *p, const char *search)
{
    if (!search || !*search) return true;
    return strcasestr(p->name, search) != NULL ||
           strcasestr(p->id, search) != NULL ||
           strcasestr(p->description, search) != NULL;
}

static void rebuild_filter(StoreApp *app);
static void rebuild_ui(StoreApp *app);
static void rebuild_ui(StoreApp *app)
{
    store_ui_build(app);
}

static void rebuild_filter(StoreApp *app)
{
    free(app->filter);
    app->filter = NULL;
    app->filter_count = 0;
    if (app->all.count <= 0) return;
    app->filter = xmalloc(sizeof(int) * (size_t)app->all.count);
    for (int i = 0; i < app->all.count; i++) {
        StorePackage *p = &app->all.items[i];
        if (category_matches(p, app->category) && search_matches(p, app->search))
            app->filter[app->filter_count++] = i;
    }
    if (app->focus >= app->filter_count)
        app->focus = app->filter_count > 0 ? app->filter_count - 1 : 0;
    if (app->focus < 0) app->focus = 0;
    if (app->show_detail && app->detail_index >= app->filter_count)
        app->show_detail = false;
}

static int cmp_pkg(const void *a, const void *b)
{
    const StorePackage *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    if (c != 0) return c;
    return strcmp(x->source, y->source);
}

static void *refresh_worker(void *arg)
{
    StoreApp *app = arg;
    StorePackageList result = {0};
    bool ok = true;
    char error[256] = "";
    int n = store_backend_count();
    int usable = 0;
    for (int i = 0; i < n && !app->refresh_cancel; i++) {
        StoreBackend *b = store_backend_at(i);
        char err[256] = "";
        if (!b || !b->probe(err, sizeof(err))) {
            if (!error[0] && err[0]) snprintf(error, sizeof(error), "%s", err);
            continue;
        }
        usable++;
        StorePackageList one = {0};
        if (b->refresh(b, &one, NULL, err, sizeof(err))) {
            store_cache_save(b->source, &one);
            for (int k = 0; k < one.count; k++) store_list_push(&result, &one.items[k]);
        } else {
            ok = false;
            if (!error[0]) snprintf(error, sizeof(error), "%s", err);
        }
        store_list_free(&one);
    }
    if (usable == 0 && !error[0])
        snprintf(error, sizeof(error), "没有可用的软件源（检查 pacman / helper）");
    if (result.items) qsort(result.items, (size_t)result.count, sizeof(StorePackage), cmp_pkg);
    pthread_mutex_lock(&app->lock);
    store_list_free(&app->refresh_result);
    app->refresh_result = result;
    snprintf(app->refresh_error, sizeof(app->refresh_error), "%s", error);
    app->refresh_ok = ok;
    app->refresh_done = true;
    pthread_mutex_unlock(&app->lock);
    return NULL;
}

void store_ui_refresh(StoreApp *app)
{
    if (!app || app->refreshing) return;
    app->refreshing = true;
    app->refresh_done = false;
    app->refresh_cancel = false;
    app->refresh_ok = false;
    app->refresh_error[0] = '\0';
    if (pthread_create(&app->refresh_thread, NULL, refresh_worker, app) != 0) {
        app->refreshing = false;
        return;
    }
    app->dirty = true;
}

static void task_progress(void *ud, const char *stage, int percent, const char *line)
{
    StoreApp *app = ud;
    pthread_mutex_lock(&app->lock);
    if (stage) snprintf(app->task_stage, sizeof(app->task_stage), "%s", stage);
    app->task_percent = percent;
    if (line) snprintf(app->task_line, sizeof(app->task_line), "%s", line);
    pthread_mutex_unlock(&app->lock);
}

static void *task_worker(void *arg)
{
    StoreApp *app = arg;
    StoreProgress progress = {task_progress, app, &app->task_cancel};
    char error[256] = {0};
    bool ok = false;
    StoreBackend *b = app->task_backend;
    if (b) {
        if (app->task_kind == 0)
            ok = b->install(b, (const char **)app->task_ids, app->task_id_count,
                            &progress, error, sizeof(error));
        else if (app->task_kind == 1)
            ok = b->remove(b, (const char **)app->task_ids, app->task_id_count,
                           &progress, error, sizeof(error));
        else
            ok = b->update(b, NULL, 0, &progress, error, sizeof(error));
    }
    pthread_mutex_lock(&app->lock);
    app->task_ok = ok;
    if (!ok && error[0]) snprintf(app->task_error, sizeof(app->task_error), "%s", error);
    app->task_done = true;
    pthread_mutex_unlock(&app->lock);
    return NULL;
}

void store_ui_start_task(StoreApp *app, int kind, int pkg_index)
{
    if (!app || app->task_running) return;
    if (kind != 2) {
        if (pkg_index < 0 || pkg_index >= app->filter_count) return;
        StorePackage *p = &app->all.items[app->filter[pkg_index]];
        StoreBackend *b = store_backend_find(p->source);
        if (!b) return;
        if (kind == 1 && !p->removable) {
            snprintf(app->task_error, sizeof(app->task_error), "该包受保护，不能卸载");
            app->show_detail = false;
            app->dirty = true;
            return;
        }
        app->task_backend = b;
        app->task_ids = xmalloc(sizeof(char *));
        app->task_ids[0] = xstrdup(p->id);
        app->task_id_count = 1;
    } else {
        StoreBackend *b = store_backend_find("pacman");
        for (int i = 0; i < store_backend_count() && !b; i++) b = store_backend_at(i);
        if (!b) return;
        app->task_backend = b;
        app->task_ids = NULL;
        app->task_id_count = 0;
    }
    app->task_kind = kind;
    app->task_running = true;
    app->task_done = false;
    app->task_ok = false;
    app->task_cancel = false;
    app->task_percent = -1;
    snprintf(app->task_stage, sizeof(app->task_stage), "准备中...");
    app->task_line[0] = '\0';
    app->task_error[0] = '\0';
    app->show_detail = false;
    if (pthread_create(&app->task_thread, NULL, task_worker, app) != 0) {
        app->task_running = false;
        return;
    }
    app->dirty = true;
}

void store_ui_init(StoreApp *app)
{
    memset(app, 0, sizeof(*app));
    config_init(&app->config);
    config_load(&app->config);
    i18n_set_language(app->config.language);
    pthread_mutex_init(&app->lock, NULL);
    for (int i = 0; i < store_backend_count(); i++) {
        StoreBackend *b = store_backend_at(i);
        char error[256] = "";
        if (!b || !b->probe(error, sizeof(error))) continue;
        StorePackageList list = {0};
        if (store_cache_load(b->source, &list)) {
            for (int k = 0; k < list.count; k++) store_list_push(&app->all, &list.items[k]);
        }
        store_list_free(&list);
    }
    if (app->all.items)
        qsort(app->all.items, (size_t)app->all.count, sizeof(StorePackage), cmp_pkg);
    rebuild_filter(app);
    app->running = true;
    app->dirty = true;
}

void store_ui_sync_config(StoreApp *app, const Config *config)
{
    if (!app || !config) return;
    app->config.language = config->language;
    app->config.theme = config->theme;
    app->config.theme_auto = config->theme_auto;
    app->config.auto_dark_min = config->auto_dark_min;
    app->config.auto_light_min = config->auto_light_min;
    app->config.ui_scale = config->ui_scale;
    app->config.ui_scale_auto = config->ui_scale_auto;
    app->config.low_power = config->low_power;
}

void store_ui_set_view(StoreApp *app, int width, int height, int percent,
                       lv_obj_t *root)
{
    if (!app) return;
    app->view_w = width > 0 ? width : 1280;
    app->view_h = height > 0 ? height : 720;
    app->percent = percent > 0 ? percent : 100;
    if (root) app->root = root;
    app->dirty = true;
}

void store_ui_init_embedded(StoreApp *app, const Config *config, int width,
                            int height, int percent, lv_obj_t *root)
{
    store_ui_init(app);
    app->embedded = true;
    store_ui_sync_config(app, config);
    store_ui_set_view(app, width, height, percent, root);
}

static void build_sidebar(StoreApp *app, int x, int y, int w, int h)
{
    lv_obj_t *box0 = box(app->root, x, y, w, h, ds(14), PAL.card);
    lv_obj_set_style_bg_opa(box0, 150, 0);
    lv_obj_set_scroll_dir(box0, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box0, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(box0, LV_OBJ_FLAG_SCROLLABLE);
    int row_h = ds(44);
    int gap = ds(4);
    for (int i = 0; i < STORE_CAT_COUNT; i++) {
        int ry = ds(8) + i * (row_h + gap);
        if (ry + row_h > h) break;
        bool sel = app->on_sidebar ? app->side_focus == i : app->category == i;
        lv_obj_t *row = box(box0, ds(6), ry, w - ds(12), row_h, ds(10),
                            sel ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(row, sel ? 45 : 0, 0);
        label(row, store_category_name(i), FONT_SM,
              sel ? PAL.accent2 : PAL.text2, ds(12), ds(12), w - ds(24), ds(20));
        if (sel) lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        if (app->on_sidebar && sel) {
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(PAL.accent), 0);
            lv_obj_set_style_border_opa(row, 200, 0);
        }
    }
    (void)row_h;
}

static void build_row(StoreApp *app, lv_obj_t *list, int pkg_index, int y,
                      int w, int row_h)
{
    StorePackage *p = &app->all.items[app->filter[pkg_index]];
    bool sel = !app->on_sidebar && app->focus == pkg_index;
    lv_obj_t *row = box(list, 0, y, w, row_h - ds(4), ds(12),
                        sel ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(row, sel ? 38 : 70, 0);
    lv_obj_set_style_border_width(row, sel ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PAL.accent), 0);
    lv_obj_set_style_border_opa(row, 180, 0);

    int icon_sz = row_h - ds(18);
    if (icon_sz > ds(56)) icon_sz = ds(56);
    int icon_y = (row_h - icon_sz) / 2;
    if (p->icon[0] && access(p->icon, R_OK) == 0) {
        lv_obj_t *img = lv_image_create(row);
        lv_obj_remove_style_all(img);
        lv_obj_set_pos(img, ds(10), icon_y);
        lv_obj_set_size(img, icon_sz, icon_sz);
        char *src = img_src(p->icon);
        lv_image_set_src(img, src);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        free(src);
    } else {
        lv_obj_t *ph = box(row, ds(10), icon_y, icon_sz, icon_sz, ds(9), PAL.elevated);
        label_center(ph, p->name[0] ? (char[]){p->name[0], 0} : "?",
                     FONT_MD, PAL.text2, 0, ds(6), icon_sz, icon_sz - ds(8));
    }
    int tx = ds(10) + icon_sz + ds(12);
    int right_w = w / 3;
    int tw = w - tx - right_w - ds(12);
    if (tw < ds(40)) tw = ds(40);
    label(row, p->name, FONT_MD, PAL.text, tx, ds(8), tw, ds(22));
    lv_obj_t *desc = label(row, p->description[0] ? p->description : p->id,
                           FONT_XS, PAL.text3, tx, ds(32), tw, ds(18));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);

    const char *status;
    uint32_t color;
    if (p->update_available) { status = "可更新"; color = PAL.warning; }
    else if (p->installed) { status = "已安装"; color = PAL.success; }
    else { status = "未安装"; color = PAL.text3; }
    label(row, status, FONT_XS, color, w - right_w - ds(8), ds(10),
          right_w, ds(18));
    char meta[128];
    snprintf(meta, sizeof(meta), "%s%s%s", p->source,
             p->installed_version[0] ? "  " : "", p->installed_version);
    label(row, meta, FONT_XS, PAL.text3, w - right_w - ds(8), ds(32),
          right_w, ds(16));
}

static void build_detail(StoreApp *app, int w, int h)
{
    if (app->detail_index < 0 || app->detail_index >= app->filter_count) return;
    StorePackage *p = &app->all.items[app->filter[app->detail_index]];
    int pw = w * 72 / 100;
    int ph = h * 76 / 100;
    if (pw < ds(260)) pw = w - ds(20);
    if (ph < ds(220)) ph = h - ds(20);
    int px = (w - pw) / 2;
    int py = (h - ph) / 2;
    lv_obj_t *shade = box(app->root, 0, 0, w, h, 0, 0x000000);
    lv_obj_set_style_bg_opa(shade, 150, 0);
    lv_obj_t *panel = box(app->root, px, py, pw, ph, ds(18), PAL.surface);
    lv_obj_set_style_bg_opa(panel, 250, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border_focus), 0);
    label(panel, p->name, FONT_XL, PAL.text, ds(24), ds(18), pw - ds(48), ds(34));
    label(panel, p->id, FONT_XS, PAL.text3, ds(24), ds(52), pw - ds(48), ds(18));
    char meta[512];
    snprintf(meta, sizeof(meta), "来源 %s   %s   %s   %lld KB",
             p->source, p->version, p->license[0] ? p->license : "-", p->size_kb);
    label(panel, meta, FONT_XS, PAL.text2, ds(24), ds(78), pw - ds(48), ds(18));
    lv_obj_t *desc = label(panel, p->description[0] ? p->description : "-",
                           FONT_SM, PAL.text2, ds(24), ds(106), pw - ds(48),
                           ph - ds(190));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    const char *action;
    if (p->update_available) action = "A 更新";
    else if (p->installed) action = p->removable ? "A 卸载" : "受保护，不能卸载";
    else action = "A 安装";
    label(panel, action, FONT_SM,
          p->installed && !p->removable ? PAL.text3 : PAL.accent2,
          ds(24), ph - ds(62), pw - ds(48), ds(22));
    label(panel, "B 返回", FONT_XS, PAL.text3, ds(24), ph - ds(34),
          pw - ds(48), ds(18));
}

static void build_task_overlay(StoreApp *app, int w, int h)
{
    lv_obj_t *shade = box(app->root, 0, 0, w, h, 0, 0x000000);
    lv_obj_set_style_bg_opa(shade, 150, 0);
    int pw = w * 60 / 100;
    if (pw < ds(240)) pw = w - ds(20);
    int ph = ds(190);
    int px = (w - pw) / 2;
    int py = (h - ph) / 2;
    lv_obj_t *panel = box(app->root, px, py, pw, ph, ds(18), PAL.surface);
    lv_obj_set_style_bg_opa(panel, 250, 0);
    pthread_mutex_lock(&app->lock);
    char stage[256], line[256], err[256];
    int pct = app->task_percent;
    snprintf(stage, sizeof(stage), "%s", app->task_stage);
    snprintf(line, sizeof(line), "%s", app->task_line);
    snprintf(err, sizeof(err), "%s", app->task_error);
    pthread_mutex_unlock(&app->lock);
    label(panel, stage[0] ? stage : "正在处理...", FONT_LG, PAL.text,
          ds(20), ds(20), pw - ds(40), ds(30));
    if (pct >= 0) {
        char pb[64];
        snprintf(pb, sizeof(pb), "%d%%", pct);
        lv_obj_t *bar = box(panel, ds(20), ds(62), pw - ds(40), ds(8), ds(4), PAL.elevated);
        int fill = (pw - ds(40)) * pct / 100;
        if (fill > 0) box(bar, 0, 0, fill, ds(8), ds(4), PAL.accent2);
        label(panel, pb, FONT_XS, PAL.text2, ds(20), ds(76), pw - ds(40), ds(16));
    }
    lv_obj_t *ln = label(panel, line[0] ? line : "", FONT_XS, PAL.text3,
                         ds(20), ds(100), pw - ds(40), ds(42));
    lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
    if (err[0]) label(panel, err, FONT_XS, PAL.danger, ds(20), ds(144),
                      pw - ds(40), ds(20));
    label_center(panel, "B 取消", FONT_XS, PAL.text3, 0, ph - ds(24),
                 pw, ds(18));
}

void store_ui_build(StoreApp *app)
{
    if (!app->root) return;
    int w = app->view_w > 0 ? app->view_w : app->backend.width;
    int h = app->view_h > 0 ? app->view_h : app->backend.height;
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    int percent = app->percent > 0 ? app->percent : 100;
    g_ds = ((float)h / 720.0f) * ((float)percent / 100.0f);
    if (g_ds < 0.55f) g_ds = 0.55f;
    if (g_ds > 2.8f) g_ds = 2.8f;
    lv_obj_clean(app->root);
    box(app->root, 0, 0, w, h, 0, PAL.bg);

    int top_h = ds(76);
    label(app->root, "应用商店", FONT_XL, PAL.text, ds(16), ds(12), w / 2, ds(34));
    lv_obj_t *search = box(app->root, ds(16), ds(48), w / 2, ds(24), ds(12), PAL.card);
    lv_obj_set_style_bg_opa(search, 160, 0);
    char sline[160];
    if (app->search_edit)
        snprintf(sline, sizeof(sline), "%s|", app->search);
    else
        snprintf(sline, sizeof(sline), "%s%s", app->search[0] ? app->search : "按 X 搜索",
                 app->search[0] ? "" : "");
    label(search, sline, FONT_XS, app->search[0] ? PAL.text : PAL.text3,
          ds(10), ds(4), w / 2 - ds(20), ds(16));

    int sw = w * 22 / 100;
    if (sw < ds(120)) sw = ds(120);
    if (sw > ds(240)) sw = ds(240);
    int side_h = h - top_h - ds(12);
    build_sidebar(app, ds(12), top_h, sw, side_h);

    int lx = ds(12) + sw + ds(10);
    int lw = w - lx - ds(12);
    int list_y = top_h + ds(6);
    int list_h = h - list_y - ds(12);
    char count[128];
    snprintf(count, sizeof(count), "%d 个应用", app->filter_count);
    label(app->root, count, FONT_XS, PAL.text3, lx, top_h - ds(18), lw / 2, ds(16));
    if (app->refreshing)
        label(app->root, "正在刷新...", FONT_XS, PAL.accent2,
              lx + lw / 2, top_h - ds(18), lw / 2, ds(16));
    if (app->notice[0])
        label(app->root, app->notice, FONT_XS, PAL.warning,
              lx, h - ds(22), lw, ds(18));

    lv_obj_t *list = lv_obj_create(app->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, lx, list_y);
    lv_obj_set_size(list, lw, list_h);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    app->list = list;

    int row_h = ds(64);
    if (row_h < 52) row_h = 52;
    int visible = list_h / row_h;
    if (visible < 1) visible = 1;
    int start = 0;
    if (app->filter_count > visible) {
        start = app->focus - visible / 2;
        if (start < 0) start = 0;
        if (start + visible > app->filter_count) start = app->filter_count - visible;
    }
    if (app->filter_count == 0) {
        const char *empty = app->refreshing ? "正在读取软件源..." :
                            (app->refresh_error[0] ? app->refresh_error : "没有找到应用");
        label(app->root, empty, FONT_MD, PAL.text3,
              lx + ds(12), list_y + ds(20), lw - ds(24), ds(24));
    } else {
        for (int i = start; i < start + visible && i < app->filter_count; i++)
            build_row(app, list, i, (i - start) * row_h, lw, row_h);
    }

    label_center(app->root,
                 "左右 切换   A 打开   X 搜索   Y 刷新   B 退出   Start 全部更新",
                 FONT_XS, PAL.text3, 0, h - ds(22), w, ds(16));
    if (app->show_detail) build_detail(app, w, h);
    if (app->task_running) build_task_overlay(app, w, h);

    app->dirty = false;
}

void store_ui_input(StoreApp *app, InputAction action, const char *text)
{
    if (!app) return;
    if (app->task_running) {
        if (action == IN_CANCEL || action == IN_B || action == IN_START)
            app->task_cancel = true;
        return;
    }
    if (app->search_edit) {
        if (action == IN_TEXT && text) {
            size_t cur = strlen(app->search);
            snprintf(app->search + cur, sizeof(app->search) - cur, "%s", text);
            app->dirty = true;
        } else if (action == IN_BACKSPACE) {
            size_t n = strlen(app->search);
            if (n > 0) app->search[n - 1] = '\0';
            app->dirty = true;
        } else if (action == IN_CONFIRM) {
            app->search_edit = false;
            rebuild_filter(app);
            app->focus = 0;
            app->dirty = true;
        } else if (action == IN_CANCEL || action == IN_B) {
            app->search_edit = false;
            app->search[0] = '\0';
            rebuild_filter(app);
            app->dirty = true;
        }
        return;
    }
    if (action == IN_X) {
        app->search_edit = true;
        app->dirty = true;
        return;
    }
    if (action == IN_Y) {
        store_ui_refresh(app);
        return;
    }
    if (action == IN_START) {
        store_ui_start_task(app, 2, 0);
        return;
    }
    if (app->show_detail) {
        if (action == IN_CANCEL || action == IN_B) {
            app->show_detail = false;
            app->dirty = true;
        } else if (action == IN_CONFIRM || action == IN_A) {
            int kind = 0;
            if (app->detail_index >= 0 && app->detail_index < app->filter_count) {
                StorePackage *p = &app->all.items[app->filter[app->detail_index]];
                if (p->update_available) kind = 0;
                else if (p->installed) kind = 1;
                else kind = 0;
            }
            store_ui_start_task(app, kind, app->detail_index);
        }
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        app->running = false;
        return;
    }
    if (action == IN_LEFT || action == IN_RIGHT) {
        app->on_sidebar = !app->on_sidebar;
        app->dirty = true;
        return;
    }
    if (action == IN_UP || action == IN_DOWN || action == IN_SCROLL_UP ||
        action == IN_SCROLL_DOWN) {
        int dir = (action == IN_UP || action == IN_SCROLL_UP) ? -1 : 1;
        if (app->on_sidebar) {
            app->side_focus = (app->side_focus + dir + STORE_CAT_COUNT) % STORE_CAT_COUNT;
        } else if (app->filter_count > 0) {
            int f = app->focus + dir;
            if (f < 0) f = 0;
            if (f >= app->filter_count) f = app->filter_count - 1;
            app->focus = f;
        }
        app->dirty = true;
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        if (app->on_sidebar) {
            app->category = app->side_focus;
            app->focus = 0;
            rebuild_filter(app);
            app->on_sidebar = false;
        } else if (app->filter_count > 0) {
            app->detail_index = app->focus;
            app->show_detail = true;
        }
        app->dirty = true;
    }
    (void)text;
}

void store_ui_tick(StoreApp *app)
{
    if (app->refresh_done) {
        pthread_mutex_lock(&app->lock);
        store_list_free(&app->all);
        app->all = app->refresh_result;
        memset(&app->refresh_result, 0, sizeof(app->refresh_result));
        app->refresh_done = false;
        app->refreshing = false;
        pthread_mutex_unlock(&app->lock);
        rebuild_filter(app);
        app->dirty = true;
    }
    if (app->task_done) {
        pthread_mutex_lock(&app->lock);
        app->task_done = false;
        bool ok = app->task_ok;
        app->task_running = false;
        pthread_mutex_unlock(&app->lock);
        if (app->task_ids) {
            for (int i = 0; i < app->task_id_count; i++) free(app->task_ids[i]);
            free(app->task_ids);
            app->task_ids = NULL;
            app->task_id_count = 0;
        }
        if (ok) {
            snprintf(app->notice, sizeof(app->notice), "操作完成");
            store_ui_refresh(app);
        } else {
            snprintf(app->notice, sizeof(app->notice), "%s",
                     app->task_error[0] ? app->task_error : "操作失败");
            app->dirty = true;
        }
    }
    if (app->task_running) app->dirty = true;
    if (app->dirty) rebuild_ui(app);
}

void store_ui_free(StoreApp *app)
{
    if (!app->running) app->running = false;
    store_list_free(&app->all);
    free(app->filter);
    if (app->task_ids) {
        for (int i = 0; i < app->task_id_count; i++) free(app->task_ids[i]);
        free(app->task_ids);
    }
    if (app->refreshing) {
        app->refresh_cancel = true;
        pthread_join(app->refresh_thread, NULL);
    }
    if (app->task_running) {
        app->task_cancel = true;
        pthread_join(app->task_thread, NULL);
    }
    pthread_mutex_destroy(&app->lock);
    config_free(&app->config);
}
