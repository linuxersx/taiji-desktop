// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "i18n.h"
#include "desktop.h"
#include "mirror.h"
#include "backend_flatpak.h"
#include "flatpak_presets.h"
#include "external_osk.h"
#include "store_cache.h"
#include "theme.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static pthread_mutex_t g_store_lock = PTHREAD_MUTEX_INITIALIZER;

#define STORE_ICON_QUEUE_MAX 128
static pthread_mutex_t g_iconq_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_icon_queue[STORE_ICON_QUEUE_MAX][PATH_MAX];
static int g_icon_queue_count;
static char g_icon_failed[64][PATH_MAX];
static int g_icon_failed_count;
static bool g_icon_conv_running;
static volatile bool g_icon_conv_done;
static lv_obj_t *g_store_viewport;
static lv_obj_t *g_store_rows_layer;
static App *g_store_app;
static int g_store_first;
static int g_store_cols;
static int g_store_total_cards;
static int g_store_row_step;
static int g_store_card_w;
static int g_store_card_h;
static int g_store_gap;
static int g_store_win_first;
static int g_store_win_last;
static int g_store_visible_rows;
static int g_store_hero_block;
static int g_store_scroll_y;
static bool g_store_programmatic_scroll;
static void resolve_icon(App *app, StorePackage *p);
static void rebuild_filter(App *app);
static void store_refresh_start(App *app);
static void store_task_start(App *app, int kind, int pkg_index);
static void store_update_badge(App *app);

static const char *category_names[] = {
    "探索", "游戏", "工具", "开发", "影音", "已安装", "可更新", "全部软件包"
};
#define STORE_CATS ((int)(sizeof(category_names) / sizeof(category_names[0])))

/* Sidebar display order: main views, then installed/updates, then settings. */
static const int side_views[] = {0, 1, 2, 3, 4, 7, 5, 6};
#define STORE_SIDE_VIEWS ((int)(sizeof(side_views) / sizeof(side_views[0])))
#define STORE_SIDE_SETTINGS STORE_SIDE_VIEWS
#define STORE_SIDE_COUNT (STORE_SIDE_VIEWS + 1)

static const char *store_active_source(App *app)
{
    return app->config.store_pkg_type == 1 ? "flatpak" : "pacman";
}

static const char *store_type_label(App *app)
{
    return app->config.store_pkg_type == 1 ? "Flatpak" : "Pacman";
}

static int store_source_count(App *app)
{
    return app->config.store_pkg_type == 1 ? store_flatpak_mirror_count()
                                           : store_mirror_count();
}

static const char *store_source_label(App *app, int index)
{
    return app->config.store_pkg_type == 1 ? store_flatpak_mirror_label(index)
                                           : store_mirror_label(index);
}

static const char *store_source_url(App *app, int index)
{
    return app->config.store_pkg_type == 1 ? store_flatpak_mirror_url(index)
                                           : store_mirror_url(index);
}

static int store_source_current(App *app)
{
    int n = store_source_count(app);
    int cur = app->config.store_pkg_type == 1 ? app->config.store_flatpak_source
                                              : app->config.store_pacman_source;
    if (cur < 0 || cur >= n)
        cur = app->config.store_pkg_type == 1 ? TAIJI_FLATPAK_DEFAULT_MIRROR : 1;
    return cur;
}

static void store_source_save(App *app, int index)
{
    if (app->config.store_pkg_type == 1)
        app->config.store_flatpak_source = index;
    else
        app->config.store_pacman_source = index;
    config_save(&app->config);
}

static bool pkg_matches_active(App *app, const StorePackage *p)
{
    return strcmp(p->source, store_active_source(app)) == 0;
}
static bool store_switch_type(App *app, int type)
{
    if (type < 0 || type > 1) return false;
    const char *src = type == 1 ? "flatpak" : "pacman";
    StoreBackend *b = store_backend_find(src);
    char err[128] = "";
    if (!b || !b->probe(err, sizeof(err))) {
        ui_toast(app, err[0] ? err : "该软件源不可用");
        return false;
    }
    if (type == app->config.store_pkg_type) return true;
    app->config.store_pkg_type = type;
    config_save(&app->config);
    app->store_category = 0;
    app->store_focus = 0;
    app->store_detail = false;
    store_update_badge(app);
    rebuild_filter(app);
    /* 切换类型后自动应用该类型保存的软件源，并在成功后刷新列表。 */
    store_task_start(app, type == 1 ? 4 : 3, store_source_current(app));
    return true;
}


static void store_count_updates_for_source(App *app)
{
    int n = 0;
    for (int i = 0; i < app->store_all.count; i++)
        if (app->store_all.items[i].update_available &&
            pkg_matches_active(app, &app->store_all.items[i]))
            n++;
    app->store_update_count = n;
    ui_update_store_badge(app);
}

static int cmp_pkg(const void *a, const void *b)
{
    const StorePackage *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->source, y->source);
}

static bool category_matches(const StorePackage *p, int view)
{
    switch (view) {
        case 1: return strcmp(p->category, "games") == 0;
        case 2: return strcmp(p->category, "utility") == 0 ||
                       strcmp(p->category, "system") == 0 ||
                       strcmp(p->category, "desktop") == 0;
        case 3: return strcmp(p->category, "development") == 0;
        case 4: return strcmp(p->category, "media") == 0 ||
                       strcmp(p->category, "network") == 0;
        case 5: return p->installed;
        case 6: return p->update_available;
        case 7: return true;
        default: return true;
    }
}

static bool search_matches(const StorePackage *p, const char *q)
{
    if (!q || !*q) return true;
    return strcasestr(p->name, q) || strcasestr(p->id, q) ||
           strcasestr(p->description, q);
}

static void store_update_badge(App *app)
{
    store_count_updates_for_source(app);
}

static void rebuild_filter(App *app)
{
    free(app->store_filter);
    app->store_filter = NULL;
    app->store_filter_count = 0;
    if (app->store_all.count <= 0) return;
    app->store_filter = xmalloc(sizeof(int) * (size_t)app->store_all.count);
    if (app->store_category == 0 && !app->store_search[0]) {
        for (int f = 0; f < app->store_featured_count; f++) {
            for (int i = 0; i < app->store_all.count; i++) {
                if (!pkg_matches_active(app, &app->store_all.items[i])) continue;
                if (strcmp(app->store_all.items[i].id, app->store_featured[f]) != 0)
                    continue;
                bool dup = false;
                for (int k = 0; k < app->store_filter_count; k++)
                    if (app->store_filter[k] == i) dup = true;
                if (!dup) app->store_filter[app->store_filter_count++] = i;
                break;
            }
        }
        if (app->store_filter_count == 0) {
            for (int i = 0; i < app->store_all.count && app->store_filter_count < 24; i++) {
                StorePackage *p = &app->store_all.items[i];
                if (!pkg_matches_active(app, p)) continue;
                if (p->description[0] && p->category[0] &&
                    strcmp(p->category, "system") != 0)
                    app->store_filter[app->store_filter_count++] = i;
            }
        }
    } else {
        for (int i = 0; i < app->store_all.count; i++) {
            StorePackage *p = &app->store_all.items[i];
            if (!pkg_matches_active(app, p)) continue;
            if (category_matches(p, app->store_category) &&
                search_matches(p, app->store_search))
                app->store_filter[app->store_filter_count++] = i;
        }
    }
    if (app->store_focus >= app->store_filter_count)
        app->store_focus = app->store_filter_count > 0 ? app->store_filter_count - 1 : 0;
    if (app->store_focus < 0) app->store_focus = 0;
    if (app->store_detail && app->store_detail_index >= app->store_filter_count)
        app->store_detail = false;
}

static void *store_refresh_worker(void *arg)
{
    App *app = arg;
    StorePackageList result = {0};
    bool ok = true;
    int usable = 0;
    char error[256] = "";
    const char *active = store_active_source(app);
    StoreBackend *b = store_backend_find(active);
    char err[256] = "";
    if (!b || !b->probe(err, sizeof(err))) {
        if (err[0]) snprintf(error, sizeof(error), "%s", err);
    } else if (!app->store_refresh_cancel) {
        usable++;
        StorePackageList one = {0};
        if (b->refresh(b, &one, NULL, err, sizeof(err))) {
            store_cache_save(b->source, &one);
            for (int k = 0; k < one.count; k++) store_list_push(&result, &one.items[k]);
        } else {
            ok = false;
            if (err[0]) snprintf(error, sizeof(error), "%s", err);
        }
        store_list_free(&one);
    }
    if (usable == 0 && !error[0])
        snprintf(error, sizeof(error), "没有可用的软件源（检查 %s / helper）", active);
    if (result.items)
        qsort(result.items, (size_t)result.count, sizeof(StorePackage), cmp_pkg);
    pthread_mutex_lock(&g_store_lock);
    store_list_free(&app->store_refresh_result);
    app->store_refresh_result = result;
    snprintf(app->store_error, sizeof(app->store_error), "%s", error);
    app->store_refresh_ok = ok;
    app->store_refresh_done = true;
    pthread_mutex_unlock(&g_store_lock);
    return NULL;
}

static void store_refresh_start(App *app)
{
    if (app->store_refreshing) return;
    app->store_refreshing = true;
    app->store_refresh_done = false;
    app->store_refresh_cancel = false;
    app->store_refresh_ok = false;
    app->store_error[0] = '\0';
    if (pthread_create(&app->store_thread, NULL, store_refresh_worker, app) != 0) {
        app->store_refreshing = false;
        return;
    }
}

static void load_featured(App *app)
{
    if (app->store_featured_count > 0) return;
    static const char *defaults[] = {
        "retroarch", "ppsspp", "mgba-qt", "ares-emu", "snes9x", "desmume",
        "mednafen", "scummvm", "dosbox", "firefox", "chromium", "steam",
        "vlc", "mpv", "gimp", "krita", "code", "dolphin-emu", "mangohud"
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        snprintf(app->store_featured[app->store_featured_count++],
                 sizeof(app->store_featured[0]), "%s", defaults[i]);
    const char *env = getenv("TAIJI_STORE_FEATURED");
    const char *path = env && *env ? env : "/usr/share/taiji/store/featured.tsv";
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return;
    app->store_featured_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line && app->store_featured_count < 32;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (!*line || *line == '#') continue;
        char *sep = strchr(line, '|');
        if (sep) *sep = '\0';
        line = str_trim(line);
        if (!*line) continue;
        snprintf(app->store_featured[app->store_featured_count++],
                 sizeof(app->store_featured[0]), "%s", line);
    }
    free(data);
    if (app->store_featured_count == 0) {
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
            snprintf(app->store_featured[app->store_featured_count++],
                     sizeof(app->store_featured[0]), "%s", defaults[i]);
    }
}

static void *icon_worker(void *arg)
{
    App *app = arg;
    IconIndex idx;
    AppStreamIndex as;
    icon_index_init(&idx);
    appstream_index_init(&as);
    bool app_ok = appstream_index_build(&as);
    bool ok = icon_index_build(&idx);
    pthread_mutex_lock(&g_store_lock);
    icon_index_free(&app->store_icons);
    appstream_index_free(&app->store_appstream);
    app->store_icons = idx;
    app->store_appstream = as;
    app->store_icons_ready = ok;
    app->store_appstream_ready = app_ok;
    app->store_icons_done = true;
    pthread_mutex_unlock(&g_store_lock);
    return NULL;
}

static void store_icons_start(App *app)
{
    if (app->store_icons_ready || app->store_icons_building) return;
    app->store_icons_building = true;
    app->store_icons_done = false;
    if (pthread_create(&app->store_icons_thread, NULL, icon_worker, app) != 0)
        app->store_icons_building = false;
}

void app_store_enter(App *app)
{
    app->store_view_row = 0;
    load_featured(app);
    store_icons_start(app);
    if (!app->store_loaded) {
        for (int i = 0; i < store_backend_count(); i++) {
            StoreBackend *b = store_backend_at(i);
            char err[256] = "";
            if (!b || !b->probe(err, sizeof(err))) continue;
            StorePackageList one = {0};
            if (store_cache_load(b->source, &one)) {
                for (int k = 0; k < one.count; k++)
                    store_list_push(&app->store_all, &one.items[k]);
            }
            store_list_free(&one);
        }
        if (app->store_all.items)
            qsort(app->store_all.items, (size_t)app->store_all.count,
                  sizeof(StorePackage), cmp_pkg);
        app->store_loaded = true;
        rebuild_filter(app);
    }
    store_update_badge(app);
    store_refresh_start(app);
}

static void task_progress(void *ud, const char *stage, int pct, const char *line)
{
    App *app = ud;
    pthread_mutex_lock(&g_store_lock);
    if (stage) snprintf(app->store_task_stage, sizeof(app->store_task_stage), "%s", stage);
    app->store_task_percent = pct;
    if (line) {
        snprintf(app->store_task_line, sizeof(app->store_task_line), "%s", line);
        if (app->store_task_output_count < 200)
            snprintf(app->store_task_output[app->store_task_output_count++],
                     sizeof(app->store_task_output[0]), "%s", line);
    }
    pthread_mutex_unlock(&g_store_lock);
}

static void *store_task_worker(void *arg)
{
    App *app = arg;
    StoreProgress progress = {task_progress, app, &app->store_task_cancel};
    char error[256] = {0};
    bool ok = false;
    StoreBackend *b = app->store_task_backend;
    if (app->store_task_kind == 3) {
        ok = store_mirror_apply(app->store_task_mirror, &progress,
                                error, sizeof(error));
    } else if (app->store_task_kind == 4) {
        ok = store_flatpak_mirror_apply(app->store_task_mirror, &progress,
                                        error, sizeof(error));
    } else if (b) {
        if (app->store_task_kind == 0)
            ok = b->install(b, (const char **)app->store_task_ids,
                            app->store_task_id_count, &progress, error, sizeof(error));
        else if (app->store_task_kind == 1)
            ok = b->remove(b, (const char **)app->store_task_ids,
                           app->store_task_id_count, &progress, error, sizeof(error));
        else
            ok = b->update(b, NULL, 0, &progress, error, sizeof(error));
    }
    pthread_mutex_lock(&g_store_lock);
    app->store_task_ok = ok;
    if (!ok && error[0]) snprintf(app->store_task_error,
                                  sizeof(app->store_task_error), "%s", error);
    app->store_task_done = true;
    pthread_mutex_unlock(&g_store_lock);
    return NULL;
}

static void store_task_start(App *app, int kind, int pkg_index)
{
    if (app->store_task_running) return;
    StoreBackend *b = NULL;
    if (kind == 3) {
        if (pkg_index < 0 || pkg_index >= store_mirror_count()) return;
    } else if (kind == 4) {
        if (pkg_index < 0 || pkg_index >= store_flatpak_mirror_count()) return;
    } else if (kind == 0 || kind == 1) {
        if (pkg_index < 0 || pkg_index >= app->store_filter_count) return;
        StorePackage *p = &app->store_all.items[app->store_filter[pkg_index]];
        b = store_backend_find(p->source);
        if (!b) return;
        if (kind == 1 && !p->removable) {
            ui_toast(app, "该包受保护，不能卸载");
            return;
        }
        app->store_task_ids = xmalloc(sizeof(char *));
        app->store_task_ids[0] = xstrdup(p->id);
        app->store_task_id_count = 1;
    } else {
        b = store_backend_find(store_active_source(app));
        if (!b) return;
        app->store_task_ids = NULL;
        app->store_task_id_count = 0;
    }
    app->store_task_kind = kind;
    app->store_task_mirror = (kind == 3 || kind == 4) ? pkg_index : -1;
    app->store_task_backend = b;
    app->store_task_running = true;
    app->store_task_done = false;
    app->store_task_ok = false;
    app->store_task_cancel = false;
    app->store_task_percent = -1;
    app->store_task_name[0] = '\0';
    if (kind == 3) {
        snprintf(app->store_task_name, sizeof(app->store_task_name), "软件源");
    } else if (kind == 4) {
        snprintf(app->store_task_name, sizeof(app->store_task_name), "Flatpak 软件源");
    } else if ((kind == 0 || kind == 1) && pkg_index >= 0 &&
               pkg_index < app->store_filter_count) {
        StorePackage *p = &app->store_all.items[app->store_filter[pkg_index]];
        snprintf(app->store_task_name, sizeof(app->store_task_name), "%s", p->name);
    } else {
        snprintf(app->store_task_name, sizeof(app->store_task_name), "全部更新");
    }
    snprintf(app->store_task_stage, sizeof(app->store_task_stage), "准备中...");
    app->store_task_line[0] = '\0';
    app->store_task_error[0] = '\0';
    app->store_task_output_count = 0;
    app->store_detail = false;
    if (pthread_create(&app->store_task_thread, NULL, store_task_worker, app) != 0)
        app->store_task_running = false;
}

void app_store_poll(App *app)
{
    if (g_icon_conv_done) {
        g_icon_conv_done = false;
        if (app->screen == SCREEN_STORE) app_rebuild(app);
    }
    if (app->store_icons_done) {
        pthread_mutex_lock(&g_store_lock);
        app->store_icons_done = false;
        app->store_icons_building = false;
        pthread_mutex_unlock(&g_store_lock);
        if (app->screen == SCREEN_STORE) app_rebuild(app);
    }
    if (app->store_refresh_done) {
        pthread_mutex_lock(&g_store_lock);
        store_list_free(&app->store_all);
        app->store_all = app->store_refresh_result;
        memset(&app->store_refresh_result, 0, sizeof(app->store_refresh_result));
        app->store_refresh_done = false;
        app->store_refreshing = false;
        pthread_mutex_unlock(&g_store_lock);
        rebuild_filter(app);
        store_update_badge(app);
        if (app->screen == SCREEN_STORE) app_rebuild(app);
    }
    if (app->store_task_done) {
        pthread_mutex_lock(&g_store_lock);
        app->store_task_done = false;
        bool ok = app->store_task_ok;
        char err[256];
        snprintf(err, sizeof(err), "%s", app->store_task_error);
        app->store_task_running = false;
        pthread_mutex_unlock(&g_store_lock);
        if (app->store_task_ids) {
            for (int i = 0; i < app->store_task_id_count; i++) free(app->store_task_ids[i]);
            free(app->store_task_ids);
            app->store_task_ids = NULL;
            app->store_task_id_count = 0;
        }
        if (app->screen == SCREEN_STORE) app_rebuild(app);
        if (ok) store_refresh_start(app);
        else ui_toast(app, err[0] ? err : "操作失败");
    }
}

void app_store_free(App *app)
{
    if (app->store_refreshing) {
        app->store_refresh_cancel = true;
        pthread_join(app->store_thread, NULL);
    }
    if (app->store_task_running) {
        app->store_task_cancel = true;
        pthread_join(app->store_task_thread, NULL);
    }
    if (app->store_icons_building) {
        pthread_join(app->store_icons_thread, NULL);
        app->store_icons_building = false;
    }
    icon_index_free(&app->store_icons);
    appstream_index_free(&app->store_appstream);
    store_list_free(&app->store_all);
    store_list_free(&app->store_refresh_result);
    free(app->store_filter);
    app->store_filter = NULL;
    if (app->store_task_ids) {
        for (int i = 0; i < app->store_task_id_count; i++) free(app->store_task_ids[i]);
        free(app->store_task_ids);
        app->store_task_ids = NULL;
    }
}

typedef struct {
    App *app;
    int side_idx;
} StoreSideCtx;

static void side_row_clicked(lv_event_t *e)
{
    StoreSideCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    audio_play(SND_ENTER);
    app->store_side_focus = ctx->side_idx;
    if (ctx->side_idx == STORE_SIDE_SETTINGS) {
        app->store_settings = true;
        app->store_settings_focus = 1 + store_source_current(app);
        app->store_dropdown_open = false;
        app->store_on_sidebar = false;
    } else {
        app->store_settings = false;
        app->store_category = side_views[ctx->side_idx];
        app->store_focus = 0;
        app->store_view_row = 0;
        app->store_detail = false;
        rebuild_filter(app);
        app->store_on_sidebar = false;
    }
    app_rebuild(app);
}

static void side_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void side_row(App *app, lv_obj_t *panel, int x, int y, int w, int h,
                     int side_idx, int view)
{
    bool settings = view < 0;
    bool active = settings ? app->store_settings :
                  (!app->store_settings && app->store_category == view);
    bool focused = app->store_on_sidebar && app->store_side_focus == side_idx;
    lv_obj_t *row = ui_rect(panel, x, y, w, h, 9,
                            active ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(row, active ? 42 : 0, 0);
    lv_font_t *font = h < 22 ? FONT_XS :
                      (app->layout.w < 600 ? FONT_SM : FONT_MD);
    int line = ui_unscale_y(lv_font_get_line_height(font));
    ui_label(row, settings ? LV_SYMBOL_SETTINGS " 设置" : category_names[view],
             font, settings ? PAL.accent2 : (active ? PAL.accent2 : PAL.text2),
             14, (h - line) / 2, w - 24, line + 2);
    if (!settings && view == 6 && app->store_update_count > 0) {
        int dot = ui_clamp(h * 18 / 100, 7, 12);
        ui_rect(row, w - dot - 12, (h - dot) / 2, dot, dot, dot / 2,
                PAL.danger);
    }
    if (focused)
        ui_set_focus_ring(row, true, PAL.accent);
    /* 触摸选择后 store_on_sidebar 会关闭；仍要把当前项保持在可视区域，
     * 否则左栏重建时会自动跳回顶部。 */
    if (app->store_side_focus == side_idx)
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    StoreSideCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    ctx->side_idx = side_idx;
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, side_row_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, side_ctx_free, LV_EVENT_DELETE, ctx);
}

static int side_index_for_category(int category)
{
    for (int i = 0; i < STORE_SIDE_VIEWS; i++)
        if (side_views[i] == category) return i;
    return 0;
}

/* Preview the highlighted sidebar view immediately while browsing with the pad. */
static void side_preview(App *app, int side_idx)
{
    if (side_idx < 0 || side_idx >= STORE_SIDE_VIEWS) return;
    int category = side_views[side_idx];
    if (app->store_category == category) return;
    app->store_category = category;
    app->store_focus = 0;
    app->store_view_row = 0;
    app->store_detail = false;
    rebuild_filter(app);
}

typedef struct {
    App *app;
    int index;
} StoreMirrorCtx;

static void mirror_row_clicked(lv_event_t *e)
{
    StoreMirrorCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    app->store_settings_focus = ctx->index + 1;
    audio_play(SND_ENTER);
    store_source_save(app, ctx->index);
    store_task_start(app, app->config.store_pkg_type == 1 ? 4 : 3,
                     ctx->index);
    app_rebuild(app);
}

static void type_row_clicked(lv_event_t *e)
{
    StoreMirrorCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    audio_play(SND_ENTER);
    app->store_dropdown_open = false;
    store_switch_type(app, ctx->index);
    app->store_settings_focus = 1 + store_source_current(app);
    app_rebuild(app);
}

static void dropdown_open_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    audio_play(SND_ENTER);
    app->store_settings_focus = 0;
    app->store_dropdown_open = true;
    app->store_dropdown_focus = app->config.store_pkg_type;
    app_rebuild(app);
}

static void mirror_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void build_store_row(App *app, lv_obj_t *list, int idx, int y, int w, int h)
{
    StorePackage *p = &app->store_all.items[app->store_filter[idx]];
    resolve_icon(app, p);
    bool sel = !app->store_on_sidebar && app->store_focus == idx;
    lv_obj_t *row = ui_rect(list, 0, y, w, h - 4, 10,
                            sel ? PAL.accent : PAL.card);
    lv_obj_set_style_bg_opa(row, sel ? 34 : 70, 0);
    lv_obj_set_style_border_width(row, sel ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PAL.accent), 0);
    lv_obj_set_style_border_opa(row, 170, 0);
    int icon = h - 18;
    if (icon > 40) icon = 40;
    int iy = (h - icon) / 2;
    if (p->icon[0] && access(p->icon, R_OK) == 0) {
        lv_obj_t *img = lv_image_create(row);
        lv_obj_remove_style_all(img);
        lv_obj_set_pos(img, 10, iy);
        lv_obj_set_size(img, icon, icon);
        char *src = xmalloc(strlen(p->icon) + 3);
        sprintf(src, "A:%s", p->icon);
        lv_image_set_src(img, src);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        free(src);
    } else {
        lv_obj_t *box = ui_rect(row, 10, iy, icon, icon, 8, PAL.elevated);
        ui_label_center(box, LV_SYMBOL_FILE, FONT_MD, PAL.text3,
                        0, (icon - 16) / 2, icon, 18);
    }
    int tx = 10 + icon + 10;
    int rw = w / 3;
    lv_obj_t *name = ui_label(row, p->name, FONT_SM, PAL.text, tx, 7,
                              w - tx - rw - 8, 20);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t *desc = ui_label(row, p->description, FONT_XS, PAL.text3,
                              tx, 29, w - tx - rw - 8, 16);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
    const char *st;
    uint32_t col;
    if (p->update_available && !p->removable) { st = "受保护"; col = PAL.warning; }
    else if (p->update_available) { st = "可更新"; col = PAL.warning; }
    else if (p->installed) { st = "已安装"; col = PAL.success; }
    else { st = "未安装"; col = PAL.text3; }
    ui_label(row, st, FONT_XS, col, w - rw - 6, 7, rw, 16);
    char meta[128];
    snprintf(meta, sizeof(meta), "%s %s", p->source,
             p->installed_version[0] ? p->installed_version : "");
    ui_label(row, meta, FONT_XS, PAL.text3, w - rw - 6, 27, rw, 16);
}

static bool icon_file(const char *name, char *out, size_t n)
{
    static const char *themes[] = {"Papirus", "Papirus-Dark", "Tela", "Tela-dark",
                                   "Qogir", "Qogir-dark", "hicolor", "Adwaita",
                                   "breeze", "breeze-dark", "Yaru", "elementary"};
    static const char *sizes[] = {"scalable", "512x512", "256x256", "128x128",
                                  "96x96", "64x64", "48x48", "32x32"};
    static const char *exts[] = {".svg", ".png", ".xpm"};
    char p[PATH_MAX];
    for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
                snprintf(p, sizeof(p), "/usr/share/icons/%s/%s/apps/%s%s",
                         themes[t], sizes[s], name, exts[e]);
                if (access(p, R_OK) == 0) { snprintf(out, n, "%s", p); return true; }
            }
        }
    }
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        snprintf(p, sizeof(p), "/usr/share/pixmaps/%s%s", name, exts[e]);
        if (access(p, R_OK) == 0) { snprintf(out, n, "%s", p); return true; }
    }
    return false;
}

static bool icon_path_needs_raster(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot && (strcasecmp(dot, ".svg") == 0 || strcasecmp(dot, ".xpm") == 0);
}

static void *store_icon_worker(void *arg)
{
    (void)arg;
    for (;;) {
        char path[PATH_MAX];
        pthread_mutex_lock(&g_iconq_lock);
        if (g_icon_queue_count == 0) {
            g_icon_conv_running = false;
            pthread_mutex_unlock(&g_iconq_lock);
            break;
        }
        snprintf(path, sizeof(path), "%s", g_icon_queue[0]);
        pthread_mutex_unlock(&g_iconq_lock);

        char png[PATH_MAX];
        bool ok = desktop_icon_to_png(path, png, sizeof(png));

        pthread_mutex_lock(&g_iconq_lock);
        if (g_icon_queue_count > 0) {
            memmove(g_icon_queue[0], g_icon_queue[1],
                    (size_t)(g_icon_queue_count - 1) * sizeof(g_icon_queue[0]));
            g_icon_queue_count--;
        }
        if (!ok && g_icon_failed_count < 64)
            snprintf(g_icon_failed[g_icon_failed_count++], PATH_MAX, "%s", path);
        g_icon_conv_done = true;
        pthread_mutex_unlock(&g_iconq_lock);
    }
    return NULL;
}

static void store_queue_icon(const char *path)
{
    if (!path || !*path) return;
    pthread_mutex_lock(&g_iconq_lock);
    for (int i = 0; i < g_icon_queue_count; i++)
        if (strcmp(g_icon_queue[i], path) == 0) {
            pthread_mutex_unlock(&g_iconq_lock);
            return;
        }
    for (int i = 0; i < g_icon_failed_count; i++)
        if (strcmp(g_icon_failed[i], path) == 0) {
            pthread_mutex_unlock(&g_iconq_lock);
            return;
        }
    if (g_icon_queue_count >= STORE_ICON_QUEUE_MAX) {
        pthread_mutex_unlock(&g_iconq_lock);
        return;
    }
    snprintf(g_icon_queue[g_icon_queue_count++], PATH_MAX, "%s", path);
    if (!g_icon_conv_running) {
        pthread_t th;
        g_icon_conv_running = true;
        if (pthread_create(&th, NULL, store_icon_worker, NULL) != 0)
            g_icon_conv_running = false;
        else
            pthread_detach(th);
    }
    pthread_mutex_unlock(&g_iconq_lock);
}

static bool finish_icon(App *app, StorePackage *p, const char *path)
{
    if (!path || !*path) return false;
    char png[PATH_MAX];
    if (desktop_icon_cached_png(path, png, sizeof(png))) {
        snprintf(p->icon, sizeof(p->icon), "%s", png);
        return true;
    }
    if (app && icon_path_needs_raster(path)) {
        store_queue_icon(path);
        return false;
    }
    if (!desktop_icon_to_png(path, png, sizeof(png))) return false;
    snprintf(p->icon, sizeof(p->icon), "%s", png);
    return true;
}

static void resolve_icon(App *app, StorePackage *p)
{
    if (!p) return;
    if (p->icon[0]) {
        if (finish_icon(app, p, p->icon)) return;
        p->icon[0] = '\0';
    }

    char found[PATH_MAX];
    if (app && app->store_appstream_ready) {
        const AppStreamEntry *as = appstream_index_find(&app->store_appstream, p->id);
        if (as) {
            if (as->summary[0] && !p->description[0])
                snprintf(p->description, sizeof(p->description), "%s", as->summary);
            if (app->store_icons_ready && as->icon[0] &&
                icon_index_lookup_name(&app->store_icons, as->icon,
                                       found, sizeof(found)) &&
                finish_icon(app, p, found))
                return;
        }
    }
    if (app && app->store_icons_ready &&
        icon_index_resolve(&app->store_icons, p->id, found, sizeof(found)) &&
        finish_icon(app, p, found))
        return;

    static const struct { const char *pkg; const char *icon; } aliases[] = {
        {"firefox", "firefox"}, {"chromium", "chromium"}, {"chrome", "google-chrome"},
        {"code", "vscode"}, {"steam", "steam"}, {"retroarch", "retroarch"},
        {"dolphin-emu", "dolphin-emu"}, {"ppsspp", "ppsspp-qt"}, {"vlc", "vlc"},
        {"mpv", "mpv"}, {"gimp", "gimp"}, {"krita", "krita"}, {"htop", "utilities-terminal"},
        {"7zip", "7zip"}, {"youtube-music", "youtube-music"}
    };
    const char *cand = p->id;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (strcmp(p->id, aliases[i].pkg) == 0) cand = aliases[i].icon;
    if (icon_file(cand, found, sizeof(found)) && finish_icon(app, p, found)) return;

    char base[128];
    snprintf(base, sizeof(base), "%s", p->id);
    char *suffix = strstr(base, "-bin");
    if (suffix) *suffix = '\0';
    suffix = strstr(base, "-git");
    if (suffix) *suffix = '\0';
    if (icon_file(base, found, sizeof(found)) && finish_icon(app, p, found)) return;

    char desktop[PATH_MAX];
    snprintf(desktop, sizeof(desktop), "/usr/share/applications/%s.desktop", p->id);
    size_t len = 0;
    char *data = read_file(desktop, &len);
    if (data) {
        char *icon = strstr(data, "\nIcon=");
        if (!icon) icon = strstr(data, "Icon=");
        if (icon) {
            icon += (icon[0] == '\n') ? 6 : 5;
            char name[128];
            size_t k = 0;
            while (icon[k] && icon[k] != '\n' && icon[k] != '\r' && k < sizeof(name) - 1) {
                name[k] = icon[k];
                k++;
            }
            name[k] = '\0';
            if (name[0] == '/') {
                finish_icon(app, p, name);
            } else if (name[0] && icon_file(name, found, sizeof(found))) {
                finish_icon(app, p, found);
            }
        }
        free(data);
    }
}

static const char *category_symbol(const StorePackage *p, uint32_t *color)
{
    if (strcmp(p->category, "games") == 0) { *color = PAL.warning; return LV_SYMBOL_PLAY; }
    if (strcmp(p->category, "development") == 0) { *color = PAL.accent2; return LV_SYMBOL_EDIT; }
    if (strcmp(p->category, "media") == 0) { *color = PAL.accent2; return LV_SYMBOL_AUDIO; }
    if (strcmp(p->category, "network") == 0) { *color = PAL.accent; return LV_SYMBOL_WIFI; }
    if (strcmp(p->category, "system") == 0) { *color = PAL.text2; return LV_SYMBOL_SETTINGS; }
    if (strcmp(p->category, "desktop") == 0) { *color = PAL.accent2; return LV_SYMBOL_IMAGE; }
    *color = PAL.text3;
    return LV_SYMBOL_FILE;
}

typedef struct {
    App *app;
    int index;
} StoreCardCtx;

static void store_card_clicked(lv_event_t *e)
{
    StoreCardCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->app) return;
    App *app = ctx->app;
    if (app->store_scroll_pending) return;
    audio_play(SND_ENTER);
    app->store_on_sidebar = false;
    app->store_settings = false;
    app->store_focus = ctx->index;
    app->store_detail_index = ctx->index;
    app->store_detail = true;
    app_rebuild(app);
}

static void store_card_ctx_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void detail_action_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app || app->store_detail_index < 0 ||
        app->store_detail_index >= app->store_filter_count) return;
    StorePackage *p = &app->store_all.items[app->store_filter[app->store_detail_index]];
    if (!p->removable) {
        ui_toast(app, "该软件包受保护，不能通过商店修改");
        return;
    }
    int kind = (p->installed && !p->update_available) ? 1 : 0;
    audio_play(SND_ENTER);
    store_task_start(app, kind, app->store_detail_index);
    app_rebuild(app);
}

static void detail_back_clicked(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app) return;
    audio_play(SND_BACK);
    app->store_detail = false;
    app_rebuild(app);
}

static void build_store_card(App *app, lv_obj_t *parent, int idx,
                             int x, int y, int w, int h)
{
    StorePackage *p = &app->store_all.items[app->store_filter[idx]];
    resolve_icon(app, p);
    bool sel = !app->store_on_sidebar && app->store_focus == idx;
    lv_obj_t *card = ui_rect(parent, x, y, w, h, 12, PAL.card);
    lv_obj_set_style_bg_opa(card, sel ? 40 : 80, 0);
    lv_obj_set_style_border_width(card, sel ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(sel ? PAL.accent : PAL.border), 0);
    lv_obj_set_style_border_opa(card, sel ? 220 : 60, 0);
    if (sel) ui_set_focus_ring(card, true, ui_focus_accent(idx));
    StoreCardCtx *cctx = xmalloc(sizeof(*cctx));
    cctx->app = app;
    cctx->index = idx;
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, store_card_clicked, LV_EVENT_CLICKED, cctx);
    lv_obj_add_event_cb(card, store_card_ctx_free, LV_EVENT_DELETE, cctx);
    int icon = ui_clamp(h * 34 / 100, 26, 42);
    if (p->icon[0] && access(p->icon, R_OK) == 0) {
        lv_obj_t *img = lv_image_create(card);
        lv_obj_remove_style_all(img);
        lv_obj_set_pos(img, ui_scale_x(10), ui_scale_y(10));
        lv_obj_set_size(img, ui_scale_x(icon), ui_scale_y(icon));
        char *src = xmalloc(strlen(p->icon) + 3);
        sprintf(src, "A:%s", p->icon);
        lv_image_set_src(img, src);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        free(src);
    } else {
        lv_obj_t *box = ui_rect(card, 10, 10, icon, icon, 8, PAL.elevated);
        uint32_t sym_color = PAL.text3;
        const char *sym = category_symbol(p, &sym_color);
        ui_label_center(box, sym, FONT_MD, sym_color,
                        0, (icon - 16) / 2, icon, 18);
    }
    int tx = 10 + icon + 10;
    lv_obj_t *name = ui_label(card, p->name, FONT_SM, PAL.text, tx, 8,
                              w - tx - 10, 20);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t *desc = ui_label(card, p->description, FONT_XS, PAL.text3,
                              tx, 30, w - tx - 10, h - 48);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
    const char *st;
    uint32_t col;
    if (p->update_available && !p->removable) { st = "受保护"; col = PAL.warning; }
    else if (p->update_available) { st = "可更新"; col = PAL.warning; }
    else if (p->installed) { st = "已安装"; col = PAL.success; }
    else { st = "未安装"; col = PAL.text3; }
    int status_h = ui_text_height(FONT_XS);
    ui_label(card, st, FONT_XS, col, 10, h - status_h - 4, w - 20, status_h);
}

#define STORE_SCROLL_BUFFER 6

static void store_build_rows(int first_row, int last_row)
{
    if (!g_store_rows_layer || !g_store_app) return;
    lv_obj_clean(g_store_rows_layer);
    if (first_row < 0) first_row = 0;
    int rows = (g_store_total_cards + g_store_cols - 1) / g_store_cols;
    if (last_row >= rows) last_row = rows - 1;
    if (last_row < first_row) return;
    g_store_win_first = first_row;
    g_store_win_last = last_row;
    for (int r = first_row; r <= last_row; r++) {
        for (int c = 0; c < g_store_cols; c++) {
            int idx = g_store_first + r * g_store_cols + c;
            if (idx >= g_store_first + g_store_total_cards) break;
            build_store_card(g_store_app, g_store_rows_layer, idx,
                             c * (g_store_card_w + g_store_gap),
                             r * (g_store_card_h + g_store_gap),
                             g_store_card_w, g_store_card_h);
        }
    }
}

static void store_scroll_settled(lv_event_t *e)
{
    App *app = lv_event_get_user_data(e);
    if (!app || !g_store_viewport || !g_store_rows_layer ||
        g_store_row_step <= 0)
        return;
    if (app->store_settings || app->store_detail) return;
    int rows = (g_store_total_cards + g_store_cols - 1) / g_store_cols;
    if (rows <= 0) return;
    if (g_store_programmatic_scroll) return;
    int sy = lv_obj_get_scroll_y(g_store_viewport);
    if (sy == g_store_scroll_y) return;
    g_store_scroll_y = sy;
    int top = 0;
    int focus;
    bool at_bottom = lv_obj_get_scroll_bottom(g_store_viewport) <= 0;
    if (g_store_hero_block > 0 && sy < g_store_hero_block) {
        focus = 0;
        app->store_view_row = 0;
    } else if (at_bottom) {
        focus = app->store_filter_count - 1;
        int total_rows = (g_store_total_cards + g_store_cols - 1) / g_store_cols;
        top = total_rows - g_store_visible_rows;
        if (top < 0) top = 0;
        app->store_view_row = top;
    } else {
        int card_y = sy - g_store_hero_block;
        if (card_y < 0) card_y = 0;
        top = card_y / g_store_row_step;
        if (top < 0) top = 0;
        if (top >= rows) top = rows - 1;
        focus = g_store_first + top * g_store_cols;
        if (focus >= app->store_filter_count)
            focus = app->store_filter_count - 1;
        if (focus < g_store_first) focus = g_store_first;
        app->store_view_row = top;
    }
    app->store_focus = focus;
    int first = top - STORE_SCROLL_BUFFER;
    int last = top + g_store_visible_rows + STORE_SCROLL_BUFFER;
    if (first < 0) first = 0;
    if (last >= rows) last = rows - 1;
    store_build_rows(first, last);
}

void ui_build_store(App *app)
{
    g_store_viewport = NULL;
    g_store_rows_layer = NULL;
    g_store_hero_block = 0;
    g_store_scroll_y = 0;
    g_store_programmatic_scroll = false;
    g_store_app = app;
    g_store_win_first = 0;
    g_store_win_last = -1;
    ui_screen_background(app, NULL, PAL.accent);
    UiLayout *L = &app->layout;
    if (!app->store_loaded) app_store_enter(app);

    int top = L->top_y + L->top_h + 8;
    int bottom = L->footer_y - 8;
    int head_h = ui_clamp(L->h * 9 / 100, 34, 62);
    /* Same rail position/height as settings, slightly narrower. */
    int side_w = ui_clamp(L->w * 19 / 100, 88, 240);
    int side_h = L->rail_h;
    if (side_h < 60) side_h = 60;

    lv_obj_t *side = ui_rect(app->ui.root, L->rail_x, L->rail_y,
                             side_w, side_h, 14, PAL.card);
    lv_obj_set_style_bg_opa(side, 135, 0);
    lv_obj_set_scroll_dir(side, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(side, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    int rh = L->rail_row_h;
    int sy = 4;
    for (int i = 0; i < 6; i++, sy += rh + 4)
        side_row(app, side, 4, sy, side_w - 8, rh, i, side_views[i]);

    lv_obj_t *div = ui_rect(side, 12, sy + 3, side_w - 24, 2, 1, PAL.text3);
    lv_obj_set_style_bg_opa(div, 220, 0);
    sy += 10;
    for (int i = 6; i < STORE_SIDE_VIEWS; i++, sy += rh + 4)
        side_row(app, side, 4, sy, side_w - 8, rh, i, side_views[i]);

    int set_y = side_h - rh - 6;
    if (set_y < sy + 4) set_y = sy + 4;
    lv_obj_t *div2 = ui_rect(side, 12, set_y - 8, side_w - 24, 2, 1, PAL.text3);
    lv_obj_set_style_bg_opa(div2, 220, 0);
    side_row(app, side, 4, set_y, side_w - 8, rh,
             STORE_SIDE_SETTINGS, -1);

    int side_gap = ui_clamp(L->w * 12 / 1000, 6, 24);
    int x = L->rail_x + side_w + side_gap;
    int w = L->w - x - L->pad;
    if (w < 80) w = 80;
    int store_title_h = ui_text_height(FONT_LG);
    int store_small_h = ui_text_height(FONT_XS);
    ui_label(app->ui.root, "应用商店", FONT_LG, PAL.text, x, top, w / 2,
             store_title_h);
    char count[128];
    snprintf(count, sizeof(count), "%d 个应用", app->store_filter_count);
    ui_label(app->ui.root, count, FONT_XS, PAL.text3, x + w / 2, top + 2,
             w / 2, store_small_h);
    int store_hint_y = top + store_title_h;
    if (!app->store_settings) {
        if (app->store_refreshing)
            ui_label(app->ui.root, "正在刷新...", FONT_XS, PAL.accent2,
                     x, store_hint_y, w, store_small_h);
        else if (app->store_error[0])
            ui_label(app->ui.root, app->store_error, FONT_XS, PAL.warning,
                     x, store_hint_y, w, store_small_h);
        else if (app->store_search[0])
            ui_label(app->ui.root, app->store_search, FONT_XS, PAL.text2,
                     x, store_hint_y, w, store_small_h);
        else
            ui_label(app->ui.root, "按 X 搜索", FONT_XS, PAL.text3,
                     x, store_hint_y, w, store_small_h);
    }

    if (app->store_settings) {
        int py = top + head_h;
        int ph = bottom - py;
        if (ph < 80) ph = 80;
        lv_obj_t *panel = ui_rect(app->ui.root, x, py, w, ph, 14, PAL.card);
        lv_obj_set_style_bg_opa(panel, 120, 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(PAL.border), 0);
        lv_obj_set_style_border_opa(panel, 80, 0);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        int set_title_h = ui_text_height(FONT_MD);
        ui_label(panel, "商店设置", FONT_MD, PAL.text, 16, 10, w - 110,
                 set_title_h);
        int n = store_source_count(app);
        int cur = store_source_current(app);
        int rh = ui_clamp(app->layout.settings_row_h, 36, 60);
        if (30 + rh + 10 + 22 + n * (rh + 6) + 24 > ph)
            rh = ui_clamp((ph - 86) / (n + 1), 30, 60);
        int type_y = 30;
        lv_obj_t *type_row = ui_rect(panel, 12, type_y, w - 24, rh, 10,
                                     app->store_settings_focus == 0
                                         ? PAL.accent : PAL.card_soft);
        lv_obj_set_style_bg_opa(type_row,
                                app->store_settings_focus == 0 ? 44 : 30, 0);
        lv_obj_set_style_border_width(type_row,
                                      app->store_settings_focus == 0 ? 1 : 0, 0);
        lv_obj_set_style_border_color(type_row, lv_color_hex(PAL.accent), 0);
        lv_obj_set_style_border_opa(type_row, 180, 0);
        lv_obj_add_flag(type_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(type_row, dropdown_open_clicked,
                            LV_EVENT_CLICKED, app);
        if (app->store_settings_focus == 0)
            ui_set_focus_ring(type_row, true, PAL.accent);
        int type_text_h = ui_text_height(FONT_SM);
        ui_label(type_row, "软件包类型", FONT_SM, PAL.text,
                 14, (rh - type_text_h) / 2, w - 180, type_text_h);
        int dd_w = ui_clamp(w * 26 / 100, 110, 240);
        int dd_x = w - 24 - dd_w;
        int dd_h = rh - 8;
        if (dd_h < 28) dd_h = 28;
        lv_obj_t *type_box = ui_rect(panel, dd_x, type_y + (rh - dd_h) / 2,
                                     dd_w, dd_h, 8,
                                     app->store_settings_focus == 0
                                         ? PAL.accent : PAL.card);
        lv_obj_set_style_bg_opa(type_box, 42, 0);
        lv_obj_set_style_border_width(type_box, 1, 0);
        lv_obj_set_style_border_color(type_box, lv_color_hex(PAL.border), 0);
        lv_obj_add_flag(type_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(type_box, dropdown_open_clicked,
                            LV_EVENT_CLICKED, app);
        int ttx = 12;
        int arrow_w = 26;
        lv_obj_t *type_label = ui_label(type_box, store_type_label(app),
                                        FONT_SM, PAL.text, ttx,
                                        (dd_h - ui_text_height(FONT_SM)) / 2,
                                        dd_w - ttx - arrow_w,
                                        ui_text_height(FONT_SM));
        lv_label_set_long_mode(type_label, LV_LABEL_LONG_DOT);
        ui_label_center(type_box, LV_SYMBOL_DOWN, FONT_XS, PAL.accent2,
                        dd_w - arrow_w, (dd_h - ui_text_height(FONT_XS)) / 2,
                        arrow_w, ui_text_height(FONT_XS));

        int heading_y = type_y + rh + 10;
        ui_label(panel,
                 app->config.store_pkg_type == 1 ? "Flatpak 软件源" : "Pacman 软件源",
                 FONT_XS, PAL.accent2, 16, heading_y, w - 32, 16);
        int rows_y = heading_y + 22;
        for (int i = 0; i < n; i++) {
            int ry = rows_y + i * (rh + 6);
            bool focus = app->store_settings_focus == i + 1;
            bool active = cur == i;
            lv_obj_t *row = ui_rect(panel, 12, ry, w - 24, rh, 10,
                                    focus ? PAL.accent : PAL.card_soft);
            lv_obj_set_style_bg_opa(row, focus ? 44 : (active ? 60 : 30), 0);
            lv_obj_set_style_border_width(row, active || focus ? 1 : 0, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(
                active ? PAL.success : PAL.accent), 0);
            lv_obj_set_style_border_opa(row, active ? 160 : 120, 0);
            if (focus) {
                ui_set_focus_ring(row, true, PAL.accent);
                lv_obj_scroll_to_view(row, LV_ANIM_ON);
            }
            int ny = rh >= 40 ? 6 : (rh - 16) / 2;
            int mirror_h = ui_text_height(FONT_SM);
            ui_label(row, store_source_label(app, i), FONT_SM,
                     focus ? PAL.text : PAL.text2, 14, ny, w - 120,
                     mirror_h);
            if (rh >= 40) {
                int url_h = ui_text_height(FONT_XS);
                lv_obj_t *ul = ui_label(row, store_source_url(app, i), FONT_XS,
                                        PAL.text3, 14, 26, w - 100, url_h);
                lv_label_set_long_mode(ul, LV_LABEL_LONG_DOT);
            }
            if (active)
                ui_label(row, "当前", FONT_XS, PAL.success,
                         w - 24 - 56, (rh - 16) / 2, 48, 16);
            StoreMirrorCtx *ctx = xmalloc(sizeof(*ctx));
            ctx->app = app;
            ctx->index = i;
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, mirror_row_clicked, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(row, mirror_ctx_free, LV_EVENT_DELETE, ctx);
        }
        ui_label(panel,
                 app->config.store_pkg_type == 1
                     ? "切换后刷新 Flatpak 应用列表；官方源置顶，默认国内源"
                     : "应用后会重写 /etc/pacman.d/mirrorlist 并执行 pacman -Syy",
                 FONT_XS, PAL.text3, 16, rows_y + n * (rh + 6) + 4,
                 w - 32, 16);

        if (app->store_dropdown_open) {
            int dd_y = type_y + rh + 2;
            int dd_item_h = rh;
            lv_obj_t *dd = ui_rect(panel, dd_x, dd_y, dd_w,
                                   dd_item_h * 2 + 8, 10, PAL.surface);
            lv_obj_set_style_bg_opa(dd, 255, 0);
            lv_obj_set_style_border_width(dd, 1, 0);
            lv_obj_set_style_border_color(dd, lv_color_hex(PAL.accent), 0);
            lv_obj_set_style_shadow_width(dd, 16, 0);
            lv_obj_set_style_shadow_color(dd, lv_color_hex(0x000000), 0);
            lv_obj_set_style_shadow_opa(dd, 100, 0);
            const char *types[2] = {"Pacman", "Flatpak"};
            int tr_h = dd_item_h - 4;
            if (tr_h < 24) tr_h = 24;
            for (int i = 0; i < 2; i++) {
                bool focus = app->store_dropdown_focus == i;
                bool active = app->config.store_pkg_type == i;
                lv_obj_t *tr = ui_rect(dd, 4, 4 + i * dd_item_h,
                                       dd_w - 8, tr_h, 7,
                                       focus ? PAL.accent : PAL.card_soft);
                lv_obj_set_style_bg_opa(tr, focus ? 48 : 24, 0);
                if (focus) ui_set_focus_ring(tr, true, PAL.accent);
                int th = ui_text_height(FONT_SM);
                lv_obj_t *tl = ui_label(tr, types[i], FONT_SM,
                                        active ? PAL.accent2 : PAL.text,
                                        10, (tr_h - th) / 2,
                                        dd_w - 48, th);
                lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
                if (active)
                    ui_label(tr, LV_SYMBOL_OK, FONT_XS, PAL.success,
                             dd_w - 36,
                             (tr_h - ui_text_height(FONT_XS)) / 2,
                             24, ui_text_height(FONT_XS));
                StoreMirrorCtx *dctx = xmalloc(sizeof(*dctx));
                dctx->app = app;
                dctx->index = i;
                lv_obj_add_flag(tr, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(tr, type_row_clicked, LV_EVENT_CLICKED, dctx);
                lv_obj_add_event_cb(tr, mirror_ctx_free, LV_EVENT_DELETE, dctx);
            }
        }
    } else {
        int hero_h = (app->store_category == 0 && app->store_filter_count > 0)
                     ? ui_clamp(bottom - top - 100, 64, 132) : 0;
        int first = hero_h > 0 ? 1 : 0;
        app->store_first = first;
        int cols = w >= 680 ? 3 : (w >= 430 ? 2 : 1);
        app->store_cols = cols;
        int gap = ui_clamp(L->h * 17 / 1000, 6, 12);
        int card_w = (w - gap * (cols - 1)) / cols;
        int card_h = ui_clamp(L->h * 20 / 100, 70, 120);
        int row_step = card_h + gap;
        int viewport_y = top + head_h;
        int viewport_h = bottom - viewport_y;
        if (viewport_h < 0) viewport_h = 0;
        int rows_full = viewport_h / row_step;
        if (rows_full < 1) rows_full = 1;
        int total_cards = app->store_filter_count - first;
        if (total_cards < 0) total_cards = 0;
        int rows_total = (total_cards + cols - 1) / cols;
        int hero_block = hero_h > 0 ? hero_h + 10 : 0;
        int rows_height = rows_total > 0 ? rows_total * row_step - gap : 0;
        int content_h = hero_block + rows_height;
        app->store_visible_rows = rows_full;
        int start_row = app->store_view_row;
        int max_start = rows_total - rows_full;
        if (max_start < 0) max_start = 0;
        if (start_row < 0) start_row = 0;
        if (start_row > max_start) start_row = max_start;
        app->store_view_row = start_row;

        if (app->store_filter_count == 0) {
            ui_label(app->ui.root,
                     app->store_refreshing ? "正在读取软件源..." :
                     (app->store_error[0] ? app->store_error : "没有找到应用"),
                     FONT_MD, PAL.text3, x, viewport_y + 10, w, 24);
        } else {
            lv_obj_t *viewport = ui_rect(app->ui.root, x, viewport_y, w,
                                         viewport_h, 0, PAL.card);
            lv_obj_set_style_bg_opa(viewport, 0, 0);
            lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
            lv_obj_add_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *content = ui_rect(viewport, 0, 0, w,
                                        content_h > viewport_h ? content_h
                                                               : viewport_h,
                                        0, PAL.card);
            lv_obj_set_style_bg_opa(content, 0, 0);
            lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

            if (hero_h > 0) {
                StorePackage *p = &app->store_all.items[app->store_filter[0]];
                resolve_icon(app, p);
                lv_obj_t *hero = ui_rect(content, 0, 0, w, hero_h,
                                         14, PAL.card_soft);
                lv_obj_set_style_bg_opa(hero, 120, 0);
                lv_obj_set_style_border_width(hero, 1, 0);
                lv_obj_set_style_border_color(hero,
                                              lv_color_hex(PAL.border_focus), 0);
                lv_obj_set_style_border_opa(hero, 90, 0);
                if (!app->store_on_sidebar && app->store_focus == 0)
                    ui_set_focus_ring(hero, true, PAL.accent);
                int icon = ui_clamp(hero_h * 42 / 100, 38, 58);
                if (p->icon[0] && access(p->icon, R_OK) == 0) {
                    lv_obj_t *img = lv_image_create(hero);
                    lv_obj_remove_style_all(img);
                    lv_obj_set_pos(img, ui_scale_x(14),
                                   ui_scale_y((hero_h - icon) / 2));
                    lv_obj_set_size(img, ui_scale_x(icon), ui_scale_y(icon));
                    char *src = xmalloc(strlen(p->icon) + 3);
                    sprintf(src, "A:%s", p->icon);
                    lv_image_set_src(img, src);
                    free(src);
                } else {
                    lv_obj_t *box = ui_rect(hero, 14, (hero_h - icon) / 2,
                                            icon, icon, 10, PAL.elevated);
                    uint32_t sym_color = PAL.text3;
                    const char *sym = category_symbol(p, &sym_color);
                    ui_label_center(box, sym, FONT_XXL, sym_color,
                                    0, (icon - 28) / 2, icon, 30);
                }
                int hx = 14 + icon + 14;
                bool tag = hero_h >= 88;
                if (tag)
                    ui_label(hero, "精选应用", FONT_XS, PAL.accent2,
                             hx, 8, w - hx - 14, 16);
                int name_y = tag ? 28 : 8;
                lv_obj_t *ht = ui_label(hero, p->name, FONT_LG, PAL.text,
                                        hx, name_y, w - hx - 14, 28);
                lv_label_set_long_mode(ht, LV_LABEL_LONG_DOT);
                int foot_y = hero_h - 22;
                int desc_y = name_y + 30;
                int desc_h = foot_y - desc_y - 4;
                if (desc_h >= 10) {
                    lv_obj_t *hd = ui_label(hero, p->description, FONT_XS,
                                            PAL.text2, hx, desc_y,
                                            w - hx - 14, desc_h);
                    lv_label_set_long_mode(hd, LV_LABEL_LONG_DOT);
                }
                ui_label(hero, p->installed ? "已安装" : "A 查看安装",
                         FONT_XS, p->installed ? PAL.success : PAL.text3,
                         hx, foot_y, w - hx - 14, 16);
            }

            lv_obj_t *rows_layer = NULL;
            if (rows_height > 0) {
                rows_layer = ui_rect(content, 0, hero_block, w, rows_height,
                                     0, PAL.card);
                lv_obj_set_style_bg_opa(rows_layer, 0, 0);
                lv_obj_clear_flag(rows_layer, LV_OBJ_FLAG_SCROLLABLE);
            }

            g_store_viewport = viewport;
            g_store_rows_layer = rows_layer;
            g_store_app = app;
            g_store_first = first;
            g_store_cols = cols;
            g_store_total_cards = total_cards;
            g_store_row_step = ui_scale_y(row_step);
            g_store_card_w = card_w;
            g_store_card_h = card_h;
            g_store_gap = gap;
            g_store_hero_block = ui_scale_y(hero_block);
            g_store_visible_rows = viewport_h / row_step;
            if (g_store_visible_rows < 1) g_store_visible_rows = 1;

            if (rows_total > 0 && start_row >= rows_total)
                start_row = rows_total - 1;
            int win_first = start_row - STORE_SCROLL_BUFFER;
            int win_last = start_row + g_store_visible_rows + STORE_SCROLL_BUFFER;
            if (win_first < 0) win_first = 0;
            if (win_last >= rows_total) win_last = rows_total - 1;
            if (rows_layer) store_build_rows(win_first, win_last);
            lv_obj_add_event_cb(viewport, store_scroll_settled,
                                LV_EVENT_SCROLL_END, app);
            lv_obj_update_layout(viewport);
            int scroll_y = 0;
            if (!(hero_h > 0 && app->store_focus == 0))
                scroll_y = ui_scale_y(hero_block + start_row * row_step);
            g_store_programmatic_scroll = true;
            lv_obj_scroll_to_y(viewport, scroll_y, LV_ANIM_OFF);
            g_store_scroll_y = lv_obj_get_scroll_y(viewport);
            g_store_programmatic_scroll = false;
        }

    
        }

    if (app->store_settings) {
        if (app->store_dropdown_open)
            ui_footer_pill(app, "商店设置", 2,
                           "A 选择", "上下 选择", "左 侧栏", "B 关闭");
        else
            ui_footer_pill(app, "商店设置", store_source_count(app) + 1,
                           "A 选择", "上下 选择", "左 侧栏", "B 返回");
    } else
        ui_footer_pill(app, "应用商店", app->store_filter_count,
                       "A 打开", "上下左右 移动", "X 搜索", "B 返回");

    if (app->store_detail && app->store_detail_index >= 0 &&
        app->store_detail_index < app->store_filter_count) {
        StorePackage *p = &app->store_all.items[app->store_filter[app->store_detail_index]];
        int pw = L->w * 68 / 100;
        int ph = L->h * 70 / 100;
        int px = (L->w - pw) / 2;
        int py = (L->h - ph) / 2;
        lv_obj_t *shade = ui_rect(app->ui.root, 0, 0, L->w, L->h, 0, 0x000000);
        lv_obj_set_style_bg_opa(shade, 140, 0);
        lv_obj_t *panel = ui_rect(app->ui.root, px, py, pw, ph, 16, PAL.surface);
        lv_obj_set_style_bg_opa(panel, 250, 0);
        int dname_h = ui_text_height(FONT_XL);
        int did_h = ui_text_height(FONT_XS);
        ui_label(panel, p->name, FONT_XL, PAL.text, 20, 16, pw - 40, dname_h);
        ui_label(panel, p->id, FONT_XS, PAL.text3, 20, 16 + dname_h,
                 pw - 40, did_h);
        char meta[512];
        snprintf(meta, sizeof(meta), "%s  %s  %s  %lld KB", p->source,
                 p->version, p->license, p->size_kb);
        ui_label(panel, meta, FONT_XS, PAL.text2, 20,
                 16 + dname_h + did_h, pw - 40, did_h);
        int desc_y = 16 + dname_h + did_h * 2 + 10;
        lv_obj_t *d = ui_label(panel, p->description, FONT_SM, PAL.text2,
                               20, desc_y, pw - 40,
                               ph - desc_y - 64 > 20 ? ph - desc_y - 64 : 20);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        const char *act = !p->removable ? "受保护" :
                          p->update_available ? "A 更新" :
                          (p->installed ? "A 卸载" : "A 安装");
        lv_obj_t *actbtn = ui_rect(panel, 20, ph - 58, pw - 40, 28, 8, PAL.elevated);
        lv_obj_set_style_bg_opa(actbtn, 110, 0);
        if (p->removable) {
            lv_obj_add_flag(actbtn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(actbtn, detail_action_clicked,
                                LV_EVENT_CLICKED, app);
        }
        int act_h = ui_text_height(FONT_SM);
        ui_label(actbtn, act, FONT_SM, PAL.accent2, 8,
                 (28 - act_h) / 2, pw - 56, act_h);
        lv_obj_t *backbtn = ui_rect(panel, 20, ph - 26, 90, 20, 6, PAL.card_soft);
        lv_obj_set_style_bg_opa(backbtn, 80, 0);
        lv_obj_add_flag(backbtn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(backbtn, detail_back_clicked, LV_EVENT_CLICKED, app);
        int dback_h = ui_text_height(FONT_XS);
        ui_label_center(backbtn, "B 返回", FONT_XS, PAL.text3, 0,
                        (20 - dback_h) / 2, 90, dback_h);
    }

    if (app->store_task_running) {
        int pw = ui_clamp(L->w * 72 / 100, 260, 640);
        int ph = ui_clamp(L->h * 46 / 100, 180, 340);
        int px = (L->w - pw) / 2;
        int py = (L->h - ph) / 2;
        lv_obj_t *shade = ui_rect(app->ui.root, 0, 0, L->w, L->h, 0, 0x000000);
        lv_obj_set_style_bg_opa(shade, 150, 0);
        lv_obj_t *panel = ui_rect(app->ui.root, px, py, pw, ph, 16, PAL.surface);
        lv_obj_set_style_bg_opa(panel, 250, 0);
        pthread_mutex_lock(&g_store_lock);
        char stage[256], line[256], name[160];
        int pct = app->store_task_percent;
        snprintf(stage, sizeof(stage), "%s", app->store_task_stage);
        snprintf(line, sizeof(line), "%s", app->store_task_line);
        snprintf(name, sizeof(name), "%s", app->store_task_name);
        pthread_mutex_unlock(&g_store_lock);
        lv_obj_t *title = ui_label(panel, name[0] ? name : "软件包",
                                   FONT_SM, PAL.text, 18, 14, pw - 36, 22);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_t *st = ui_label(panel, stage[0] ? stage : "正在处理...",
                                FONT_XS, PAL.text2, 18, 42, pw - 36, 22);
        lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_LEFT, 0);
        if (pct >= 0) {
            char pb[32];
            snprintf(pb, sizeof(pb), "%d%%", pct);
            int pct_h = ui_text_height(FONT_XS);
            ui_label(panel, pb, FONT_XS, PAL.accent2, pw - 70, 16, 52, pct_h);
            lv_obj_t *bar = ui_rect(panel, 18, 72, pw - 36, 8, 4, PAL.elevated);
            int fill = (pw - 36) * pct / 100;
            if (fill > 0) ui_rect(bar, 0, 0, fill, 8, 4, PAL.accent2);
        }
        int out_y = pct >= 0 ? 88 : 72;
        int out_h = ph - out_y - 36;
        if (out_h < 24) out_h = 24;
        lv_obj_t *out = lv_obj_create(panel);
        lv_obj_remove_style_all(out);
        lv_obj_set_pos(out, ui_scale_x(18), ui_scale_y(out_y));
        lv_obj_set_size(out, ui_scale_x(pw - 36), ui_scale_y(out_h));
        lv_obj_set_style_pad_all(out, 0, 0);
        lv_obj_set_style_bg_color(out, lv_color_hex(PAL.card_soft), 0);
        lv_obj_set_style_bg_opa(out, 120, 0);
        lv_obj_set_scroll_dir(out, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(out, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(out, LV_OBJ_FLAG_SCROLLABLE);
        pthread_mutex_lock(&g_store_lock);
        int out_n = app->store_task_output_count;
        char outputs[200][256];
        for (int i = 0; i < out_n; i++)
            snprintf(outputs[i], sizeof(outputs[i]), "%s", app->store_task_output[i]);
        pthread_mutex_unlock(&g_store_lock);
        if (out_n == 0) {
            lv_obj_t *ol = ui_label(out, line[0] ? line : "等待输出...",
                                    FONT_XS, PAL.text3, 4, 3, pw - 44, 16);
            lv_obj_set_style_text_align(ol, LV_TEXT_ALIGN_LEFT, 0);
        } else {
            int line_step = ui_unscale_y(lv_font_get_line_height(FONT_XS)) + 1;
            if (line_step < 12) line_step = 12;
            for (int i = 0; i < out_n; i++) {
                lv_obj_t *ol = ui_label(out, outputs[i], FONT_XS, PAL.text3,
                                        4, i * line_step, pw - 44, line_step);
                lv_obj_set_style_text_align(ol, LV_TEXT_ALIGN_LEFT, 0);
                lv_label_set_long_mode(ol, LV_LABEL_LONG_DOT);
            }
            lv_obj_scroll_to_y(out, 100000, LV_ANIM_OFF);
        }
        lv_obj_t *cancel = ui_label(panel, "B 取消", FONT_XS, PAL.text3,
                                    18, ph - 26, pw - 36, 16);
        lv_obj_set_style_text_align(cancel, LV_TEXT_ALIGN_LEFT, 0);
    }

    if (app->store_search_edit) {
        int sw = ui_min(w * 80 / 100, 620);
        int sh = 94;
        int sx = x + (w - sw) / 2;
        int sy = top + head_h + 8;
        lv_obj_t *panel = ui_rect(app->ui.root, sx, sy, sw, sh, 14,
                                  PAL.surface);
        lv_obj_set_style_bg_opa(panel, 250, 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(PAL.accent), 0);
        lv_obj_set_style_shadow_width(panel, 20, 0);
        lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(panel, 110, 0);
        int title_h = ui_text_height(FONT_XS);
        ui_label(panel, "搜索应用", FONT_XS, PAL.accent2,
                 14, 8, sw - 28, title_h);
        int input_h = ui_text_height(FONT_MD) + 8;
        lv_obj_t *input = ui_rect(panel, 14, 28, sw - 28, input_h, 8,
                                  PAL.card);
        lv_obj_set_style_bg_opa(input, 140, 0);
        lv_obj_set_style_border_width(input, 1, 0);
        lv_obj_set_style_border_color(input, lv_color_hex(PAL.border_focus), 0);
        lv_obj_set_style_border_opa(input, 180, 0);
        char qbuf[160];
        snprintf(qbuf, sizeof(qbuf), "%s|", app->store_search);
        lv_obj_t *ql = ui_label(input,
                                app->store_search[0] ? qbuf : "输入应用名称…",
                                FONT_MD,
                                app->store_search[0] ? PAL.text : PAL.text3,
                                10, (input_h - ui_text_height(FONT_MD)) / 2,
                                sw - 48, ui_text_height(FONT_MD));
        lv_label_set_long_mode(ql, LV_LABEL_LONG_DOT);
        int hint_h = ui_text_height(FONT_XS);
        ui_label(panel, "A 确认    B 取消    退格删除", FONT_XS, PAL.text3,
                 14, sh - hint_h - 6, sw - 28, hint_h);
    }
}

static void store_ensure_focus_visible(App *app)
{
    int first = app->store_first;
    int cols = app->store_cols > 0 ? app->store_cols : 1;
    int visible = app->store_visible_rows > 0 ? app->store_visible_rows : 1;
    if (first && app->store_focus == 0) {
        app->store_view_row = 0;
        return;
    }
    int row = (app->store_focus - first) / cols;
    if (row < 0) row = 0;
    if (row < app->store_view_row) app->store_view_row = row;
    if (row >= app->store_view_row + visible)
        app->store_view_row = row - visible + 1;
    if (app->store_view_row < 0) app->store_view_row = 0;
}

void input_store(App *app, InputAction action, const char *text)
{
    if (app->store_task_running) {
        if (action == IN_CANCEL || action == IN_B || action == IN_START)
            app->store_task_cancel = true;
        return;
    }
    if (app->store_search_edit) {
        if (action == IN_TEXT && text) {
            size_t n = strlen(app->store_search);
            snprintf(app->store_search + n, sizeof(app->store_search) - n, "%s", text);
        } else if (action == IN_BACKSPACE) {
            size_t n = strlen(app->store_search);
            if (n) app->store_search[n - 1] = '\0';
        } else if (action == IN_CONFIRM) {
            app->store_search_edit = false;
            ui_set_input_active(false);
            external_osk_hide(app);
            rebuild_filter(app);
            app->store_focus = 0;
        } else if (action == IN_CANCEL || action == IN_B) {
            app->store_search_edit = false;
            ui_set_input_active(false);
            external_osk_hide(app);
            app->store_search[0] = '\0';
            rebuild_filter(app);
            app->store_focus = 0;
        }
        app_rebuild(app);
        return;
    }
    if (app->store_settings) {
        int n = store_source_count(app);
        int total = n + 1;
        if (app->store_dropdown_open) {
            if (action == IN_UP || action == IN_DOWN ||
                action == IN_SCROLL_UP || action == IN_SCROLL_DOWN) {
                app->store_dropdown_focus = app->store_dropdown_focus ? 0 : 1;
                audio_play(SND_NAV);
                app_rebuild(app);
            } else if (action == IN_CONFIRM || action == IN_A) {
                audio_play(SND_ENTER);
                int type = app->store_dropdown_focus;
                app->store_dropdown_open = false;
                store_switch_type(app, type);
                app->store_settings_focus = 1 + store_source_current(app);
                app_rebuild(app);
            } else if (action == IN_CANCEL || action == IN_B) {
                audio_play(SND_BACK);
                app->store_dropdown_open = false;
                app_rebuild(app);
            }
            return;
        }
        if (action == IN_UP || action == IN_SCROLL_UP) {
            app->store_settings_focus =
                (app->store_settings_focus - 1 + total) % total;
            audio_play(SND_NAV);
            app_rebuild(app);
        } else if (action == IN_DOWN || action == IN_SCROLL_DOWN) {
            app->store_settings_focus =
                (app->store_settings_focus + 1) % total;
            audio_play(SND_NAV);
            app_rebuild(app);
        } else if (action == IN_CONFIRM || action == IN_A) {
            audio_play(SND_ENTER);
            if (app->store_settings_focus == 0) {
                app->store_dropdown_open = true;
                app->store_dropdown_focus = app->config.store_pkg_type;
            } else {
                int idx = app->store_settings_focus - 1;
                store_source_save(app, idx);
                store_task_start(app, app->config.store_pkg_type == 1 ? 4 : 3,
                                 idx);
            }
            app_rebuild(app);
        } else if (action == IN_CANCEL || action == IN_B) {
            audio_play(SND_BACK);
            app->store_settings = false;
            app_rebuild(app);
        }
        return;
    }
    if (action == IN_X) {
        audio_play(SND_ENTER);
        app->store_search_edit = true;
        app->store_settings = false;
        app->store_on_sidebar = false;
        ui_set_input_active(true);
        external_osk_show(app);
        app_rebuild(app);
        return;
    }
    if (action == IN_Y) {
        audio_play(SND_NAV);
        store_refresh_start(app);
        app_rebuild(app);
        return;
    }
    if (action == IN_START) {
        audio_play(SND_ENTER);
        store_task_start(app, 2, 0);
        app_rebuild(app);
        return;
    }
    if (app->store_detail) {
        if (action == IN_CANCEL || action == IN_B) {
            audio_play(SND_BACK);
            app->store_detail = false;
            app_rebuild(app);
        } else if (action == IN_CONFIRM || action == IN_A) {
            audio_play(SND_ENTER);
            int kind = 0;
            if (app->store_detail_index >= 0 &&
                app->store_detail_index < app->store_filter_count) {
                StorePackage *p = &app->store_all.items[
                    app->store_filter[app->store_detail_index]];
                if (!p->removable) {
                    ui_toast(app, "该软件包受保护，不能通过商店修改");
                    return;
                }
                kind = (p->installed && !p->update_available) ? 1 : 0;
            }
            store_task_start(app, kind, app->store_detail_index);
            app_rebuild(app);
        }
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        /* 搜索完成后再按 B：清除搜索条件，回到当前分类/来源的完整应用列表，
         * 而不是继续停留在过滤结果里。 */
        if (app->store_search[0]) {
            audio_play(SND_BACK);
            app->store_search[0] = '\0';
            app->store_search_edit = false;
            app->store_detail = false;
            app->store_focus = 0;
            app->store_view_row = 0;
            app->store_on_sidebar = false;
            rebuild_filter(app);
            app_rebuild(app);
            return;
        }
        /* 右栏为空时 B 返回左栏，避免只能停在一个空页面。 */
        if (!app->store_on_sidebar && app->store_filter_count == 0) {
            audio_play(SND_BACK);
            app->store_on_sidebar = true;
            app->store_side_focus = side_index_for_category(app->store_category);
            app_rebuild(app);
        }
        return;
    }
    if (action == IN_LEFT || action == IN_RIGHT) {
        int cols = app->store_cols > 0 ? app->store_cols : 1;
        int first = app->store_first;
        audio_play(SND_NAV);
        if (app->store_on_sidebar) {
            if (action == IN_RIGHT) app->store_on_sidebar = false;
        } else if (action == IN_LEFT) {
            if (app->store_focus <= first) {
                app->store_on_sidebar = true;
                app->store_side_focus =
                    side_index_for_category(app->store_category);
            } else {
                int col = (app->store_focus - first) % cols;
                if (col == 0) app->store_on_sidebar = true;
                else app->store_focus--;
            }
        } else {
            if (app->store_focus < app->store_filter_count - 1 &&
                (app->store_focus < first ||
                 (app->store_focus - first) % cols < cols - 1))
                app->store_focus++;
        }
        app_rebuild(app);
        return;
    }
    if (action == IN_UP || action == IN_DOWN || action == IN_SCROLL_UP ||
        action == IN_SCROLL_DOWN) {
        int dir = (action == IN_UP || action == IN_SCROLL_UP) ? -1 : 1;
        int cols = app->store_cols > 0 ? app->store_cols : 1;
        audio_play(SND_NAV);
        int first = app->store_first;
        if (app->store_on_sidebar) {
            app->store_side_focus =
                (app->store_side_focus + dir + STORE_SIDE_COUNT) % STORE_SIDE_COUNT;
            side_preview(app, app->store_side_focus);
        } else if (app->store_filter_count > 0) {
            if (first && app->store_focus == 0) {
                if (dir > 0) app->store_focus = first;
            } else {
                int f = app->store_focus + dir * cols;
                if (first && dir < 0 && f < first)
                    f = 0; /* back up to the featured card */
                else if (f < 0)
                    f = 0;
                if (f >= app->store_filter_count)
                    f = app->store_filter_count - 1;
                if (f < 0) f = 0;
                app->store_focus = f;
            }
            store_ensure_focus_visible(app);
        }
        app_rebuild(app);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        if (app->store_on_sidebar) {
            audio_play(SND_ENTER);
            if (app->store_side_focus == STORE_SIDE_SETTINGS) {
                app->store_on_sidebar = false;
                app->store_settings = true;
                app->store_settings_focus = 1 + store_source_current(app);
                app->store_dropdown_open = false;
                app_rebuild(app);
                return;
            }
            app->store_category = side_views[app->store_side_focus];
            app->store_focus = 0;
            app->store_view_row = 0;
            rebuild_filter(app);
            app->store_on_sidebar = false;
        } else if (app->store_filter_count > 0) {
            app->store_detail_index = app->store_focus;
            app->store_detail = true;
        }
        app_rebuild(app);
    }
}
